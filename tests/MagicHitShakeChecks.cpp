#include "Core/MagicHitShake.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;
using namespace DietDrCamera::MagicHitShake;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() try
{
    for (const auto school : HitShake::kMagicSchools) for (bool staff : {false, true})
        for (bool sneak : {false, true}) for (const auto cast : {"fire_and_forget", "ritual"}) {
            const std::string key = std::string(staff ? "staves." : "magic.") + std::string(school) +
                (sneak ? ".sneak." : ".") + cast;
            Require(HitShake::IsMagicKey(key) && HitShake::IsAttackKey(key), "Projectile school/cast entry lacks controls");
            for (const auto hand : {"left", "both", "right"})
                Require(HitShake::IsMagicKey(key + ".hand." + hand) == !staff, "Per-hand spell entry is misclassified");
        }
    for (const auto key : {"magic", "magic.unknown.ritual", "magic.destruction.concentration",
                          "staves.destruction.sneak.concentration", "magic.destruction.fire_and_forget.hand.wrong",
                          "weapons.magic", "mounts.horseback", "transformations.vampire_lord.concentration"})
        Require(!HitShake::IsMagicKey(key), "Invalid/stream/idle entry advertises projectile hit shake");
    Require(HitShake::Parents("transformations.vampire_lord.fire_and_forget")[1].empty(), "VL magic borrows melee hit tuning");
    for (bool fof : {false, true}) for (bool missile : {false, true}) for (bool continuous : {false, true}) for (bool hand : {false, true})
        Require(Eligible(fof, missile, continuous, hand) == (fof && missile && !continuous && hand),
                "Concentration, lingering or non-hand cast can create a hit impulse");
    Require(!ArcheryHitShake::TargetEligible(false, false) && !ArcheryHitShake::TargetEligible(true, true), "Scenery/self hits are eligible");

    ShotBridge bridge;
    std::array<Impact, 32> out;
    std::array<Profile, 4> profiles{};
    profiles[0] = {{2, .25f, .75f, .5f, true}, 100, 0, 0};
    profiles[1] = {{0, 1, 0, 1, true}, 200, 300, 2};
    bridge.Publish(profiles, 10);
    Require(!bridge.Launch({1, 1}, 100, 0, 0, 10), "Unavailable camera tracks magic");
    bridge.SetView(0); bridge.Publish(profiles, 10);
    Require(!bridge.Launch({1, 1}, 999, 0, 0, 10) && !bridge.Launch({1, 1}, 200, 999, 1, 10) &&
            !bridge.Launch({1, 1}, 100, 0, 3, 10) && !bridge.Launch({1, 1}, 100, 0, 0, 10.3), "Wrong source/stale/instant launch borrows settings");
    bridge.BeginCast(0, 10);
    Require(bridge.Launch({1, 1}, 100, 0, 0, 10.01) && !bridge.Launch({1, 1}, 100, 0, 0, 10.02), "Repeated launch is not deduplicated");
    Require(bridge.Launch({2, 2}, 100, 0, 0, 10.02), "Second projectile of a cast is not tracked");
    bridge.BeginCast(1, 10.02);
    Require(bridge.Launch({3, 3}, 200, 300, 1, 10.03), "Simultaneous staff launch is lost");
    const auto left = profiles[0];
    profiles[0].tuning.strength = 0; profiles[0].magic = 999;
    bridge.Publish(profiles, 10.1);
    Require(bridge.Contact({1, 1}, 100, 10, {0, 1000, 0}, 10.2, 2000), "Actor contact is lost after an equipment/settings change");
    Require(!bridge.Contact({1, 1}, 100, 11, {}, 10.21) && !bridge.Contact({2, 2}, 100, 12, {}, 10.22), "Callbacks/multi-projectile/explosion targets multiply one cast");
    Require(bridge.Contact({3, 3}, 200, 13, {}, 10.23, 4000), "Independent hand's zero-strength shot is lost");
    Require(bridge.Drain(10.225, out) == 1 && out[0].profile.tuning == left.tuning && out[0].profile.hand == 0 && out[0].distance == 2000,
            "In-flight left-hand profile is overwritten or future callback is consumed");
    Require(bridge.Drain(10.24, out) == 1 && out[0].profile.staff == 300 && out[0].profile.tuning.strength == 0 && out[0].distance == 4000,
            "Zero-strength staff/other hand is incorrectly inherited or mid-frame callback dropped");
    HitShake::Mixer muted;
    muted.Arm(ArcheryHitShake::ImpactTuning(out[0].profile.tuning), {1, 0, 0}, 0, false, 0, HitShake::Onset::Contact);
    Require(muted.Advance(.02f).pitch == 0, "Zero is not an exact off switch");

    // Independent rapid casts must not be treated as a multi-effect burst.
    profiles[0] = left; bridge.Publish(profiles, 11);
    bridge.BeginCast(0, 11);
    Require(bridge.Launch({4, 4}, 100, 0, 0, 11), "New cast lost");
    bridge.BeginCast(0, 11.01);
    Require(bridge.Launch({5, 5}, 100, 0, 0, 11.01), "Rapid cast lost");
    Require(bridge.Contact({4, 4}, 100, 1, {}, 11.02) && bridge.Contact({5, 5}, 100, 1, {}, 11.02) && bridge.Drain(11.03, out) == 2,
            "Two rapid casts hitting the same actor are collapsed");
    // Pool generation/lifetime changes are new projectiles, never old contacts.
    bridge.Observe({5, 5}, .5f); bridge.BeginCast(0, 11.04);
    Require(bridge.Launch({5, 5}, 100, 0, 0, 11.04, 0) && bridge.Contact({5, 5}, 100, 1, {}, 11.05), "In-place projectile pool reset is lost");
    bridge.BeginCast(0, 11.06);
    Require(bridge.Launch({5, 6}, 100, 0, 0, 11.06), "Recycled form with a new reference generation is lost");

    bridge.SetView(-1); bridge.SetView(1); bridge.Publish(profiles, 12);
    Require(!bridge.Contact({5, 6}, 100, 1, {}, 12) && bridge.Drain(12, out) == 0, "Menu/POV/load reset retains projectiles or pending pulses");
    profiles[1].magic = 100; profiles[1].staff = 0;
    profiles[2] = profiles[3] = profiles[0]; profiles[2].hand = profiles[3].hand = 1;
    bridge.Publish(profiles, 12);
    bridge.BeginCast(0, 12);
    Require(bridge.Launch({6, 6}, 100, 0, 0, 12, 0, true), "Dual left launch lost");
    bridge.BeginCast(1, 12.001);
    Require(bridge.Launch({7, 7}, 100, 0, 1, 12.001, 0, true) && bridge.Launch({8, 8}, 100, 0, 2, 12.002, 0, true), "Dual right/other launch lost");
    Require(bridge.Contact({7, 7}, 100, 1, {}, 12.05) && !bridge.Contact({6, 6}, 100, 2, {}, 12.06) &&
            !bridge.Contact({8, 8}, 100, 3, {}, 12.07), "Dual cast creates more than one impulse");
    Require(bridge.Drain(12.08, out) == 1 && out[0].profile.hand == 1, "Native dual flag fails to select the Both Hands snapshot");
    // Both Hands is also an editing category for simultaneous independent
    // casts. Only the projectile's native dual flag merges their cast groups.
    profiles[0].hand = profiles[1].hand = 1;
    bridge.Publish(profiles, 13);
    bridge.BeginCast(0, 13); bridge.BeginCast(1, 13);
    Require(bridge.Launch({9, 9}, 100, 0, 0, 13) && bridge.Launch({10, 10}, 100, 0, 1, 13) &&
            bridge.Contact({9, 9}, 100, 1, {}, 13.01) && bridge.Contact({10, 10}, 100, 1, {}, 13.01),
            "Both Hands profile collapses two independent casts");
    profiles[0].hand = 0; profiles[1].hand = 2;
    for (auto& p : profiles) p.ritual = true;
    bridge.Publish(profiles, 14); bridge.BeginCast(0, 14); bridge.BeginCast(1, 14);
    Require(bridge.Launch({11, 11}, 100, 0, 2, 14) && bridge.Contact({11, 11}, 100, 1, {}, 14.01),
            "Two-handed ritual on Other source is rejected without the native dual flag");
    Require(bridge.Drain(14.02, out) == 1 && out[0].profile.hand == 1 && out[0].profile.ritual,
            "Ritual Other source borrows a single-hand profile");

    bridge.SetView(-1); bridge.SetView(0);
    profiles[0] = left; profiles[1] = left; profiles[1].hand = 2; profiles[2] = {};
    bridge.Publish(profiles, 20);
    Require(!bridge.Launch({1, 1}, 100, 0, 2, 20), "Ambiguous other-source cast guesses the wrong hand");
    Require(bridge.Launch({1, 1}, 100, 0, 0, 20) && bridge.Launch({2, 2}, 100, 0, 0, 20.01), "Animations without cast events lose projectiles");
    Require(bridge.Contact({1, 1}, 100, 1, {}, 20.02) && !bridge.Contact({2, 2}, 100, 2, {}, 20.03), "Event-less burst multiplies shake");
    Require(bridge.Drain(21, out) == 0, "Stale contacts create late shake");
    Require(!bridge.Contact({1, 1}, 100, 1, {}, std::numeric_limits<double>::quiet_NaN()), "Invalid time is accepted");
    bridge.SetView(-1); bridge.SetView(0);
    for (std::uint32_t i = 1; i <= 64; ++i) {
        bridge.Publish(profiles, 30 + i * .01); bridge.BeginCast(0, 30 + i * .01);
        Require(bridge.Launch({i, i}, 100, 0, 0, 30 + i * .01), "Bounded cast capacity fills early");
        Require(bridge.Contact({i, i}, 100, 1, {}, 30 + i * .01) == (i <= 32), "Contact queue capacity differs");
    }
    bridge.Publish(profiles, 31); bridge.BeginCast(0, 31);
    Require(!bridge.Launch({65, 65}, 100, 0, 0, 31), "Full cast pool overwrites a live shot");
    bridge.Publish(profiles, 65); bridge.BeginCast(0, 65);
    Require(bridge.Launch({65, 65}, 100, 0, 0, 65), "Expired cast pool never recovers");
    std::cout << "Magic hit routing, burst ownership, native contacts, timing and lifecycle checks passed\n";
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
