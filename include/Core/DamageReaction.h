#pragma once

#include "Core/HitShakeMotion.h"
#include <array>
#include <cstdint>
#include <mutex>

namespace DietDrCamera::DamageReaction
{
    using Vector = HitShakeMotion::Vector;
    using Basis = HitShakeMotion::Basis;
    using Rotation = HitShake::Rotation;
    enum class Kind { Unarmed, Blade, Blunt, Arrow, Fire, Frost, Shock, Poison, Magic, Environment, HeavyBlade, HeavyBlunt,
        GiantClub, GiantSwipe, GiantStomp, CenturionAxe, CenturionHammer, Centurion,
        CreatureBite, CreatureClaw, CreatureHeavy, DragonBite, DragonWing, DragonTail,
        FrostAtronachSpike, FrostAtronachBlunt, HeavyClaw, HeavyBite, SmallBite,
        Tusk, HeavyTusk, Hoof, Ram, Tentacle, ConstructBlunt, MechanicalPierce,
        MechanicalBlade, HeavyProjectile, SpiritTouch, Spear, CreatureStomp, VenomSpit, Web, Drain, Force, Count };

    struct Tuning {
        float intensity = 0.0f;
        bool operator==(const Tuning&) const = default;
    };
    inline Tuning Sanitize(Tuning p)
    {
        p.intensity = HitShake::FiniteClamp(p.intensity, 0, 3, 0);
        return p;
    }
    struct Contact {
        enum class Origin { Physical, Effect, MagicImpact };
        Kind kind = Kind::Environment;
        Vector towardSource{};
        double time = 0;
        std::uint32_t attacker = 0, source = 0;
        bool sustained = false, power = false, blocked = false;
        Vector strikeDirection{}; // optional incoming world-space force, from a known attack animation
        Origin origin = Origin::Physical;
        std::uint32_t projectile = 0;
    };
    struct Trajectory {
        Vector towardSource{};
        double time = 0;
        std::uint32_t attacker = 0, source = 0;
    };

    inline bool IsHarmfulEffectTick(float amount, bool hostile, bool detrimental, bool affectsHealth)
    {
        // A detrimental stat modifier is not necessarily damage: block perks
        // apply negative movement-speed changes through this same callback.
        // The affected value identifies damage; its amount and the player's
        // current/maximum health never scale the reaction.
        return affectsHealth && std::isfinite(amount) && amount < 0 && (hostile || detrimental);
    }

    // Only values cross from damage callbacks to the camera thread.
    class Inbox
    {
    public:
        std::uint64_t Token() const { std::scoped_lock lock(mutex); return view < 0 ? 0 : generation; }
        bool SetView(int next)
        {
            std::scoped_lock lock(mutex);
            if (view == next) return false;
            view = next; ++generation; count = contextCount = receiptCount = 0;
            return true;
        }
        void Clear()
        {
            std::scoped_lock lock(mutex);
            view = -1; ++generation; count = contextCount = receiptCount = 0;
        }
        void Push(Contact hit, std::uint64_t token)
        {
            if (!std::isfinite(hit.time)) return;
            std::scoped_lock lock(mutex);
            if (!token || view < 0 || token != generation) return;
            // A missile contact, TESHitEvent and instantaneous value change
            // can all report the same spell. Keep receipts across camera drains.
            // A poison/elemental stream is separate from its initial impact.
            if (!hit.sustained && hit.origin != Contact::Origin::Physical) {
                for (std::size_t i = 0; i < (std::min)(receiptCount, receipts.size()); ++i) {
                    auto& prior = receipts[i];
                    if (prior.attacker != hit.attacker || std::abs(prior.time-hit.time) > .08) continue;
                    // An area stomp can name both its carrier spell and the
                    // explosion's stagger enchantment. Those describe the
                    // same force impulse even though the form IDs differ.
                    const bool linkedForce = hit.attacker && hit.kind == Kind::Force && prior.kind == Kind::Force &&
                        hit.origin == Contact::Origin::MagicImpact && prior.origin == Contact::Origin::MagicImpact;
                    if (prior.source != hit.source && !linkedForce) continue;
                    if (prior.projectile && hit.projectile && prior.projectile != hit.projectile) continue;
                    if (prior.origin == Contact::Origin::Effect && hit.origin == Contact::Origin::Effect &&
                        prior.kind != hit.kind) continue;
                    if (hit.projectile) prior.projectile = hit.projectile;
                    return;
                }
                receipts[receiptCount++ % receipts.size()] = hit;
            }
            // One spell can modify multiple actor values in the same engine
            // update. Identical notifications describe one incoming contact.
            if (!hit.sustained) for (std::size_t i = 0; i < count; ++i) {
                const auto& prior = contacts[i];
                if (!prior.sustained && prior.attacker == hit.attacker && prior.source == hit.source &&
                    prior.kind == hit.kind && std::abs(prior.time - hit.time) < 0.008) return;
            }
            // Streaming effects produce one contact per frame, independent of
            // damage magnitude and the number of engine updates in that frame.
            if (hit.sustained) for (std::size_t i = 0; i < count; ++i) {
                auto& prior = contacts[i];
                if (prior.sustained && prior.attacker == hit.attacker && prior.source == hit.source &&
                    prior.kind == hit.kind && std::abs(prior.time - hit.time) < 0.12) {
                    if (hit.time >= prior.time) prior = hit;
                    return;
                }
            }
            if (count < contacts.size()) contacts[count++] = hit;
        }
        void Describe(Trajectory context, std::uint64_t token)
        {
            std::scoped_lock lock(mutex);
            if (!token || view < 0 || token != generation) return;
            contexts[contextCount++ % contexts.size()] = context;
        }
        std::size_t Drain(double now, std::array<Contact, 64>& out)
        {
            std::scoped_lock lock(mutex);
            std::size_t size = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto hit = contacts[i];
                if (!std::isfinite(now) || now < hit.time || now - hit.time > 0.2) continue;
                const Trajectory* projectile = nullptr;
                for (std::size_t j = 0; j < (std::min)(contextCount, contexts.size()); ++j) {
                    const auto& context = contexts[j];
                    if (context.attacker != hit.attacker || std::abs(context.time - hit.time) > 0.08) continue;
                    if (hit.source && context.source && hit.source != context.source) continue;
                    if (!projectile || std::abs(context.time - hit.time) < std::abs(projectile->time - hit.time)) projectile = &context;
                }
                if (projectile && projectile->towardSource.Finite() && projectile->towardSource.Length() > 1e-4f)
                    hit.towardSource = projectile->towardSource;
                out[size++] = hit;
            }
            count = 0;
            return size;
        }
    private:
        mutable std::mutex mutex;
        int view = -1;
        std::uint64_t generation = 1;
        std::array<Contact, 64> contacts{};
        std::array<Trajectory, 32> contexts{};
        std::array<Contact, 32> receipts{};
        std::size_t count = 0, contextCount = 0, receiptCount = 0;
    };

    struct Character {
        float rise, weight, rebound, frequency, texture;
    };
    inline Character Shape(Kind kind)
    {
        switch (kind) {
        case Kind::Blade:       return {0.020f, 0.95f, 0.10f, 19, 0.25f};
        case Kind::Blunt:       return {0.042f, 1.15f, 0.24f,  9, 0.45f};
        case Kind::HeavyBlade:  return {0.036f, 1.40f, 0.20f, 14, 0.35f};
        case Kind::HeavyBlunt:  return {0.052f, 1.60f, 0.28f,  8, 0.50f};
        case Kind::Arrow:       return {0.015f, 0.80f, 0.08f, 23, 0.20f};
        case Kind::Fire:        return {0.035f, 0.80f, 0.08f, 13, 0.70f};
        case Kind::Frost:       return {0.065f, 0.85f, 0.18f,  7, 0.40f};
        case Kind::Shock:       return {0.012f, 0.80f, 0.05f, 27, 1.00f};
        case Kind::Poison:      return {0.090f, 0.45f, 0.04f,  4, 0.25f};
        case Kind::Magic:       return {0.045f, 0.80f, 0.12f, 11, 0.45f};
        case Kind::Environment: return {0.050f, 0.85f, 0.15f,  8, 0.25f};
        case Kind::GiantClub:       return {.095f, 2.10f, .12f,  5, .25f};
        case Kind::GiantSwipe:      return {.065f, 1.65f, .12f,  8, .25f};
        case Kind::GiantStomp:      return {.085f, 1.80f, .10f,  5, .35f};
        case Kind::CenturionAxe:    return {.045f, 1.65f, .10f, 13, .20f};
        case Kind::CenturionHammer: return {.080f, 1.95f, .14f,  6, .30f};
        case Kind::Centurion:       return {.070f, 1.75f, .12f,  7, .25f};
        case Kind::CreatureBite:    return {.028f, 0.85f, .08f, 16, .20f};
        case Kind::CreatureClaw:    return {.030f, 1.00f, .10f, 14, .20f};
        case Kind::CreatureHeavy:   return {.065f, 1.50f, .16f,  7, .30f};
        case Kind::DragonBite:      return {.065f, 1.80f, .12f,  7, .25f};
        case Kind::DragonWing:      return {.085f, 1.90f, .12f,  5, .30f};
        case Kind::DragonTail:      return {.080f, 2.00f, .14f,  6, .25f};
        // New creature contacts reuse the existing envelope and camera limits.
        // Weight describes the striking body/limb, never the enemy's health,
        // level, damage amount or difficulty multiplier.
        case Kind::FrostAtronachSpike: return {.045f, 1.65f, .10f, 13, .20f};
        case Kind::FrostAtronachBlunt: return {.080f, 1.95f, .14f,  6, .30f};
        case Kind::HeavyClaw:       return {.040f, 1.50f, .12f, 12, .25f};
        case Kind::HeavyBite:       return {.045f, 1.45f, .10f, 11, .25f};
        case Kind::SmallBite:       return {.022f, 0.55f, .06f, 19, .15f};
        case Kind::Tusk:            return {.040f, 1.15f, .12f, 12, .20f};
        case Kind::HeavyTusk:       return {.075f, 1.85f, .14f,  7, .25f};
        case Kind::Hoof:            return {.045f, 1.10f, .20f,  9, .25f};
        case Kind::Ram:             return {.055f, 1.15f, .18f,  8, .25f};
        case Kind::Tentacle:        return {.060f, 1.25f, .18f,  8, .30f};
        case Kind::ConstructBlunt:  return {.075f, 1.65f, .14f,  6, .30f};
        case Kind::MechanicalPierce:return {.023f, 0.95f, .08f, 19, .20f};
        case Kind::MechanicalBlade: return {.030f, 1.20f, .10f, 16, .25f};
        case Kind::HeavyProjectile: return {.026f, 1.35f, .12f, 17, .25f};
        case Kind::SpiritTouch:     return {.055f, 0.65f, .08f,  8, .30f};
        case Kind::Spear:           return {.025f, 0.95f, .08f, 18, .20f};
        case Kind::CreatureStomp:   return {.080f, 1.65f, .12f,  6, .30f};
        case Kind::VenomSpit:       return {.035f, 0.75f, .08f, 11, .25f};
        case Kind::Web:             return {.060f, 0.70f, .06f,  6, .15f};
        case Kind::Drain:           return {.050f, 0.70f, .08f,  8, .30f};
        case Kind::Force:           return {.060f, 1.15f, .12f,  7, .20f};
        default:               return {0.035f, 0.90f, 0.18f, 12, 0.30f};
        }
    }
    inline Rotation Direction(Vector toward, Basis camera, float weight, bool firstPerson)
    {
        Rotation neutral{0.65f, 0, 0};
        if (!toward.Finite() || toward.Length() < 1e-4f) return neutral;
        const auto local = camera.ToLocal(toward * (1.0f / toward.Length()));
        // Positive yaw turns the viewing ray left in both compositors.
        // Deflect along the incoming blow, away from a source on the right;
        // rear blows reverse the front impact's upward pitch cue.
        return {(1 - weight) * neutral.pitch + weight * (0.75f * local.y - 0.45f * local.z),
                weight * 0.85f * local.x,
                -weight * (firstPerson ? 0.10f : 0.30f) * local.x};
    }
    inline float Smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); }
    inline Rotation ContactDirection(const Contact& hit, Basis camera, bool firstPerson)
    {
        auto axes = Direction(hit.towardSource, camera, 1, firstPerson);
        const float length = hit.strikeDirection.Length();
        if (!hit.strikeDirection.Finite() || !std::isfinite(length) || length < 1e-4f) return axes;
        const auto force = camera.ToLocal(hit.strikeDirection*(1/length));
        // A known chop/sweep follows the striking limb, including vertical
        // compression. Depth still supplies a readable front/back cue.
        const float screen = std::clamp(std::sqrt(force.x*force.x + force.z*force.z), 0.0f, 1.0f);
        return {force.z + axes.pitch*(1-screen), -force.x,
            force.x*(firstPerson ? .18f : .30f)};
    }

    // Stream motion has its own continuous envelope and lower-frequency texture.
    // Shock is a fine vibration, fire a turbulent sway, frost a slower pressure.
    struct StreamCharacter { float attackRate, releaseRate, swayFrequency, sway, rippleFrequency, ripple; };
    inline StreamCharacter StreamShape(Kind kind)
    {
        switch (kind) {
        case Kind::Fire:   return {24, 14, 0.85f, 0.16f, 4.2f, 0.045f};
        case Kind::Frost:  return {14, 10, 0.40f, 0.18f, 2.2f, 0.025f};
        case Kind::Shock:  return {30, 17, 1.25f, 0.07f, 8.0f, 0.055f};
        case Kind::Poison: return {12, 10, 0.33f, 0.20f, 1.7f, 0.015f};
        default:          return {20, 13, 0.65f, 0.13f, 3.4f, 0.035f};
        }
    }

    class Mixer
    {
    public:
        void Clear() { pulses = {}; streams = {}; }
        void Add(Contact hit, Tuning tuning, Basis camera, bool firstPerson, double now)
        {
            tuning = Sanitize(tuning);
            if (tuning.intensity <= 0 || !std::isfinite(now) || !std::isfinite(hit.time) || now < hit.time || now - hit.time > 0.2) return;
            const auto shape = Shape(hit.kind);
            const auto axes = ContactDirection(hit, camera, firstPerson);
            const float gain = tuning.intensity * shape.weight *
                (hit.power ? 1.35f : 1.0f) * (hit.blocked ? 0.6f : 1.0f);
            if (hit.sustained) {
                Stream* stream = nullptr;
                for (auto& s : streams) if (s.active && s.attacker == hit.attacker && s.source == hit.source && s.kind == hit.kind) {
                    stream = &s; break;
                }
                if (!stream) {
                    for (auto& s : streams) if (!s.active) { stream = &s; break; }
                    if (!stream) return;
                    *stream = {};
                    stream->attacker = hit.attacker;
                    stream->source = hit.source;
                    stream->kind = hit.kind;
                    stream->seed = ++sequence;
                    stream->firstPerson = firstPerson;
                    stream->active = true;
                }
                if (hit.time < stream->lastContact) return;
                stream->lastContact = hit.time;
                stream->age = now - hit.time;
                stream->target = axes;
                stream->gain = gain;
                // A tick refreshes presence and direction, never the envelope,
                // phase or seed. Resuming during the tail stays continuous too.
                return;
            }
            float recentEnergy = 0;
            for (const auto& p : pulses) if (p.active && p.elapsed < 0.10f) recentEnergy += p.gain;
            for (auto& p : pulses) if (!p.active) {
                // Begin at the current pose even if dispatch was delayed; a
                // fresh event must not appear already at its recoil peak.
                p = {axes, shape, 0,
                    gain / std::sqrt(1 + recentEnergy), ++sequence, firstPerson, true};
                return;
            }
        }
        Rotation Advance(float dt, bool firstPerson)
        {
            if (!std::isfinite(dt) || dt < 0 || dt > 0.25f) { Clear(); return {}; }
            Rotation result{};
            for (auto& p : pulses) if (p.active) {
                p.elapsed += dt;
                const float duration = 0.4f * (p.shape.rise > 0.05f ? 1.2f : 1.0f);
                if (p.elapsed >= duration) { p.active = false; continue; }
                const float rise = (std::min)(p.shape.rise, duration * 0.25f);
                float clean = Smooth(p.elapsed / rise);
                if (p.elapsed > rise) {
                    const float t = Smooth((p.elapsed - rise) / (duration - rise));
                    clean = (1 - Smooth(t)) * (std::exp(-5 * t) - p.shape.rebound * std::sin(HitShake::kPi * t));
                }
                const float envelope = Smooth(p.elapsed / rise) * (1 - Smooth(p.elapsed / duration));
                const float phase = static_cast<float>(p.seed % 997) * 0.173f;
                const float texture = envelope * 0.3f * p.shape.texture * 0.14f;
                // Restore the quick impact/recovery in both views. First
                // person needs more directional travel, not a longer gesture.
                // Double pitch/yaw reach without amplifying roll or vibration.
                const float magnitude = p.gain * (p.firstPerson ? 0.100f : 0.045f);
                const float rollMagnitude = p.gain * (p.firstPerson ? 0.050f : 0.045f);
                const float textureMagnitude = p.gain * (p.firstPerson ? 0.014f : 0.024f);
                result.pitch += magnitude * p.axes.pitch * clean +
                    textureMagnitude * texture * HitShake::TextureWave(p.elapsed, p.shape.frequency, phase);
                result.yaw += magnitude * p.axes.yaw * clean +
                    textureMagnitude * texture * 0.65f * HitShake::TextureWave(p.elapsed, p.shape.frequency, phase + 2);
                result.roll += rollMagnitude * p.axes.roll * clean;
            }
            for (auto& s : streams) if (s.active) {
                const auto shape = StreamShape(s.kind);
                // Split exactly at the last-contact grace boundary, so a missed
                // callback or low frame rate cannot introduce a one-frame dip.
                const float held = static_cast<float>(std::clamp(kStreamHold - s.age, 0.0, static_cast<double>(dt)));
                AdvanceStream(s, held, s.gain, shape.attackRate);
                AdvanceStream(s, dt - held, 0, shape.releaseRate);
                s.age += dt;
                s.elapsed += dt;
                if (s.age > kStreamHold + 1.6) { s.active = false; continue; }
                const float phase = static_cast<float>(s.seed % 997) * 0.173f;
                const auto texture = [&](float offset) {
                    return shape.sway * StreamWave(s.elapsed, shape.swayFrequency, phase + offset) +
                        shape.ripple * StreamWave(s.elapsed, shape.rippleFrequency, phase + offset + 1);
                };
                // More sustained directional pressure, with the same smooth
                // envelope and elemental sway/vibration as before.
                const float magnitude = s.firstPerson ? 0.010f : 0.012f;
                const float textureMagnitude = s.firstPerson ? 0.005f : 0.009f;
                result.pitch += magnitude * s.pressure.pitch + textureMagnitude * s.level * texture(0);
                result.yaw += magnitude * s.pressure.yaw + textureMagnitude * s.level * 0.65f * texture(2);
                result.roll += magnitude * s.pressure.roll + textureMagnitude * s.level * (s.firstPerson ? 0.025f : 0.10f) *
                    shape.sway * StreamWave(s.elapsed, shape.swayFrequency, phase + 4);
            }
            // Leave room for the intensity slider and heavy/power attacks.
            // These are overlap ceilings, not the size of an ordinary hit.
            const float pitchLimit = firstPerson ? 0.220f : 0.120f;
            const float yawLimit = firstPerson ? 0.200f : 0.110f;
            const float rollLimit = firstPerson ? 0.022f : 0.035f;
            result.pitch = pitchLimit * std::tanh(result.pitch / pitchLimit);
            result.yaw = yawLimit * std::tanh(result.yaw / yawLimit);
            result.roll = rollLimit * std::tanh(result.roll / rollLimit);
            return result;
        }
    private:
        struct Pulse {
            Rotation axes{}; Character shape{};
            float elapsed = 0, gain = 0;
            std::uint32_t seed = 0;
            bool firstPerson = false, active = false;
        };
        struct Stream {
            std::uint32_t attacker = 0, source = 0, seed = 0;
            Kind kind{};
            Rotation target{}, pressure{}, velocity{};
            double lastContact = 0, age = 0, elapsed = 0;
            float gain = 0, level = 0, levelVelocity = 0;
            bool firstPerson = false, active = false;
        };
        static constexpr double kStreamHold = 0.16;
        static void Follow(float& value, float& velocity, float target, float rate, float dt)
        {
            // Exact critically damped response: both position and velocity stay
            // continuous when the stream starts, stops or changes direction.
            if (dt <= 0) return;
            const float offset = value - target;
            const float step = (velocity + rate * offset) * dt;
            const float decay = std::exp(-rate * dt);
            value = target + (offset + step) * decay;
            velocity = (velocity - rate * step) * decay;
        }
        static void AdvanceStream(Stream& s, float dt, float gain, float rate)
        {
            Follow(s.level, s.levelVelocity, gain, rate, dt);
            // Greater displacement takes slightly longer to track a change
            // of direction, keeping its acceleration near the original feel.
            const float pressureRate = rate * (s.firstPerson ? 0.70f : 0.85f);
            Follow(s.pressure.pitch, s.velocity.pitch, gain * s.target.pitch, pressureRate, dt);
            Follow(s.pressure.yaw, s.velocity.yaw, gain * s.target.yaw, pressureRate, dt);
            Follow(s.pressure.roll, s.velocity.roll, gain * s.target.roll, pressureRate, dt);
        }
        static float StreamWave(double time, float frequency, float phase)
        {
            const double angle = 2.0 * HitShake::kPi * frequency * time;
            return static_cast<float>(0.65 * std::sin(angle + phase) + 0.35 * std::sin(angle * 0.618 + phase * 1.7));
        }
        std::array<Pulse, 16> pulses{};
        std::array<Stream, 16> streams{};
        std::uint32_t sequence = 0;
    };
}
