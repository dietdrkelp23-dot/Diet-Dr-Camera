#include "Core/ArcheryHitShake.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;
using namespace DietDrCamera::ArcheryHitShake;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

static void CheckDistanceAndView()
{
    using HitShakeMotion::Vector;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Require(ImpactDistance({10, 20, 30}, {310, 420, 30}) == 500 &&
            ImpactDistance({10, 20, 30}, {10, 20, 2030}) == 2000,
            "Projectile falloff ignores world position or vertical separation");
    Require(ImpactDistance({10010, -9980, 30}, {10310, -9580, 30}) == 500 &&
            ImpactDistance({0, 0, 0}, {400, -300, 0}) == 500,
            "World origin or heading changes distance attenuation");
    Require(ImpactDistance({nan, 0, 0}, {}) == 0 && DistanceScale(nan) == 1 && DistanceScale(-1) == 1 &&
            DistanceScale(inf) == .5f, "Invalid/very large distances corrupt strength or amplify the hit");
    Require(DistanceScale(0) == 1 && DistanceScale(100) > .998f &&
            std::abs(DistanceScale(1000) - .9f) < 1e-6f && std::abs(DistanceScale(2000) - .75f) < 1e-6f &&
            std::abs(DistanceScale(4000) - .6f) < 1e-6f, "Falloff loses the near hit or is too aggressive at range");
    float previous = 1;
    for (int distance = 1; distance <= 20000; ++distance) {
        const float current = DistanceScale(static_cast<float>(distance));
        Require(current <= previous && current >= .5f && previous - current < .0002f,
                "Distance falloff increases, switches off or snaps at a threshold");
        previous = current;
    }
    const HitShake::Tuning saved{3, .35f, .7f, .8f, true};
    for (bool fp : {false, true}) for (float distance : {0.0f, 1000.0f, 2000.0f, 4000.0f, 20000.0f, inf, nan}) {
        const auto p = ImpactTuning(saved, fp, distance);
        Require(p.speed == saved.speed && p.bounce == saved.bounce && p.texture == saved.texture && p.authored && saved.strength == 3,
                "Distance/POV scaling edits saved tuning or changes the waveform controls");
        Require(p.strength <= (fp ? 3.0f : 1.5f) && p.strength >= (fp ? 1.5f : .75f),
                "First-person calibration or far-hit floor is lost");
        HitShake::Mixer off;
        off.Arm(ImpactTuning({0, 1, 1, 1, true}, fp, distance), {1, 0, 0}, 0, fp, 0, HitShake::Onset::Contact);
        Require(off.Advance(.012f).pitch == 0, "Distance/first-person gain bypasses zero Strength");
        for (int fps : {30, 60, 120, 240}) {
            HitShake::Mixer mixer;
            for (int i = 0; i < 32; ++i) mixer.Arm(p, {1, .2f, .1f}, i, fp, 0, HitShake::Onset::Contact);
            for (int i = 0; i < fps * 4; ++i) {
                const auto r = mixer.Advance(1.0f / fps);
                Require(std::isfinite(r.pitch) && std::abs(r.pitch) <= .04f && std::abs(r.yaw) <= .025f && std::abs(r.roll) <= .007f,
                        "Stronger first-person burst bypasses the shared rotation limits");
            }
            Require(mixer.Advance(.01f).pitch == 0, "Scaled projectile burst does not settle");
        }
    }
    auto peak = [&](bool fp, float distance) {
        HitShake::Mixer mixer;
        mixer.Arm(ImpactTuning({3, .5f, .5f, 0}, fp, distance), {1, 0, 0}, 0, fp, 0, HitShake::Onset::Contact);
        return mixer.Advance(.02f).pitch;
    };
    Require(peak(true, 0) > 1.6f * peak(false, 0) && peak(true, 0) > .03f,
            "First-person max still has the weak half-strength projectile response after limiting");
    for (bool fp : {false, true})
        Require(peak(fp, 0) > peak(fp, 2000) && peak(fp, 2000) > peak(fp, 4000),
                "Mixer limits erase the requested distance falloff");
}

int main() try
{
    CheckDistanceAndView();
    for (const auto key : HitShake::kArcheryKeys)
        Require(HitShake::IsAttackKey(key), "Archery entry lacks Hit Shake controls");
    for (bool crossbow : {false, true}) for (bool sneak : {false, true}) for (bool zoom : {false, true}) {
        const auto key = HitShake::ArcheryKey(crossbow, sneak, zoom);
        const auto parents = HitShake::Parents(key);
        Require(key.starts_with(crossbow ? "weapons.crossbow." : "weapons.bow.") &&
                (key.find("sneak") != key.npos) == sneak && key.ends_with(zoom ? "zoom" : "draw"),
                "Shot context crosses weapon, sneak or zoom profiles");
        Require(parents[0] == key && parents[1] == HitShake::ArcheryKey(crossbow, sneak, false) &&
                parents[2] == HitShake::ArcheryKey(crossbow, false, false), "Archery fallback leaves its weapon family");
    }
    for (int slot = -1; slot <= 15; ++slot)
        Require(HitShake::IsArcherySlot(slot) == (slot == 2 || slot == 5 || slot == 6 || slot == 7),
                "Hit Shake leaks into idle/sprint/swim binding rows");
    Require(TargetEligible(true, false) && !TargetEligible(false, false) && !TargetEligible(true, true),
            "Scenery or player self-contact creates a delayed target-hit reaction");

    ShotBridge bridge;
    std::array<Impact, 32> out;
    Profile bow{{2, 0.25f, 0.75f, 0.5f, true}, 100, 3};
    bridge.Publish(bow, 10);
    Require(!bridge.Launch({1, 1}, 100, 10), "Unavailable camera tracks a projectile");
    bridge.SetView(0);
    bridge.Publish(bow, 10);
    Require(!bridge.Launch({1, 1}, 200, 10) && !bridge.Launch({0, 0}, 100, 10) && !bridge.Launch({1, 1}, 100, 10.3),
            "Unknown source or stale camera supplies launch settings");
    Require(bridge.Launch({1, 1}, 100, 10.01) && !bridge.Launch({1, 1}, 100, 10.02) && bridge.Drain(10.03, out) == 0,
            "Launching arms shake or repeated first updates duplicate a shot");

    // End the draw, leave sneak/zoom, change both item and settings while the
    // arrow is in flight. Only its own captured profile may supply the impact.
    Profile crossbow{{0, 1, 0, 1, true}, 200, 4};
    bridge.Publish(crossbow, 10.04);
    Require(bridge.Launch({2, 2}, 200, 10.05), "Second in-flight weapon cannot retain independent settings");
    bow.tuning = {};
    bridge.Publish({}, 11);
    Require(!bridge.Contact({1, 1}, 200, 50, {0, 3000, 0}, 11.1) &&
            bridge.Contact({1, 1}, 100, 50, {0, 3000, -200}, 11.1, 2000), "Projectile identity or source is ignored");
    Require(bridge.Drain(11.11, out) == 1 && out[0].profile.key == 3 && out[0].profile.tuning.strength == 2 &&
            out[0].profile.tuning.bounce == 0.75f && out[0].velocity.z == -200 && out[0].distance == 2000,
            "A delayed impact borrowed current equipment, pose or tuning");
    Require(!bridge.Contact({1, 1}, 100, 50, {}, 11.12) && !bridge.Contact({1, 1}, 100, 60, {}, 11.13),
            "Duplicate collision or ricochet rearms an already consumed shot");
    // Skyrim recycles projectile form IDs well before the old 30-second
    // duplicate window. Distinct reference generations are independent shots.
    bridge.Publish(crossbow, 11.14);
    Require(bridge.Launch({1, 101}, 200, 11.14) &&
            bridge.Contact({1, 101}, 200, 50, {}, 11.15) && bridge.Drain(11.16, out) == 1 &&
            out[0].projectile.reference == 101 && out[0].profile.tuning.strength == 0,
            "A recycled projectile form ID suppresses the next shot or borrows its previous tuning");
    // Also recognize an in-place projectile pool resetting its lifetime, while
    // repeated first updates of that same flight remain duplicates.
    bridge.Observe({1, 101}, 0.10f);
    Require(bridge.Launch({1, 101}, 200, 11.17, 0) &&
            !bridge.Launch({1, 101}, 200, 11.18, 0.01f), "Lifetime reset is ignored or repeated spawn ticks rearm a flight");
    Require(!bridge.Contact({1, 101}, 200, 50, {}, 11.16) &&
            bridge.Contact({1, 101}, 200, 50, {}, 11.19) && bridge.Drain(11.19, out) == 1,
            "A late callback from the pooled projectile's previous flight consumes the new shot");
    Require(bridge.Contact({2, 2}, 200, 50, {}, 11.2) && bridge.Drain(11.21, out) == 1 &&
            out[0].profile.tuning.strength == 0, "Zero-strength shot inherits a previous arrow's settings");
    HitShake::Mixer muted;
    muted.Arm(ImpactTuning(out[0].profile.tuning));
    Require(muted.Advance(0.05f).pitch == 0, "Zero Strength still shakes on impact");

    // Callback arrives after the camera sampled its time, before queue drain.
    // It must be retained, not misclassified as corrupt/future and discarded.
    bridge.Publish(crossbow, 12);
    Require(bridge.Launch({9, 9}, 200, 12) && !bridge.Contact({9, 9}, 200, 0, {}, 12.01) &&
            bridge.Contact({9, 9}, 200, 50, {}, 12.02, 4000), "Unknown terrain target consumes a tracked shot");
    Require(bridge.Drain(12.019, out) == 0 && bridge.Drain(12.025, out) == 1 &&
            out[0].time == 12.02 && out[0].launchedAt == 12 && out[0].distance == 4000 && bridge.Drain(12.03, out) == 0,
            "Contact arriving during camera work is lost, delayed more than one frame or replayed");

    // A POV/menu/load handoff discards both in-flight and queued contacts.
    for (int next : {1, -1}) {
        bridge.SetView(0);
        bow.tuning = {2, 0.25f, 0.75f, 0.5f, true};
        bridge.Publish(bow, 20);
        Require(bridge.Launch({3, 3}, 100, 20) && bridge.Launch({4, 4}, 100, 20), "Handoff setup failed");
        bridge.Contact({3, 3}, 100, 50, {}, 20.1);
        bridge.SetView(next);
        Require(!bridge.Contact({4, 4}, 100, 50, {}, 20.11) && bridge.Drain(20.11, out) == 0 &&
                !bridge.Launch({5, 5}, 100, 20.11), "Handoff replays a contact or keeps an old prepared profile");
    }

    // Bounds, abandoned flights, invalid clocks, stale contacts and multiple
    // arrows arriving on the same frame must stay safe and deterministic.
    bridge.SetView(0);
    bridge.Publish(bow, 30);
    for (std::uint32_t i = 1; i <= 64; ++i) Require(bridge.Launch({i, i}, 100, 30), "Flight pool is smaller than its bound");
    Require(!bridge.Launch({65, 65}, 100, 30), "Flight pool grew or evicted an active shot");
    for (std::uint32_t i = 1; i <= 64; ++i) bridge.Contact({i, i}, 100, 50, {}, 30.1);
    Require(bridge.Drain(30.11, out) == 32 && bridge.Drain(30.12, out) == 0 &&
            !bridge.Contact({64, 64}, 100, 50, {}, 30.13), "Impact pool overflows or a dropped duplicate is replayed");
    bridge.Publish(bow, 61);
    Require(bridge.Launch({65, 65}, 100, 61) && !bridge.Contact({1, 1}, 100, 50, {}, 61), "Expired flights do not release their slots");
    bridge.Contact({65, 65}, 100, 50, {}, 61.1);
    Require(bridge.Drain(62, out) == 0, "Stale impact survives a camera stall");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Require(!bridge.Launch({66, 66}, 100, nan) && !bridge.Contact({1, 1}, 100, 50, {}, nan) &&
            !bridge.Launch({66, 66}, 100, 60.9), "Invalid or reversed time is accepted");

    const auto tuning = ImpactTuning({2, 0.25f, 0.75f, 0.5f, true});
    Require(tuning.strength == 1 && tuning.speed == 0.25f && tuning.bounce == 0.75f && tuning.texture == 0.5f,
            "Gentler archery response changes the other feel controls");
    for (const HitShakeMotion::Vector travel : {HitShakeMotion::Vector{0, 3000, -200}, {200, 1000, 400}, {0, 0, 0}}) {
        const auto axes = ImpactAxes(travel, {}, true);
        HitShake::Mixer mixer;
        for (int i = 0; i < 32; ++i) mixer.Arm(tuning, axes, i, true, 0, HitShake::Onset::Contact);
        for (int i = 0; i < 120; ++i) {
            const auto value = mixer.Advance(1.0f / 60);
            Require(std::isfinite(value.pitch) && std::abs(value.pitch) <= 0.04f &&
                    std::abs(value.yaw) <= 0.025f && std::abs(value.roll) <= 0.007f, "Archery burst escapes shared rotation limits");
        }
        Require(mixer.Advance(0.01f).pitch == 0, "Archery impact does not settle");
    }
    for (float speed : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) for (float bounce : {0.0f, 0.5f, 1.0f}) {
        const HitShake::Tuning p{1, speed, bounce, 0.75f};
        const HitShake::Response contact(p, HitShake::Onset::Contact), melee(p);
        Require(contact.Clean(0) == 0 && contact.Clean(0.012f) > 0.999f && contact.Clean(0.008f) > 0.9f,
                "Slow archery tuning adds a perceptible onset delay");
        Require(std::abs(contact.Clean(0.012f - 1e-6f) - contact.Clean(0.012f + 1e-6f)) < 1e-5f,
                "Fast archery onset snaps into its peak");
        for (float age = contact.peakTime; age < contact.duration; age += 0.001f)
            Require(contact.Clean(age) == melee.Clean(age), "Fast onset changes authored return/bounce timing");
        for (int fps : {30, 60, 120, 144, 240}) for (int phase = 0; phase < 32; ++phase) {
            const float first = (phase + 1.0f) / (32 * fps);
            float peak = 0;
            for (float age = first; age < contact.duration; age += 1.0f / fps)
                peak = (std::max)(peak, contact.Clean(age));
            Require(peak > 0.90f, "Camera frame phase skips the shortened archery onset");
        }
    }
    // Tiny angular deviations around a forward shot used to normalize into a
    // full lateral kick or even reverse pitch. Keep small, continuous cues.
    for (bool fp : {false, true}) {
        HitShake::Rotation previous{1, 0, 0};
        for (int angle = -900; angle <= 900; ++angle) {
            const float a = angle * 0.001f;
            const HitShakeMotion::Vector travel{std::sin(a) * 0.1f, std::cos(a), std::sin(a)};
            const auto axes = ImpactAxes(travel, {}, fp);
            Require(axes.pitch > 0.96f && std::abs(axes.yaw) < 0.26f && std::abs(axes.roll) < 0.03f,
                    "Ballistic arc or small aim change reverses/dominates the recoil");
            if (angle > -900) Require(std::abs(axes.yaw - previous.yaw) < 0.001f &&
                std::abs(axes.pitch - previous.pitch) < 0.001f, "Archery direction has a discontinuity");
            previous = axes;
            const HitShakeMotion::Basis camera{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
            const auto rotated = ImpactAxes(camera.ToWorld(travel * 10000), camera, fp);
            Require(std::abs(rotated.pitch - axes.pitch) < 1e-6f && std::abs(rotated.yaw - axes.yaw) < 1e-6f,
                    "World heading or arrow speed changes screen-space recoil");
        }
    }
    HitShake::Mixer off;
    off.Arm({0, 1, 1, 1}, {1, 0, 0}, 0, false, 0, HitShake::Onset::Contact);
    Require(off.Advance(0.012f).pitch == 0, "Fast onset bypasses zero Strength");
    std::cout << "Archery hit shake checks passed (captured flights, duplicates, zero, bounds and handoffs)\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
