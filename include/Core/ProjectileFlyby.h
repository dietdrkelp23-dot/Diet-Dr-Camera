#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace DietDrCamera::ProjectileFlyby
{
    struct Point
    {
        float x = 0, y = 0, z = 0;
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
        Point operator-(Point p) const { return {x-p.x, y-p.y, z-p.z}; }
        Point operator+(Point p) const { return {x+p.x, y+p.y, z+p.z}; }
        Point operator*(float f) const { return {x*f, y*f, z*f}; }
        float Dot(Point p) const { return x*p.x + y*p.y + z*p.z; }
    };

    struct Identity
    {
        std::uint32_t form = 0, reference = 0;
        bool operator==(const Identity&) const = default;
        explicit operator bool() const { return form && reference; }
    };

    constexpr float kRange = 300.0f;
    constexpr float kMagicRange = 600.0f;
    // Cinematic event units, before the renderer's existing POV gains. The
    // first implementation compounded a low amplitude with a low rotation
    // weight, leaving even a point-blank pass almost invisible.
    constexpr float kIntensity = 1.0f;
    constexpr float kMagicIntensity = 1.5f;
    constexpr float kSpeed = 4.0f;
    constexpr float kRotation = 2.2f;
    constexpr float kTranslation = 0.25f;
    constexpr float kDriftJitter = 0.75f;
    constexpr float kRoughness = 0.60f;
    constexpr float kDecay = 0.22f;
    constexpr double kFreshness = 0.25;
    inline bool Fresh(double now, double then, double limit = kFreshness)
    {
        return std::isfinite(now) && std::isfinite(then) && now >= then && now-then <= limit;
    }
    inline bool InFlight(bool flying, Point velocity, bool firstTick = false, float minimumSpeed = 100.0f)
    {
        return flying && velocity.Finite() && (firstTick || velocity.Dot(velocity) >= minimumSpeed*minimumSpeed);
    }
    inline bool Eligible(bool npcOwned, bool archeryWeapon, bool hasAmmo, bool flying, Point velocity, bool firstTick = false)
    {
        return npcOwned && archeryWeapon && hasAmmo && InFlight(flying, velocity, firstTick);
    }
    struct MagicProjectile
    {
        // Native SPEL/ENCH and PROJ values; no spell names or load-order IDs.
        int casting = 0, delivery = 0, spellType = 0;
        std::uint16_t type = 0, flags = 0;
        bool Travels() const
        {
            // Fire-and-forget spells/staves/scrolls, including finite moving
            // cones such as Ice Storm. Beams, runes, sustained streams and
            // voice powers retain their existing cast/shout effects.
            const bool cast = casting == 1 || casting == 3;
            const bool spell = spellType == 0 || spellType == 2 || spellType == 3 ||
                spellType == 6 || spellType == 12 || spellType == 13;
            return cast && spell && delivery >= 2 && delivery <= 4 &&
                (type == 1 || type == 16) && (flags & ((1u << 0) | (1u << 11))) == 0;
        }
    };
    template <class Runtime>
    Point FlightVelocity(const Runtime& runtime)
    {
        // Projectile::GetLinearVelocity and native arrow integration use
        // linearVelocity. The separate velocity member is not a flight gate.
        return {runtime.linearVelocity.x, runtime.linearVelocity.y, runtime.linearVelocity.z};
    }
    inline float DistanceScale(float distance, float range = kRange)
    {
        if (!std::isfinite(distance) || !std::isfinite(range) || range <= 0 || distance < 0 || distance >= range) return 0;
        const float t = distance / range;
        const float smooth = 1.0f - t*t*(3.0f-2.0f*t);
        return smooth*smooth;
    }

    struct Passage { Point position; float distance = 0, strength = 0; };

    // Only an observed segment can cross the closest-approach plane. Never
    // extend velocity past a wall, or fire just because an archer releases.
    // Relative endpoints account for the player moving during the pass.
    inline std::optional<Passage> ClosestPass(Point from, Point to, Point listenerFrom, Point listenerTo, float range = kRange)
    {
        if (!from.Finite() || !to.Finite() || !listenerFrom.Finite() || !listenerTo.Finite()) return {};
        const auto travel = to-from;
        if (travel.Dot(travel) < 0.0001f) return {}; // a stuck arrow is not a flyby
        const auto a = from-listenerFrom, b = to-listenerTo, delta = b-a;
        const float length2 = delta.Dot(delta);
        if (!std::isfinite(length2) || length2 < 0.0001f) return {};
        const float t = -a.Dot(delta) / length2;
        if (!std::isfinite(t) || t < 0 || t > 1) return {};
        const auto offset = a+delta*t;
        const float distance = std::sqrt(offset.Dot(offset));
        const float strength = DistanceScale(distance, range);
        if (strength <= 0) return {};
        return Passage{from+travel*t, distance, strength};
    }

    struct Pass : Passage
    {
        Identity projectile;
        bool bolt = false;
        double time = 0;
        std::uint32_t shooter = 0;
    };

    // The camera publishes one value-only player snapshot. Physics callbacks
    // observe actual positions under this lock; no engine pointer crosses it.
    // Fixed storage also bounds volleys, deduplication and pause/load cleanup.
    class Tracker
    {
    public:
        explicit Tracker(float detectionRange = kRange) : range(detectionRange) {}
        float Range() const { return range; }
        bool Active() const { return active.load(std::memory_order_relaxed); }
        void Reset()
        {
            std::scoped_lock guard(lock);
            Clear();
        }
        // Returns true when the camera must also discard rendered tails.
        bool Publish(int nextView, std::uint32_t nextCell, Point position, double now)
        {
            std::scoped_lock guard(lock);
            if (nextView < 0 || nextView > 1 || !nextCell || !position.Finite() || !std::isfinite(now)) {
                Clear();
                return true;
            }
            const auto moved = position-listener;
            const bool reset = !active.load(std::memory_order_relaxed) || nextView != view || nextCell != cell ||
                !Fresh(now, published) || moved.Dot(moved) > 512.0f*512.0f;
            if (reset) shots = {};
            view = nextView; cell = nextCell; listener = position; published = now;
            active.store(true, std::memory_order_relaxed);
            return reset;
        }
        void Observe(Identity id, Point position, float livingTime, double now, bool bolt,
                     bool terminal = false, bool hitPlayer = false, std::uint32_t shooter = 0)
        {
            if (!Active() || !id || !position.Finite() || !std::isfinite(livingTime) || livingTime < 0) return;
            std::scoped_lock guard(lock);
            if (!Active() || !Fresh(now, published)) return;
            Shot* found = nullptr;
            Shot* free = nullptr;
            for (auto& shot : shots) {
                if (shot.id == id) { found = &shot; break; }
                if (!free && (!shot.id || !Fresh(now, shot.seen, 2.0))) free = &shot;
            }
            if (!found) {
                if (free) *free = {id, position, listener, livingTime, now, terminal};
                return; // first observation is never a release burst
            }
            auto& shot = *found;
            if (livingTime < shot.livingTime || !Fresh(now, shot.seen)) {
                shot = {id, position, listener, livingTime, now, terminal};
                return;
            }
            if (hitPlayer) { shot.done = true; shot.pending = false; }
            if (!shot.done) {
                if (const auto pass = ClosestPass(shot.position, position, shot.listener, listener, range)) {
                    shot.pass = {*pass, id, bolt, now, shooter};
                    shot.pending = true;
                    shot.done = true;
                }
            }
            shot.done |= terminal;
            shot.position = position; shot.listener = listener; shot.livingTime = livingTime; shot.seen = now;
        }
        std::size_t Drain(double now, std::array<Pass, 64>& out)
        {
            std::scoped_lock guard(lock);
            std::size_t count = 0;
            for (auto& shot : shots) if (shot.pending) {
                if (Active() && Fresh(now, published) && Fresh(now, shot.pass.time, 0.15)) out[count++] = shot.pass;
                shot.pending = false;
            }
            return count;
        }
    private:
        struct Shot
        {
            Identity id;
            Point position, listener;
            float livingTime = 0;
            double seen = 0;
            bool done = false, pending = false;
            Pass pass;
        };
        void Clear()
        {
            active.store(false, std::memory_order_relaxed);
            shots = {}; view = -1; cell = 0; published = 0;
        }
        const float range;
        std::atomic<bool> active{false};
        std::mutex lock;
        std::array<Shot, 64> shots{};
        Point listener;
        double published = 0;
        std::uint32_t cell = 0;
        int view = -1;
    };
}
