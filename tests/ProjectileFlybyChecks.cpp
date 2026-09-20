#include "Core/ProjectileFlyby.h"
#include "Core/FirstPersonNoise.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera::ProjectileFlyby;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static bool Near(float a, float b) { return std::abs(a-b) < 0.0005f; }

static void MagicFlights()
{
    // SPEL -> MGEF -> PROJ fixtures from the locally audited game records.
    // Keep expected coverage explicit: a beam/stream is not a passing object.
    struct Fixture { const char* name; MagicProjectile magic; float speed; bool expected; };
    const Fixture fixtures[] = {
        {"Firebolt", {1,2,0,1,0}, 2500, true},
        {"Fireball", {1,2,0,1,0}, 2500, true},
        {"IceSpike", {1,2,0,1,0}, 2500, true},
        {"IcySpear", {1,2,0,1,0}, 2500, true},
        {"IceStorm", {1,2,0,16,512}, 512, true},
        {"Magelight", {1,2,0,1,512}, 512, true},
        {"crSpider01PoisonSpit", {1,2,0,1,520}, 2000, true},
        {"DLC2MQ02SeekerMouthSpell", {1,2,0,1,2}, 600, true},
        {"DLC2crLurkerSpit01", {1,2,0,1,514}, 1536, true},
        {"DLC1VampireDrain09Alt", {1,2,0,1,0}, 2500, true},
        {"LightningBolt", {1,2,0,4,512}, 20000, false},
        {"ChainLightning", {1,2,0,4,512}, 20000, false},
        {"Flames", {2,2,0,8,512}, 1000, false},
        {"Frostbite", {2,2,0,8,512}, 1000, false},
        {"crDwarvenCenturionSteamBreath", {2,2,11,8,512}, 0, false},
        {"FireRune", {1,4,0,2,6}, 1000, false},
        // Mod metadata, staves and scrolls use the same path without IDs.
        {"ModSlowMissile", {1,2,0,1,0}, 10, true},
        {"ModFastMissile", {1,2,0,1,0}, 12000, true},
        {"Staff", {1,2,12,1,0}, 2500, true},
        {"Scroll", {3,2,13,16,0}, 512, true},
        {"HomingTarget", {1,3,0,1,0}, 1000, true},
        {"ConcentrationMissile", {2,2,0,1,0}, 2500, false},
        {"ContinuousMissile", {1,2,0,1,2048}, 2500, false},
        {"HitscanMissile", {1,2,0,1,1}, 2500, false},
        {"VoiceCone", {1,2,11,16,512}, 512, false},
        {"SelfSpell", {1,0,0,1,0}, 2500, false},
        {"ContactSpell", {1,1,0,1,0}, 2500, false}
    };
    for (const auto& fixture : fixtures) {
        Require(fixture.magic.Travels() == fixture.expected, fixture.name);
        for (int view : {0,1}) for (int fps : {20,30,60,120,240}) for (float distance : {75.0f,450.0f,600.0f}) {
            Tracker tracker(kMagicRange);
            std::array<Pass,64> out;
            std::size_t passes = 0;
            for (int frame = 0; frame <= fps; ++frame) {
                const float time = float(frame)/fps;
                tracker.Publish(view,42,{},100+time);
                if (fixture.magic.Travels() && InFlight(true,{fixture.speed,0,0},frame == 0,1))
                    tracker.Observe({1,2},{fixture.speed*(time-.45f),distance,0},time,100+time,false,false,false,1234);
                const auto count = tracker.Drain(100+time,out);
                passes += count;
                if (count) Require(out[0].shooter == 1234 && Near(out[0].distance,distance) &&
                                   Near(out[0].strength,DistanceScale(distance,kMagicRange)), "Magic loses ownership or proximity across frame rates");
            }
            Require(passes == (fixture.expected && distance < kMagicRange ? 1u : 0u), fixture.name);
        }
    }
    Require(!InFlight(true,{},false,1) && !InFlight(false,{2500,0,0},false,1),
            "A stationary or destroyed spell is still considered in flight");
}

static void IndependentSourcesAndWaves()
{
    Tracker archery, magic(kMagicRange);
    std::array<Pass,64> out;
    archery.Publish(0,42,{},10); magic.Publish(0,42,{},10);
    archery.Observe({1,1},{-100,40,0},0,10,true);
    magic.Observe({2,2},{-100,60,0},0,10,false,false,false,1234);
    // A wave's contact with another actor is not its terminal segment.
    magic.Observe({2,2},{-50,60,0},.01f,10.01,false,false,false,1234);
    Require(magic.Drain(10.01,out) == 0, "A wave's surface contact creates an early flyby");
    magic.Observe({2,2},{100,60,0},.02f,10.02,false,false,false,1234);
    archery.Reset();
    Require(magic.Drain(10.02,out) == 1 && out[0].shooter == 1234, "Muting archery discards a magic pass");
    magic.Observe({2,2},{-100,60,0},.03f,10.03,false,false,false,1234);
    Require(magic.Drain(10.03,out) == 0, "A piercing wave or ricochet repeats the same magic pass");

    archery.Publish(0,42,{},20); magic.Publish(0,42,{},20);
    archery.Observe({1,1},{-100,40,0},0,20,true);
    archery.Observe({1,1},{100,40,0},.01f,20.01,true);
    magic.Reset();
    Require(archery.Drain(20.01,out) == 1 && out[0].bolt, "Muting magic discards an arrow/bolt pass");

    magic.Publish(0,42,{},30);
    magic.Observe({3,3},{-100,0,0},0,30,false,false,false,5678);
    magic.Observe({3,3},{10,0,0},.01f,30.01,false,false,false,5678);
    magic.Observe({3,3},{10,0,0},.01f,30.01,false,true,true,5678);
    Require(magic.Drain(30.01,out) == 0, "Known magic player contact doubles the hit reaction");
}

static void NativeArrowVelocity()
{
    // Native GetLinearVelocity reads +0x104/108/10C on 1.6.1170, which
    // CommonLib exposes as linearVelocity. The preceding velocity triple may
    // stay zero. A launch-only exception hid this mistake in the original test.
    struct Runtime { Point velocity{}, linearVelocity{}; } runtime;
    Tracker tracker;
    std::array<Pass,64> out;
    tracker.Publish(0,42,{},10);
    tracker.Observe({1,1},{-500,30,0},0,10,false);
    runtime.linearVelocity = {3000,0,0};
    std::size_t arrowPasses = 0;
    for (int frame = 1; frame <= 20; ++frame) {
        const float time = float(frame)/60;
        tracker.Publish(0,42,{},10+time);
        Require(Eligible(true,true,true,true,FlightVelocity(runtime),false),
                "A live native arrow with zero auxiliary velocity is rejected after launch");
        tracker.Observe({1,1},{-500+3000*time,30,0},time,10+time,false);
        arrowPasses += tracker.Drain(10+time,out);
    }
    Require(arrowPasses == 1, "A native arrow fails to reach the flyby queue or repeats");
    tracker.Reset(); tracker.Publish(0,42,{},20);
    tracker.Observe({2,2},{-100,30,0},0,20,true);
    Require(Eligible(true,true,true,true,FlightVelocity(runtime),false), "Bolt linear velocity is rejected");
    tracker.Observe({2,2},{100,30,0},.02f,20.02,true);
    Require(tracker.Drain(20.02,out) == 1, "Native velocity adapter cannot deliver a bolt flyby");
    runtime.velocity = {3000,0,0}; runtime.linearVelocity = {};
    Require(!Eligible(true,true,true,true,FlightVelocity(runtime),false),
            "A stationary projectile's auxiliary velocity pretends it is still flying");
}

static void CinematicOutput()
{
    using namespace DietDrCamera;
    const dietdrcamera::Perlin1D x(101), y(202), z(303);
    const auto peak = [&](float distance, float amount, float range = kRange, float intensity = kIntensity) {
        float maximum = 0;
        for (int frame = 0; frame < 240; ++frame) {
            // Use the actual cinematic first-person sampler and its existing
            // event gain, including normalization of each axis-angle band.
            const FirstPersonNoiseLayer layer{double(frame)/240*kSpeed,
                intensity*3*DistanceScale(distance,range)*amount, kSpeed, kRotation, kDriftJitter, kRoughness};
            const auto bands = SampleFirstPersonNoise(layer,1,x,y,z);
            Point rotation;
            for (const auto& band : bands) {
                const Point axis{band.axis[0],band.axis[1],band.axis[2]};
                const float length = std::sqrt(axis.Dot(axis));
                if (length > .000001f) rotation = rotation+axis*(band.theta/length);
            }
            maximum = (std::max)(maximum,std::sqrt(rotation.Dot(rotation)));
        }
        return maximum;
    };
    const float near = peak(30,1), medium = peak(150,1), far = peak(270,1);
    Require(near > .0004f && near < .001f, "Close flyby is effectively invisible or exceeds its cinematic calibration");
    Require(near > medium*3 && medium > far*20 && peak(300,1) == 0 && peak(0,0) == 0,
            "Rendered cinematic noise loses distance scaling or ignores its off setting");
    Require(std::abs(peak(30,3)-near*3) < .000001f, "NPC Archery amount does not scale actual output");
    const auto magicPeak = [&](float distance, float amount) { return peak(distance,amount,kMagicRange,kMagicIntensity); };
    Require(std::abs(magicPeak(0,1)-peak(0,1)*1.5f) < .000001f && magicPeak(30,1) > near*1.5f,
            "Magic's rendered base intensity is not stronger than arrows");
    Require(magicPeak(450,1) > 0 && peak(450,1) == 0 && magicPeak(600,1) == 0 && magicPeak(0,0) == 0,
            "Magic's broader rendered falloff loses its boundary, mute or separation from arrows");
    Require(std::abs(magicPeak(30,3)-magicPeak(30,1)*3) < .000001f,
            "NPC Magic amount does not scale the stronger output");
}

static void Geometry()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Require(Eligible(true, true, true, true, {3000,0,0}) && Eligible(true, true, true, true, {}, true),
            "Arrow/bolt flight or the initial pre-velocity sample is lost");
    Require(!Eligible(false, true, true, true, {3000,0,0}) && !Eligible(true, false, true, true, {3000,0,0}) &&
            !Eligible(true, true, false, true, {3000,0,0}) && !Eligible(true, true, true, false, {3000,0,0}) &&
            !Eligible(true, true, true, true, {}) && !Eligible(true, true, true, true, {nan,0,0}),
            "Player shots, non-archery projectiles, stuck arrows or invalid motion enter NPC flybys");
    Require(DistanceScale(0) == 1 && DistanceScale(kRange) == 0 && DistanceScale(kRange+1) == 0 &&
            DistanceScale(nan) == 0 && DistanceScale(inf) == 0 && DistanceScale(-1) == 0,
            "Falloff has no quiet boundary or accepts invalid geometry");
    Require(DistanceScale(1,0) == 0 && DistanceScale(1,-1) == 0 &&
            DistanceScale(1,nan) == 0 && DistanceScale(1,inf) == 0,
            "A per-source range accepts invalid geometry");
    float previous = 1;
    for (int i = 1; i <= 3000; ++i) {
        const auto strength = DistanceScale(i*0.1f);
        Require(strength <= previous && strength >= 0 && previous-strength < 0.002f,
                "Distance scaling increases with distance or snaps");
        previous = strength;
    }
    for (float miss : {0.0f, 25.0f, 100.0f, 200.0f, 299.0f}) {
        const auto pass = ClosestPass({-1000,miss,0}, {1000,miss,0}, {}, {});
        Require(pass && Near(pass->distance, miss) && Near(pass->position.x, 0) && Near(pass->strength, DistanceScale(miss)),
                "A fast projectile skips the player between updates");
        const Point shift{23000,-41000,7000};
        const auto shifted = ClosestPass(Point{-1000,miss,0}+shift, Point{1000,miss,0}+shift, shift, shift);
        const auto vertical = ClosestPass({miss,0,-1000}, {miss,0,1000}, {}, {});
        Require(shifted && vertical && Near(shifted->distance, miss) && Near(vertical->distance, miss),
                "World origin, flight direction or height changes distance weighting");
    }
    Require(!ClosestPass({-1000,20,0}, {-400,20,0}, {}, {}) &&
            !ClosestPass({100,20,0}, {600,20,0}, {}, {}) &&
            !ClosestPass({0,20,0}, {0,20,0}, {}, {}) &&
            !ClosestPass({-1000,0,301}, {1000,0,301}, {}, {}) &&
            !ClosestPass({nan,0,0}, {1000,0,0}, {}, {}),
            "Approaching, departing, stationary, overhead-distant or invalid shots produce noise");
    const auto moving = ClosestPass({-1000,100,0}, {1000,100,0}, {0,0,0}, {0,200,0});
    Require(moving && Near(moving->distance, 0), "Player motion is ignored during a crossing");
}

static void DifferentRadii()
{
    Tracker archery, magic(kMagicRange);
    std::array<Pass,64> out;
    // The same crossing must be audible to magic's tracker after every reset,
    // while arrows/bolts retain their original boundary in either view.
    for (int view : {0,1}) for (bool bolt : {false,true}) {
        archery.Reset(); magic.Reset();
        archery.Publish(view,42,{},10); magic.Publish(view,42,{},10);
        archery.Observe({1,1},{-100,450,0},0,10,bolt);
        magic.Observe({2,2},{-100,450,0},0,10,false,false,false,1234);
        archery.Observe({1,1},{100,450,0},.01f,10.01,bolt);
        magic.Observe({2,2},{100,450,0},.01f,10.01,false,false,false,1234);
        Require(archery.Drain(10.01,out) == 0 && magic.Drain(10.01,out) == 1 &&
                Near(out[0].distance,450) && out[0].strength > 0,
                "Magic does not reach beyond arrows/bolts, or loses its wider range after reset");
    }
}

static void FlightAndContacts()
{
    Tracker tracker;
    std::array<Pass,64> out;
    const Identity arrow{1,1001}, bolt{2,1002};
    tracker.Observe(arrow, {-100,30,0}, 0, 10, false);
    Require(tracker.Drain(10,out) == 0 && !tracker.Active(), "Disabled source retains flight");
    tracker.Publish(0, 42, {}, 10);
    tracker.Observe(arrow, {-100,30,0}, 0, 10, false);
    Require(tracker.Drain(10,out) == 0, "Bow release creates an impulse");
    tracker.Observe(arrow, {-100,30,0}, 0, 10, false);
    tracker.Observe(arrow, {-10,30,0}, .02f, 10.02, false);
    Require(tracker.Drain(10.02,out) == 0, "Approach arms before the actual pass");
    tracker.Observe(arrow, {100,30,0}, .04f, 10.04, false);
    Require(tracker.Drain(10.04,out) == 1 && out[0].projectile == arrow && !out[0].bolt && Near(out[0].distance,30),
            "Actual close arrow pass is lost");
    tracker.Observe(arrow, {100,30,0}, .04f, 10.04, false);
    tracker.Observe(arrow, {-100,30,0}, .05f, 10.05, false);
    Require(tracker.Drain(10.05,out) == 0, "Repeated flight or a ricochet repeats the same projectile");
    tracker.Observe(bolt, {-100,80,0}, 0, 10.05, true);
    tracker.Observe(bolt, {100,80,0}, .01f, 10.06, true, true);
    Require(tracker.Drain(10.06,out) == 1 && out[0].bolt && Near(out[0].distance,80),
            "Final segment before a scenery impact drops a real bolt flyby");

    tracker.Reset(); tracker.Publish(0, 42, {}, 20);
    tracker.Observe(arrow, {-1000,0,0}, 0, 20, false);
    tracker.Observe(arrow, {-400,0,0}, .1f, 20.1, false, true);
    tracker.Observe(arrow, {400,0,0}, .15f, 20.15, false);
    Require(tracker.Drain(20.15,out) == 0, "Wall impact allows an imaginary segment past the wall");
    tracker.Observe(bolt, {-100,0,0}, 0, 20.1, true);
    tracker.Observe(bolt, {10,0,0}, .01f, 20.11, true);
    tracker.Observe(bolt, {10,0,0}, .01f, 20.11, true, true, true);
    Require(tracker.Drain(20.11,out) == 0, "Known player hit doubles Damage Reaction with a pending flyby");

    // Dynamic IDs include the reference generation. Pooling the same live
    // reference is also supported when the engine resets projectile lifetime.
    tracker.Publish(0,42,{},20.12);
    tracker.Observe({1,1003}, {-100,60,0}, 0, 20.12, false);
    tracker.Observe({1,1003}, {100,60,0}, .01f, 20.13, false);
    Require(tracker.Drain(20.13,out) == 1, "A recycled FormID is incorrectly deduplicated");
    tracker.Observe({1,1003}, {-100,60,0}, 0, 20.14, false);
    tracker.Observe({1,1003}, {100,60,0}, .01f, 20.15, false);
    Require(tracker.Drain(20.15,out) == 1, "A pooled reference lifetime reset loses a new projectile");
}

static void RateAndResets()
{
    for (bool bolt : {false,true}) for (int fps : {20,30,60,120,240}) for (float speed : {1500.0f,3000.0f,12000.0f}) {
        Tracker tracker;
        std::array<Pass,64> out;
        int passes = 0;
        for (int tick = 0; tick <= fps*2; ++tick) {
            const float elapsed = float(tick)/fps;
            tracker.Publish(0,42,{},100+elapsed);
            tracker.Observe({10,20}, {-speed*.45f+speed*elapsed, 75, 0}, elapsed, 100+elapsed, bolt);
            const auto count = tracker.Drain(100+elapsed,out);
            passes += static_cast<int>(count);
            if (count) Require(Near(out[0].distance,75) && Near(out[0].strength,DistanceScale(75)),
                               "Frame rate or projectile speed changes close-pass strength");
        }
        Require(passes == 1, "An arrow/bolt is missed or repeated at a supported frame rate");
    }
    for (int change = 0; change < 6; ++change) {
        Tracker tracker;
        std::array<Pass,64> out;
        tracker.Publish(0,42,{},10);
        tracker.Observe({1,1},{-100,30,0},0,10,false);
        tracker.Observe({1,1},{100,30,0},.01f,10.01,false);
        switch (change) {
        case 0: tracker.Reset(); break;
        case 1: tracker.Publish(-1,42,{},10.02); break;
        case 2: tracker.Publish(1,42,{},10.02); break;
        case 3: tracker.Publish(0,43,{},10.02); break;
        case 4: tracker.Publish(0,42,{10000,0,0},10.02); break;
        case 5: tracker.Publish(0,42,{},11); break;
        }
        Require(tracker.Drain(change == 5 ? 11 : 10.02,out) == 0,
                "Disable, POV, load, teleport or stalled camera replays a queued flyby");
    }
    Tracker stale;
    std::array<Pass,64> out;
    stale.Publish(0,42,{},10);
    stale.Observe({1,1},{-100,30,0},0,10,false);
    stale.Observe({1,1},{100,30,0},.01f,10.01,false);
    Require(stale.Drain(10.2,out) == 0, "Delayed noise survives beyond the freshness window");

    Tracker volley;
    volley.Publish(0,42,{},10);
    for (std::uint32_t id = 1; id <= 80; ++id) volley.Observe({id,id},{-100,30,0},0,10,false);
    for (std::uint32_t id = 1; id <= 80; ++id) volley.Observe({id,id},{100,30,0},.01f,10.01,false);
    Require(volley.Drain(10.01,out) == 64 && volley.Drain(10.01,out) == 0,
            "A volley exceeds fixed storage or repeats already drained passes");
}

int main() try
{
    Geometry(); FlightAndContacts(); RateAndResets(); NativeArrowVelocity(); CinematicOutput();
    MagicFlights(); IndependentSourcesAndWaves(); DifferentRadii();
    std::cout << "Projectile flyby checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
