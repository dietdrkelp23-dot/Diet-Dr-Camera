#include "Core/HitShake.h"
#include "Core/HitShakeMotion.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera::HitShake;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static float Magnitude(Rotation r) { return std::sqrt(r.pitch * r.pitch + r.yaw * r.yaw + r.roll * r.roll); }
static float Difference(Rotation a, Rotation b) { return Magnitude({a.pitch - b.pitch, a.yaw - b.yaw, a.roll - b.roll}); }
static Rotation Limited(Rotation r) { return {0.04f * std::tanh(r.pitch / 0.04f),
    0.025f * std::tanh(r.yaw / 0.025f), 0.007f * std::tanh(r.roll / 0.007f)}; }

static void CheckFirstPersonCameraAxes()
{
    using V = DietDrCamera::HitShakeMotion::Vector;
    auto cross = [](V a, V b) { return V{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; };
    auto turn = [&](V v, V angular) {
        const float angle = angular.Length();
        if (angle == 0) return v;
        const auto axis = angular * (1 / angle);
        return v * std::cos(angle) + cross(axis, v) * std::sin(angle) + axis * (axis.Dot(v) * (1 - std::cos(angle)));
    };
    auto close = [](V a, V b) { return (a - b).Length() < 2e-6f; };
    // NiCamera.world.rotate columns are forward/up/right, as used by the
    // production face-lock and view-ray code. Exercise its rendered ray, not
    // just the size of the pitch scalar before it reaches this basis.
    for (float yaw : {-2.8f, -1.2f, 0.0f, .8f, 2.4f}) for (float pitch : {-1.3f, -.4f, 0.0f, .6f, 1.3f})
        for (float bank : {-.3f, 0.0f, .3f}) {
            const V forward{std::sin(yaw)*std::cos(pitch), std::cos(yaw)*std::cos(pitch), -std::sin(pitch)};
            const V unbankedRight{std::cos(yaw), -std::sin(yaw), 0};
            const auto right = turn(unbankedRight, forward * bank);
            const auto up = cross(right, forward);
            auto rendered = [&](Rotation r, V ray) {
                const auto local = NiCameraRotationVector(r);
                return turn(ray, forward * local[0] + up * local[1] + right * local[2]);
            };
            for (float angle : {-.035f, -.007f, .007f, .035f}) {
                const auto pitched = rendered({angle, 0, 0}, forward);
                Require(close(pitched, forward * std::cos(angle) + up * std::sin(angle)) &&
                        (pitched - forward).Length() > .99f * std::abs(angle),
                        "First-person pitch becomes roll and leaves the viewing direction unchanged");
                Require(close(rendered({0, angle, 0}, forward), forward * std::cos(angle) - right * std::sin(angle)),
                        "First-person yaw is applied around the wrong camera axis");
                Require(close(rendered({0, 0, angle}, forward), forward) &&
                        close(rendered({0, 0, angle}, right), right * std::cos(angle) - up * std::sin(angle)),
                        "First-person roll changes aim or rolls in the wrong direction");
            }
            Require(close(rendered({}, forward), forward), "Zero hit shake changes the camera ray");
            Mixer hit;
            hit.Arm({3, .5f, .5f, 0}, {1, 0, 0}, 0, true, 0, Onset::Contact);
            const auto actual = hit.Advance(.02f);
            const auto ray = rendered(actual, forward);
            Require(up.Dot(ray - forward) > .03f && std::abs(right.Dot(ray - forward)) < 1e-6f,
                    "Maximum first-person projectile contact still renders as a barely visible roll");
        }
}

static void CheckMotion()
{
    namespace M = DietDrCamera::HitShakeMotion;
    auto alignment = [](M::Vector a, M::Vector b) { return a.Dot(b) / (a.Length() * b.Length()); };
    const auto fallback = AutomaticAxes(false, -1, false);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // Continuous directions, including diagonal cuts of arbitrary angle and
    // thrusts. Body/camera heading must not quantize or rotate the result.
    for (float angle : {-2.8f, -2.1f, -1.5f, -0.9f, -0.2f, 0.0f, 0.3f, 0.8f, 1.6f, 2.2f, 2.9f}) {
        const M::Vector velocity{std::cos(angle) * 300, 0, std::sin(angle) * 300};
        const auto axes = M::Axes(velocity, {}, fallback, false);
        Require(alignment({axes.yaw, 0, -axes.pitch}, velocity) > 0.99999f,
                "Measured diagonal slash is quantized or has the wrong screen direction");
        const auto fp = M::Axes(velocity, {}, fallback, true);
        Require(std::abs(Magnitude(axes) - 1) < 1e-6f && std::abs(Magnitude(fp) - 1) < 1e-6f &&
                std::abs(fp.roll) <= std::abs(axes.roll) * 0.26f,
                "Measured direction changes Strength or retains excessive FP roll");
        Require(Difference(axes, M::Axes(velocity * 10000, {}, fallback, false)) < 1e-6f,
                "Animation speed changes directional Strength");
        const M::Basis camera{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
        Require(Difference(axes, M::Axes(camera.ToWorld(velocity), camera, fallback, false)) < 1e-6f,
                "Camera heading changes the measured screen-space recoil");
    }
    Require(M::Axes({0, 200, 0}, {}, fallback, false).pitch > 0 &&
            M::Axes({0, 0, 200}, {}, fallback, false).pitch < 0 &&
            M::Axes({0, 0, -200}, {}, fallback, false).pitch > 0,
            "Thrust, rising and overhead cuts collapse to one direction");
    Require(Difference(fallback, M::Axes({}, {}, fallback, false)) == 0 &&
            Difference(fallback, M::Axes({nan, 0, 0}, {}, fallback, false)) == 0,
            "Unavailable motion loses the safe attack/hand direction");

    // Analytic curved trajectories at the actual event time, with the next
    // camera sample anywhere within one frame. Include fast sweeping attacks.
    for (int fps : {30, 60, 120, 144, 240}) for (float phase : {0.0f, 0.2f, 0.5f, 0.8f}) {
        for (float frequency : {-12.0f, -8.0f, -3.0f, 3.0f, 8.0f, 12.0f}) {
            M::History history;
            const double event = 0.18 + static_cast<double>(phase) / fps;
            for (int frame = 0; frame <= static_cast<int>(std::ceil(event * fps)); ++frame) {
                const double t = static_cast<double>(frame) / fps;
                const float a = frequency * static_cast<float>(t);
                history.Add({t, {}, {80 * std::cos(a), 30 * std::sin(a), 60 * std::sin(a)}});
            }
            const float a = frequency * static_cast<float>(event);
            const M::Vector expected{-80 * frequency * std::sin(a), 30 * frequency * std::cos(a), 60 * frequency * std::cos(a)};
            const auto result = history.At(event, {}, false);
            Require(result.valid && alignment(result.velocity, expected) > 0.998f,
                    "Curved swing tangent depends on frame rate or camera-frame phase");
        }
    }

    // Remove travel and uniform scale of the whole character while retaining
    // body turns: spinning attacks must still have measured weapon motion.
    M::History movingRoot;
    const M::Vector localVelocity{120, -75, 90};
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) / 60;
        const float yaw = t * 3;
        const M::Basis root{{std::cos(yaw), std::sin(yaw), 0}, {-std::sin(yaw), std::cos(yaw), 0}, {0, 0, 1}};
        const M::Vector origin{100000 + 400 * t, -200000 + 200 * t, 5000 + 100 * t};
        const M::Vector local{20 + localVelocity.x * t, 50 + localVelocity.y * t, 80 + localVelocity.z * t};
        const auto world = origin + root.ToWorld(local * 1.7f);
        const auto relative = M::RelativeToRoot(world, origin, 1.7f);
        movingRoot.Add({t, relative, relative});
    }
    const auto rooted = movingRoot.At(10.0 / 60, {}, false);
    const float at = 10.0f / 60;
    const M::Basis finalRoot{{std::cos(at * 3), std::sin(at * 3), 0}, {-std::sin(at * 3), std::cos(at * 3), 0}, {0, 0, 1}};
    const auto pointAtHit = finalRoot.ToWorld({20 + localVelocity.x * at, 50 + localVelocity.y * at, 80 + localVelocity.z * at});
    const auto expectedRooted = finalRoot.ToWorld(localVelocity) + M::Vector{-3 * pointAtHit.y, 3 * pointAtHit.x, 0};
    Require(rooted.valid && alignment(rooted.velocity, expectedRooted) > 0.999f,
            "Player travel/scale contaminates direction or a spinning attack loses its body rotation");

    M::History reverse;
    for (int i = 0; i <= 24; ++i) {
        const double t = static_cast<double>(i) / 120;
        const float x = 1000 * static_cast<float>((t - 0.1) * (t - 0.1));
        reverse.Add({t, {x, 0, 0}, {x, 0, 0}});
    }
    Require(reverse.At(0.075, {}, false).velocity.x < 0 && reverse.At(0.15, {}, false).velocity.x > 0,
            "Recovery/reversal motion overwrites an earlier contact's direction");

    M::History hitstop;
    for (int i = 0; i <= 12; ++i) {
        const double t = static_cast<double>(i) / 120;
        const float moving = static_cast<float>((std::min)(t, 0.075));
        hitstop.Add({t, {300 * moving, 0, -200 * moving}, {300 * moving, 0, -200 * moving}});
    }
    const auto stopped = hitstop.At(0.08, {}, false);
    Require(stopped.valid && alignment(stopped.velocity, {300, 0, -200}) > 0.9999f,
            "A repeated hitstop pose loses the contact's incoming direction");
    Require(!hitstop.At(0.3, {}, false).valid, "Stale hitstop motion is reused for another hit");

    M::History irregular;
    for (double t : {0.0, 0.012, 0.029, 0.036, 0.061, 0.076, 0.091, 0.117, 0.123, 0.147, 0.162, 0.180}) {
        const float a = static_cast<float>(t) * 6;
        irregular.Add({t, {}, {80 * std::cos(a), 50 * std::sin(a), 30 * std::sin(a)}});
        irregular.Add({t + 0.000001, {999, 999, 999}, {999, 999, 999}});
    }
    const auto variableFrame = irregular.At(0.17, {}, false);
    Require(variableFrame.valid && alignment(variableFrame.velocity,
                {-480 * std::sin(1.02f), 300 * std::cos(1.02f), 180 * std::cos(1.02f)}) > 0.998f,
            "Uneven frames or repeated camera calls corrupt the measured swing direction");

    // Rotating the blade about a moving grip must include rotation, not just
    // the wrist's linear travel. Nearby contacts can strike different portions.
    M::History blade;
    for (int i = 0; i <= 6; ++i) {
        const float t = static_cast<float>(i) / 60;
        blade.Add({t, {100 * t, 0, 0}, {100 * t, 80, 500 * t}});
    }
    const auto nearGrip = blade.At(0.1, {10, 16, 10}, true);
    const auto nearTip = blade.At(0.1, {10, 80, 50}, true);
    Require(nearGrip.valid && nearTip.valid && nearTip.velocity.z > nearGrip.velocity.z * 3.0f,
            "Rendered weapon rotation or contact position is ignored");

    M::History identity;
    identity.Bind(1, 2, 3, 1);
    identity.Add({1, {}, {0, 10, 0}}); identity.Add({1.02, {5, 0, 0}, {5, 10, 0}});
    Require(identity.At(1.02, {}, false).valid, "Native motion never becomes available");
    for (const auto change : {std::array<std::uintptr_t, 3>{1, 2, 4}, {1, 5, 4}, {6, 5, 4}}) {
        identity.Bind(change[0], change[1], static_cast<std::uint32_t>(change[2]), 1);
        Require(!identity.At(1.02, {}, false).valid, "Weapon, mesh or rig replacement keeps an old swing");
        identity.Add({1, {}, {0, 10, 0}}); identity.Add({1.02, {5, 0, 0}, {5, 10, 0}});
    }
    identity.Bind(6, 5, 4, 2);
    Require(!identity.At(1.02, {}, false).valid, "Rig/weapon scale handoff retains motion");
    identity.Add({1, {}, {0, 10, 0}}); identity.Add({1.02, {5, 0, 0}, {5, 10, 0}});
    identity.Clear();
    Require(!identity.At(1.02, {}, false).valid, "Menu/POV/load reset retains motion");
    identity.Add({1, {}, {0, 10, 0}}); identity.Add({1.02, {5, 0, 0}, {5, 10, 0}});
    identity.Add({1.03, {5000, 0, 0}, {5000, 10, 0}});
    Require(!identity.At(1.03, {}, false).valid, "Teleport or pose discontinuity creates a bogus direction");
    identity.Add({2, {}, {0, 10, 0}});
    Require(!identity.At(2, {}, false).valid, "Long frame gap preserves stale pose history");
    identity.Add({2.01, {nan, 0, 0}, {}});
    Require(!identity.At(2.01, {}, false).valid && !identity.At(nan, {}, false).valid,
            "Invalid poses/timestamps escape motion validation");
}

int main() try
{
    CheckFirstPersonCameraAxes();
    CheckMotion();
    for (const auto key : {"weapons.melee.attack", "weapons.melee.power_attack", "weapons.melee.sprint_attack",
        "weapons.melee.sprint_power_attack", "weapons.melee.sneak_attack", "weapons.melee.sneak_power_attack",
        "weapons.melee.power_attack.dir.standing", "weapons.melee.power_attack.dir.forward",
        "weapons.melee.power_attack.dir.back", "weapons.melee.power_attack.dir.left", "weapons.melee.power_attack.dir.right",
        "transformations.werewolf.attack", "transformations.werewolf.power_attack", "transformations.werewolf.sprint_power_attack",
        "transformations.vampire_lord.melee.attack", "transformations.vampire_lord.melee.power_attack",
        "mounts.horseback.melee.attack_left", "mounts.horseback.melee.attack_right"})
        Require(IsAttackKey(key), "An attack entry is missing hit shake controls/routing");
    for (const auto key : {"weapons.melee", "weapons.melee.sprint", "weapons.melee.sneak", "weapons.bow",
        "magic.destruction.concentration", "transformations.werewolf.roar", "animcam.5", "mounts.dragon_riding.attack"})
        Require(!IsAttackKey(key), "Hit shake appears on an unrelated entry");
    for (int slot : {0, 1, 2, 4, 15, -1}) Require(!IsMeleeSlot(slot), "Idle/non-melee binding slot is treated as an attack");
    Require(AttackKey(Family::Melee, false, true, false) == "weapons.melee.sprint_attack" &&
            AttackKey(Family::Melee, true, true, false) == "weapons.melee.sprint_power_attack" &&
            AttackKey(Family::Melee, false, false, true) == "weapons.melee.sneak_attack" &&
            AttackKey(Family::Melee, true, false, true) == "weapons.melee.sneak_power_attack",
            "Actual sprint/sneak contact routes to an ordinary attack");
    const std::string_view directions[]{"standing", "forward", "back", "left", "right"};
    for (int direction = 0; direction < 5; ++direction) {
        const auto key = AttackKey(Family::Melee, true, false, false, direction);
        Require(key.starts_with("weapons.melee.power_attack.dir.") && key.ends_with(directions[direction]),
                "Actual power attack loses its captured direction");
    }
    Require(AttackKey(Family::Werewolf, true, true, false) == "transformations.werewolf.sprint_power_attack" &&
            AttackKey(Family::VampireLord, true, false, false) == "transformations.vampire_lord.melee.power_attack" &&
            AttackKey(Family::Mounted, false, false, false, -1, -1) == "mounts.horseback.melee.attack_left" &&
            AttackKey(Family::None, true, false, false).empty(), "Transformation/mount context borrows generic melee");

    const auto axes = AutomaticAxes(false, -1, false);
    Require(Magnitude(Sample(Response{}, 0.1f, axes, 123, false)) == 0, "Defaults introduce shake into existing presets");
    for (float speed : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
        for (float bounce : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
            for (float texture : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const Response response({3.0f, speed, bounce, texture});
        const auto sample = [&](float t) { return Sample(response, t, axes, 521, false); };
        Require(Magnitude(sample(0)) == 0 && Magnitude(sample(response.duration)) == 0 &&
                Magnitude(sample(response.duration + 1)) == 0, "Impact has a discontinuous start/end or remains active");
        Require(std::abs(Magnitude(sample(response.peakTime)) - 3.0f * kPeakRadians) < 1e-6f,
                "Speed/Bounce/Texture changes the first-kick strength");
        float lowest = 0.0f, last = 0.0f;
        for (int frame = 0; frame <= 2000; ++frame) {
            const float t = static_cast<float>(frame) * response.duration / 2000.0f;
            const auto value = sample(t);
            Require(std::isfinite(Magnitude(value)) && Magnitude(value) <= 3.0f * kPeakRadians + 1e-6f,
                    "A shape control makes the response non-finite/unbounded");
            const float clean = response.Clean(t);
            lowest = (std::min)(lowest, clean);
            Require(std::abs(clean - last) < 0.025f, "Kick/spring/tail handoff snaps");
            last = clean;
        }
        Require(std::abs(lowest + 0.85f * bounce) < 0.002f,
                "Bounce does not produce its intended counter-movement at matched strength");
        Require(response.frequency * 1.19f < 12.0f, "Texture carriers alias at the supported 30 FPS minimum");
        constexpr float epsilon = 1e-5f;
        const float join = 2.0f * response.peakTime;
        const float incoming = (response.Clean(join) - response.Clean(join - epsilon)) / epsilon;
        const float outgoing = (response.Clean(join + epsilon) - response.Clean(join)) / epsilon;
        Require(std::abs(incoming - outgoing) < 0.2f, "Kick and rebound disagree on return velocity");
        Require(Magnitude(sample(epsilon)) < 1e-6f &&
                Magnitude(sample(response.duration - epsilon)) < 1e-6f,
                "Onset or final clearing creates a visible step");
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Require(Sanitize({nan, nan, nan, nan}) == Tuning{}, "Invalid tuning does not recover safe defaults");
    Require(Magnitude(Sample(Response({1, 1, 1, 1}), nan, axes, 0, false)) == 0, "Invalid time creates a NaN camera");
    for (float control = 0.0f; control < 1.0f; control += 0.1f) {
        Require(Response({1, control + 0.1f, 0.5f, 0}).duration < Response({1, control, 0.5f, 0}).duration,
                "Increasing Speed makes the response slower");
        Require(Response({1, 0.5f, control + 0.1f, 0}).duration > Response({1, 0.5f, control, 0}).duration,
                "Increasing Bounce cuts its settling short");
    }

    const Response smooth({1, 0.5f, 0.8f, 0}), rough({1, 0.5f, 0.8f, 1});
    float roughDifference = 0.0f, seedDifference = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f;
        Require(Difference(Sample(smooth, t, axes, 1, false), Sample(smooth, t, axes, 9751, false)) == 0,
                "Zero Texture introduces random motion");
        roughDifference += Difference(Sample(smooth, t, axes, 1, false), Sample(rough, t, axes, 1, false));
        seedDifference += Difference(Sample(rough, t, axes, 1, false), Sample(rough, t, axes, 9751, false));
    }
    Require(roughDifference > 0.01f && seedDifference > 0.01f, "Texture is cosmetic or repeats the identical rattle on every contact");
    for (int direction : {-1, 0, 1, 2, 3, 4}) {
        const auto tp = AutomaticAxes(false, direction, false), fp = AutomaticAxes(false, direction, true);
        Require(std::abs(Magnitude(tp) - 1.0f) < 1e-6f && std::abs(Magnitude(fp) - 1.0f) < 1e-6f &&
                std::abs(fp.roll) < std::abs(tp.roll) * 0.3f, "Automatic direction changes strength or leaves full roll in first person");
    }
    Require(AutomaticAxes(true, -1, false).yaw < 0 && AutomaticAxes(false, -1, false).yaw > 0 &&
            AutomaticAxes(false, 3, false).yaw < 0 && AutomaticAxes(true, 4, false).yaw > 0,
            "Hand or directional attack loses its automatic axis direction");
    Require(AutomaticAxes(false, 0, false).pitch > axes.pitch &&
            AutomaticAxes(false, 1, false).pitch > axes.pitch &&
            std::abs(AutomaticAxes(false, 3, false).yaw) > std::abs(axes.yaw) &&
            std::abs(AutomaticAxes(true, 4, false).yaw) > std::abs(axes.yaw),
            "Standing/forward and sideways power attacks lose their distinct native direction shaping");

    for (int fps : {30, 60, 120, 144, 240}) {
        // Point-time agreement alone misses a fast kick that falls entirely
        // between frames. Exercise every phase within a camera frame as well.
        for (float speed : {0.0f, 0.5f, 1.0f}) for (int phase = 0; phase < 32; ++phase) {
            const Response quick({1, speed, 0, 0});
            float peak = 0.0f;
            for (float t = static_cast<float>(phase) / (32.0f * fps); t < quick.duration; t += 1.0f / fps)
                peak = (std::max)(peak, Magnitude(Sample(quick, t, axes, 0, false)));
            Require(peak > 0.8f * kPeakRadians, "A fast hit loses most of its strength between low-FPS frames");
        }
        Mixer pulse;
        const Tuning p{1, 0.5f, 0.8f, 0.7f};
        pulse.Arm(p, axes, 823);
        Mixer::Rotation sampled;
        for (int i = 0; i < fps / 6; ++i) sampled = pulse.Advance(1.0f / fps);
        const auto expected = Limited(Sample(Response(p), static_cast<float>(fps / 6) / fps, axes, 823, false));
        Require(Difference(sampled, expected) < 1e-6f, "Hit timing/texture depends on frame rate");
        const auto before = pulse.Advance(0);
        for (int i = 0; i < 100; ++i) pulse.Arm({3, 1, 1, 1}, axes, 123);
        Require(Difference(pulse.Advance(0), before) == 0, "New crowd contact snaps an existing impact");
        const auto crowded = pulse.Advance(0.04f);
        Require(std::abs(crowded.pitch) <= 0.04f && std::abs(crowded.yaw) <= 0.025f && std::abs(crowded.roll) <= 0.007f,
                "Crowd contacts stack without a limit");
        pulse.Clear();
        Require(Magnitude(pulse.Advance(0.01f)) == 0, "Camera handoff retains an old impact");
        pulse.Arm(p, axes, 823, false, 0.0125f);
        Require(Difference(pulse.Advance(0), Limited(Sample(Response(p), 0.0125f, axes, 823, false))) < 1e-6f,
                "New contact is backdated by a camera frame instead of its timestamp");
        Require(Magnitude(pulse.Advance(0.3f)) == 0 && Magnitude(pulse.Advance(0.01f)) == 0,
                "Long frame gap replays stale camera motion");
    }

    // Snapshot tuning and deterministic diminishing returns for cleaves.
    Tuning editable{1, 0.5f, 0.5f, 0.5f};
    Mixer captured, reference;
    captured.Arm(editable, axes, 123); reference.Arm(editable, axes, 123);
    editable = {3, 1, 1, 1};
    Require(Difference(captured.Advance(0.1f), reference.Advance(0.1f)) == 0,
            "Editing a profile reshapes an already-running hit");
    Mixer single, cleave;
    single.Arm({1, 0.5f, 0.5f, 0});
    for (int i = 0; i < 4; ++i) cleave.Arm({1, 0.5f, 0.5f, 0});
    const float one = single.Advance(0.02f).pitch, many = cleave.Advance(0.02f).pitch;
    Require(many > one && many < 3.0f * one, "Cleaves either disappear or scale linearly per target");
    Mixer lateCleave;
    for (int i = 0; i < 4; ++i) lateCleave.Arm({1, 0.5f, 0.5f, 0}, {1, 0, 0}, 0, false, 0.09f);
    Mixer timelyCleave;
    for (int i = 0; i < 4; ++i) timelyCleave.Arm({1, 0.5f, 0.5f, 0});
    Require(Difference(lateCleave.Advance(0), timelyCleave.Advance(0.09f)) < 1e-6f,
            "Delayed contacts bypass diminishing cleave contributions");
    Mixer expired;
    for (int i = 0; i < 4; ++i) expired.Arm({3, 1, 0, 0}, axes, 0, false, 0.2f);
    expired.Arm({1, 0.5f, 0.5f, 0});
    Require(std::abs(expired.Advance(0.02f).pitch - one) < 1e-6f, "Expired contacts occupy the pool and drop a fresh hit");
    Mixer sideways, rollOnly;
    sideways.Arm({1, 0.5f, 0.5f, 0}, {0, 1, 0});
    rollOnly.Arm({1, 0.5f, 0.5f, 0}, {0, 0, 1});
    const auto yaw = sideways.Advance(0.05f), roll = rollOnly.Advance(0.05f);
    Require(yaw.pitch == 0 && yaw.yaw > 0 && roll.pitch == 0 && roll.roll > 0,
            "Non-pitch rotation is lost");
    for (float feel : {0.0f, 0.5f, 1.0f}) for (float recovery : {0.05f, 0.3f, 1.5f}) {
        const auto migrated = MigrateLegacy(2.0f, feel, recovery, true);
        float peak = 0.0f, rebound = 0.0f;
        for (int i = 0; i <= 10000; ++i) {
            const float u = static_cast<float>(i) / 10000.0f;
            const float value = 2.0f * 0.018f * (1.0f - u) * (1.0f - u) * std::sin(kPi * (1.0f + 2.0f * feel) * u);
            peak = (std::max)(peak, value); rebound = (std::max)(rebound, -value);
        }
        Require(std::abs(migrated.strength * kPeakRadians - peak) < 1e-6f &&
                std::abs(migrated.bounce * 0.85f - rebound / peak) < 1e-5f && migrated.texture == 0 && migrated.authored,
                "Legacy conversion loses first-kick strength, rebound or explicit authorship");
    }

    ContactQueue queue;
    std::array<Contact, 32> out;
    queue.Push({1, 2, 10});
    Require(queue.Drain(10, out) == 0, "Suppressed camera accepts a hit");
    queue.SetView(0);
    queue.Push({1, 2, 10}); queue.Push({1, 2, 10.001}); queue.Push({1, 3, 10.001}); queue.Push({4, 2, 10.001});
    Require(queue.Drain(10.01, out) == 3, "Duplicate filtering discards a distinct target/weapon");
    queue.Push({1, 2, 10.02, false, false, false, true});
    queue.Push({1, 2, 10.02, false, false, true, true});
    Require(queue.Drain(10.03, out) == 2, "Dual-wield hits with matching weapon form IDs collapse together");
    queue.Push({1, 2, 11}); queue.SetView(1);
    Require(queue.Drain(11.01, out) == 0, "POV change retains a pending hit");
    queue.Push({1, 2, 12}); queue.SetView(-1); queue.SetView(1);
    Require(queue.Drain(12.01, out) == 0, "Menu/load reset replays a hit");
    queue.Push({1, 2, 13}); queue.Push({1, 3, nan}); queue.Push({1, 4, 15});
    Require(queue.Drain(14, out) == 0, "Stale, invalid or future hits survive");
    for (std::uint32_t i = 1; i < 100; ++i) queue.Push({1, i, 20});
    Require(queue.Drain(20, out) == 32 && queue.Drain(20, out) == 0, "Contact queue is unbounded or replays drained hits");

    queue.Push({7, 8, 21, false, false, true, true});
    queue.Push({7, 8, 21.001, ContactPower(false, false, true), false, true, true});
    Require(queue.Drain(21.01, out) == 1 && out[0].power && out[0].left && out[0].time == 21,
            "Duplicate native reports lose power evidence or produce a second impact");
    queue.Push({7, 8, 24, ContactPower(false, false, true)});
    Require(queue.Drain(24.01, out) == 1 && out[0].power, "Captured graph power changes when the swing ends before drain");
    queue.Push({7, 8, 25, ContactPower(false, false, false)});
    Require(queue.Drain(25.01, out) == 1 && !out[0].power, "A normal contact retains previous power evidence");
    queue.Push({7, 8, 26.002});
    Require(queue.Drain(26.001, out) == 0 && queue.Drain(26.016, out) == 1 && out[0].time == 26.002,
            "A hit arriving after the camera clock snapshot is lost at drain");

    std::cout << "Hit shake checks passed (four feel controls, continuous layers, migration, frame rates, directions, crowd limits and handoffs)\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
