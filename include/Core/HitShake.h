#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace DietDrCamera::HitShake
{
    struct Tuning
    {
        float strength = 0.0f;
        float speed = 0.5f;     // weighty -> quick
        float bounce = 0.5f;    // firm return -> springy rebound
        float texture = 0.0f;   // smooth -> rattling
        bool authored = false;  // retains an explicit zero in optional attack directions
        bool operator==(const Tuning&) const = default;
    };

    inline float FiniteClamp(float value, float low, float high, float fallback)
    {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    }

    inline Tuning Sanitize(Tuning p)
    {
        p.strength = FiniteClamp(p.strength, 0.0f, 3.0f, 0.0f);
        p.speed = FiniteClamp(p.speed, 0.0f, 1.0f, 0.5f);
        p.bounce = FiniteClamp(p.bounce, 0.0f, 1.0f, 0.5f);
        p.texture = FiniteClamp(p.texture, 0.0f, 1.0f, 0.0f);
        return p;
    }

    inline bool HasTuning(const Tuning& p)
    {
        return p.authored || !(p == Tuning{});
    }

    // Shared by the menu and hit routing. Never infer eligibility from a
    // generic '.attack' suffix (dragon actions and animation cameras differ).
    inline constexpr std::array<std::string_view, 11> kMeleeKeys{
        "weapons.melee.attack", "weapons.melee.power_attack",
        "weapons.melee.sprint_attack", "weapons.melee.sprint_power_attack",
        "weapons.melee.sneak_attack", "weapons.melee.sneak_power_attack",
        "weapons.melee.power_attack.dir.standing", "weapons.melee.power_attack.dir.forward",
        "weapons.melee.power_attack.dir.back", "weapons.melee.power_attack.dir.left",
        "weapons.melee.power_attack.dir.right"
    };
    inline constexpr std::array<int, 11> kMeleeSlots{3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    inline constexpr std::array<std::string_view, 7> kOtherKeys{
        "transformations.werewolf.attack", "transformations.werewolf.power_attack",
        "transformations.werewolf.sprint_power_attack",
        "transformations.vampire_lord.melee.attack", "transformations.vampire_lord.melee.power_attack",
        "mounts.horseback.melee.attack_left", "mounts.horseback.melee.attack_right"
    };

    inline constexpr std::array<std::string_view, 10> kArcheryKeys{
        "weapons.bow.draw", "weapons.bow.sneak.draw", "weapons.bow.zoom", "weapons.bow.sneak.zoom",
        "weapons.crossbow.draw", "weapons.crossbow.sneak.draw", "weapons.crossbow.zoom", "weapons.crossbow.sneak.zoom",
        "mounts.horseback.archery.draw", "mounts.horseback.archery.zoom"
    };
    inline constexpr std::array<int, 4> kArcherySlots{2, 5, 6, 7};

    inline constexpr int ArcheryIndex(std::string_view key)
    {
        for (std::size_t i = 0; i < kArcheryKeys.size(); ++i)
            if (key == kArcheryKeys[i]) return static_cast<int>(i);
        return -1;
    }

    inline constexpr std::string_view ArcheryKey(bool crossbow, bool sneak, bool zoom)
    {
        return kArcheryKeys[(crossbow ? 4 : 0) + (sneak ? 1 : 0) + (zoom ? 2 : 0)];
    }

    inline constexpr bool IsArcherySlot(int slot)
    {
        for (const auto candidate : kArcherySlots) if (slot == candidate) return true;
        return false;
    }

    inline constexpr int MeleeIndex(std::string_view key)
    {
        for (std::size_t i = 0; i < kMeleeKeys.size(); ++i)
            if (key == kMeleeKeys[i]) return static_cast<int>(i);
        return -1;
    }

    inline constexpr std::array<std::string_view, 5> kMagicSchools{
        "alteration", "conjuration", "destruction", "illusion", "restoration"
    };

    inline constexpr bool IsMagicKey(std::string_view key)
    {
        if (key == "transformations.vampire_lord.fire_and_forget") return true;
        const bool staff = key.starts_with("staves.");
        if (!staff && !key.starts_with("magic.")) return false;
        key.remove_prefix(staff ? 7 : 6);
        const auto dot = key.find('.');
        if (dot == key.npos) return false;
        bool school = false;
        for (const auto name : kMagicSchools) school |= key.substr(0, dot) == name;
        if (!school) return false;
        key.remove_prefix(dot + 1);
        if (key.starts_with("sneak.")) key.remove_prefix(6);
        if (!staff) {
            for (const auto hand : {".hand.left", ".hand.both", ".hand.right"})
                if (key.ends_with(hand)) { key.remove_suffix(std::string_view(hand).size()); break; }
        }
        return key == "fire_and_forget" || key == "ritual";
    }

    inline constexpr bool IsAttackKey(std::string_view key)
    {
        if (MeleeIndex(key) >= 0) return true;
        if (ArcheryIndex(key) >= 0) return true;
        if (IsMagicKey(key)) return true;
        for (const auto candidate : kOtherKeys) if (key == candidate) return true;
        return false;
    }

    inline constexpr bool IsMeleeSlot(int slot)
    {
        for (const auto candidate : kMeleeSlots) if (slot == candidate) return true;
        return false;
    }

    enum class Family { None, Melee, Werewolf, VampireLord, Mounted };
    inline constexpr std::string_view AttackKey(Family family, bool power, bool sprint, bool sneak,
                                                 int direction = -1, int mountSide = 0)
    {
        if (family == Family::Werewolf) return power ? (sprint ? kOtherKeys[2] : kOtherKeys[1]) : kOtherKeys[0];
        if (family == Family::VampireLord) return power ? kOtherKeys[4] : kOtherKeys[3];
        if (family == Family::Mounted) return mountSide == -1 ? kOtherKeys[5] : mountSide == 1 ? kOtherKeys[6] : std::string_view{};
        if (family != Family::Melee) return {};
        if (sprint) return power ? kMeleeKeys[3] : kMeleeKeys[2];
        if (sneak) return power ? kMeleeKeys[5] : kMeleeKeys[4];
        if (power && direction >= 0 && direction < 5) return kMeleeKeys[6 + direction];
        return power ? kMeleeKeys[1] : kMeleeKeys[0];
    }

    inline constexpr std::array<std::string_view, 3> Parents(std::string_view key)
    {
        if (!IsAttackKey(key)) return {};
        if (IsMagicKey(key)) return {key, {}, {}};
        const int archery = ArcheryIndex(key);
        if (archery >= 8) return {key, kArcheryKeys[8], {}};
        if (archery >= 0) {
            const int base = archery >= 4 ? 4 : 0;
            return {key, kArcheryKeys[base + (archery % 2)], kArcheryKeys[base]};
        }
        if (key.starts_with("weapons.melee."))
            return {key, key.find("power_attack") != key.npos ? kMeleeKeys[1] : kMeleeKeys[0], kMeleeKeys[0]};
        if (key.starts_with("transformations.werewolf."))
            return {key, key.find("power_attack") != key.npos ? kOtherKeys[1] : kOtherKeys[0], kOtherKeys[0]};
        if (key.starts_with("transformations.vampire_lord.")) return {key, kOtherKeys[3], {}};
        return {key, {}, {}};
    }

    inline bool ContactPower(bool nativePower, bool attackPower, bool liveGraphPower)
    {
        // Some animation frameworks announce power swings in their graph
        // without setting the native hit flag. An absent flag is not proof
        // of a normal attack. Capture this evidence when the hit occurs.
        return nativePower || attackPower || liveGraphPower;
    }

    struct Contact
    {
        std::uint32_t source = 0;
        std::uint32_t target = 0;
        double time = 0.0;
        bool power = false;
        bool sneak = false;
        bool left = false;
        bool leftKnown = false;
    };

    class ContactQueue
    {
    public:
        bool SetView(int next)
        {
            std::scoped_lock guard(lock);
            if (view == next) return false;
            view = next;
            count = 0;
            return true;
        }
        void Push(Contact contact)
        {
            std::scoped_lock guard(lock);
            if (view < 0 || !contact.target || !std::isfinite(contact.time) || count == contacts.size()) return;
            // The same physical hit may be reported twice by a producer.
            // Distinct targets and weapon sources remain independent.
            for (std::size_t i = 0; i < count; ++i)
                if (contacts[i].target == contact.target && contacts[i].source == contact.source &&
                    contacts[i].left == contact.left &&
                    std::abs(contacts[i].time - contact.time) < 0.005) {
                    contacts[i].power = contacts[i].power || contact.power;
                    return;
                }
            contacts[count++] = contact;
        }
        std::size_t Drain(double now, std::array<Contact, 32>& out)
        {
            std::scoped_lock guard(lock);
            std::size_t n = 0;
            std::size_t pending = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const double age = now - contacts[i].time;
                if (std::isfinite(age) && age >= 0.0 && age <= 0.25) out[n++] = contacts[i];
                else if (std::isfinite(age) && age < 0.0 && age >= -0.25) contacts[pending++] = contacts[i];
            }
            count = pending;
            return n;
        }
    private:
        std::mutex lock;
        std::array<Contact, 32> contacts{};
        std::size_t count = 0;
        int view = -1;
    };

    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kPeakRadians = 0.014f;

    inline float SmoothStep(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    inline float Power8(float value)
    {
        value *= value;
        value *= value;
        return value * value;
    }

    // Compiled once per contact. A normalized, authored kick hands its exact
    // return velocity to an analytic spring; a separate band-limited reaction
    // supplies texture. Shape controls never change the first kick's size.
    enum class Onset { Swing, Contact };

    struct Response
    {
        Tuning tuning{};
        float peakTime = 0.0f;
        float riseTime = 0.0f;
        float omega = 0.0f;
        float decay = 0.0f;
        float reboundGain = 0.0f;
        float reboundDuration = 0.0f;
        float rumbleDuration = 0.0f;
        float frequency = 0.0f;
        float duration = 0.0f;

        explicit Response(Tuning p = {}, Onset onset = Onset::Swing) : tuning(Sanitize(p))
        {
            peakTime = 0.14f * std::pow(0.03f / 0.14f, tuning.speed);
            // Projectile feedback needs a prompt onset after flight. Retain
            // the broad peak and authored return so low FPS cannot skip it.
            riseTime = onset == Onset::Contact ? (std::min)(peakTime, 0.012f) : peakTime;
            omega = kPi / (2.0f * peakTime);
            const float damping = 0.85f - 0.67f * tuning.bounce;
            decay = damping * omega;
            const float peakPhase = std::atan(1.0f / damping);
            reboundGain = 0.85f * tuning.bounce * std::exp(damping * peakPhase) / std::sin(peakPhase);
            reboundDuration = tuning.bounce > 0.0f ? (0.65f + 1.75f * tuning.bounce) * 4.0f * peakTime : 0.0f;
            rumbleDuration = tuning.texture > 0.0f ? (3.0f + 5.0f * tuning.texture) * peakTime : 0.0f;
            // All three texture bands stay below 12 Hz, including at maximum
            // Speed/Texture, so a 30 FPS camera cannot fold them into slow sway.
            frequency = 4.0f + 4.0f * tuning.speed + 2.0f * tuning.texture;
            duration = (std::max)(2.0f * peakTime + reboundDuration, peakTime + rumbleDuration);
        }

        float Clean(float elapsed) const
        {
            if (!std::isfinite(elapsed) || elapsed <= 0.0f || elapsed >= duration) return 0.0f;
            if (elapsed < riseTime) {
                const float u = elapsed / riseTime;
                // A broad shoulder keeps even the quickest hit visible when
                // its peak falls between two 30 FPS camera updates.
                return 1.0f - Power8(1.0f - u * u);
            }
            if (elapsed < peakTime) return 1.0f;
            if (elapsed < 2.0f * peakTime) {
                const float u = (elapsed - peakTime) / peakTime;
                // Endpoint-matched polynomial: peak=1, zero velocity at the
                // peak, and the spring's velocity at the crossing (C1 handoff).
                const float endSlope = -reboundGain * omega * peakTime;
                return 1.0f - Power8(u * (2.0f - u)) + endSlope * Power8(u) * (u - 1.0f);
            }
            const float t = elapsed - 2.0f * peakTime;
            if (t >= reboundDuration) return 0.0f;
            const float fade = 1.0f - SmoothStep((t - 0.7f * reboundDuration) / (0.3f * reboundDuration));
            return -reboundGain * std::exp(-decay * t) * std::sin(omega * t) * fade;
        }

        float RumbleEnvelope(float elapsed) const
        {
            const float t = elapsed - peakTime;
            if (!std::isfinite(t) || t <= 0.0f || t >= rumbleDuration) return 0.0f;
            const float rise = SmoothStep(t / peakTime);
            const float fade = 1.0f - SmoothStep(t / rumbleDuration);
            return 0.5f * tuning.texture * rise * fade;
        }
    };

    // Format 6 had a quadratic-taper sine. Convert its measured first peak,
    // peak time and counter-movement to the new controls. This is a deliberate
    // approximation of the old shape, not a reinterpretation of old Feel.
    // The source TOML is not changed until the user saves it as format 7.
    inline Tuning MigrateLegacy(float strength, float feel, float recovery, bool authored)
    {
        strength = FiniteClamp(strength, 0.0f, 3.0f, 0.0f);
        feel = FiniteClamp(feel, 0.0f, 1.0f, 0.5f);
        recovery = FiniteClamp(recovery, 0.05f, 1.5f, 0.30f);
        float peak = 0.0f, rebound = 0.0f, peakTime = 0.0f;
        for (int i = 1; i < 1024; ++i) {
            const float u = static_cast<float>(i) / 1024.0f;
            const float value = (1.0f - u) * (1.0f - u) * std::sin(kPi * (1.0f + 2.0f * feel) * u);
            if (value > peak) { peak = value; peakTime = u * recovery; }
            rebound = (std::max)(rebound, -value);
        }
        Tuning p;
        p.strength = strength * 0.018f * peak / kPeakRadians;
        p.speed = std::log(peakTime / 0.14f) / std::log(0.03f / 0.14f);
        p.bounce = rebound / (peak * 0.85f);
        p.authored = authored;
        return Sanitize(p);
    }

    struct Rotation { float pitch = 0.0f; float yaw = 0.0f; float roll = 0.0f; };

    inline constexpr std::array<float, 3> NiCameraRotationVector(Rotation rotation)
    {
        // cameraRoot uses right/forward/up; its child NiCamera uses
        // forward/up/right. A delta post-multiplied onto the child must rotate
        // pitch around Z, yaw around Y and roll around X. Root axes here turn
        // the main pitch kick into roll and barely move the viewing direction.
        return {rotation.roll, rotation.yaw, rotation.pitch};
    }

    inline Rotation AutomaticAxes(bool left, int direction, bool firstPerson)
    {
        float side = left ? -1.0f : 1.0f;
        if (direction == 3 || direction == 4) side = direction == 3 ? -1.0f : 1.0f;
        Rotation axes{0.92f, side * 0.36f, side * (firstPerson ? 0.025f : 0.10f)};
        if (direction == 3 || direction == 4) { axes.pitch = 0.72f; axes.yaw = side * 0.68f; }
        if (direction == 0 || direction == 1) { axes.pitch = 1.0f; axes.yaw = side * 0.16f; }
        const float length = std::sqrt(axes.pitch * axes.pitch + axes.yaw * axes.yaw + axes.roll * axes.roll);
        axes.pitch /= length; axes.yaw /= length; axes.roll /= length;
        return axes;
    }

    inline float TextureWave(float time, float frequency, float phase)
    {
        return 0.5f * std::sin(2.0f * kPi * frequency * time + phase) +
               0.3f * std::sin(2.0f * kPi * frequency * 0.73f * time + phase * 1.7f) +
               0.2f * std::sin(2.0f * kPi * frequency * 1.19f * time + phase * 2.3f);
    }

    inline Rotation Sample(const Response& response, float elapsed, Rotation axes, std::uint32_t seed, bool firstPerson)
    {
        if (!std::isfinite(elapsed) || elapsed <= 0.0f || elapsed >= response.duration ||
            response.tuning.strength <= 0.0f) return {};
        const float clean = response.Clean(elapsed);
        // Reserve headroom instead of normalizing every frame: texture cannot
        // inflate the initial impact or introduce an amplitude discontinuity.
        const float texture = response.RumbleEnvelope(elapsed) * (1.0f - std::abs(clean));
        const float phase = static_cast<float>(seed % 65521) * (2.0f * kPi / 65521.0f);
        const float size = response.tuning.strength * kPeakRadians;
        return {
            size * (clean * axes.pitch + texture * 0.65f * TextureWave(elapsed, response.frequency, phase)),
            size * (clean * axes.yaw + texture * 0.55f * TextureWave(elapsed, response.frequency, phase + 2.1f)),
            size * (clean * axes.roll + texture * (firstPerson ? 0.06f : 0.25f) * TextureWave(elapsed, response.frequency, phase + 4.2f))
        };
    }

    class Mixer
    {
    public:
        using Rotation = HitShake::Rotation;
        void Clear() { for (auto& p : pulses) p.active = false; }
        void Arm(Tuning tuning, Rotation axes = {1.0f, 0.0f, 0.0f}, std::uint32_t seed = 0,
                 bool firstPerson = false, float age = 0.0f, Onset onset = Onset::Swing)
        {
            tuning = Sanitize(tuning);
            if (tuning.strength <= 0.0f) return;
            const float length = std::sqrt(axes.pitch * axes.pitch + axes.yaw * axes.yaw + axes.roll * axes.roll);
            if (!std::isfinite(length) || length < 1e-6f) axes = {1.0f, 0.0f, 0.0f};
            else { axes.pitch /= length; axes.yaw /= length; axes.roll /= length; }
            const Response response(tuning, onset);
            age = FiniteClamp(age, 0.0f, 0.25f, 0.0f);
            if (age >= response.duration) return;
            int recent = 0;
            for (const auto& p : pulses) if (p.active && std::abs(p.elapsed - age) < 0.06f) ++recent;
            // Full pool drops excess contacts instead of replacing a live
            // displacement with zero or adding unbounded crowd shake.
            for (auto& p : pulses) if (!p.active) {
                p = {response, axes, age,
                     1.0f / std::sqrt(1.0f + static_cast<float>(recent)), seed, firstPerson, true};
                return;
            }
        }
        Rotation Advance(float dt)
        {
            Rotation out;
            if (!std::isfinite(dt) || dt < 0.0f || dt > 0.25f) { Clear(); return out; }
            for (auto& p : pulses) if (p.active) {
                p.elapsed += dt;
                const auto value = Sample(p.response, p.elapsed, p.axes, p.seed, p.firstPerson);
                out.pitch += value.pitch * p.gain;
                out.yaw += value.yaw * p.gain;
                out.roll += value.roll * p.gain;
                if (p.elapsed >= p.response.duration) p.active = false;
            }
            // Smooth saturation preserves continuity when several contacts
            // arrive together, with a ceiling of about 2.3 degrees.
            out.pitch = 0.04f * std::tanh(out.pitch / 0.04f);
            out.yaw = 0.025f * std::tanh(out.yaw / 0.025f);
            out.roll = 0.007f * std::tanh(out.roll / 0.007f);
            return out;
        }
    private:
        struct Pulse {
            Response response{};
            Rotation axes{};
            float elapsed = 0.0f;
            float gain = 1.0f;
            std::uint32_t seed = 0;
            bool firstPerson = false;
            bool active = false;
        };
        std::array<Pulse, 4> pulses{};
    };
}
