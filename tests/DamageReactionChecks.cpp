#include "Core/DamageReactionAttacks.h"
#include "Core/CreatureMagic.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace DietDrCamera;
namespace Reaction = DamageReaction;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
#include "CreatureMagicRecords.inc"

static void CheckMagicContacts()
{
    using Origin = Reaction::Contact::Origin;
    Reaction::Inbox inbox;
    inbox.SetView(0);
    auto token = inbox.Token();
    std::array<Reaction::Contact, 64> out;
    Reaction::Contact hit{Reaction::Kind::VenomSpit, {1,0,0}, 10, 7, 42};
    hit.origin = Origin::MagicImpact; hit.projectile = 101;
    inbox.Push(hit, token);
    Require(inbox.Drain(10, out) == 1, "Spit body contact has no impact");
    hit.time += .01; hit.projectile = 0;
    inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 0, "Hit event repeats a consumed projectile impact");
    hit.sustained = true; hit.kind = Reaction::Kind::Poison; hit.origin = Origin::Effect;
    inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1 && out[0].sustained,
        "A spit impact suppressed its poison damage stream");
    hit.sustained = false; hit.kind = Reaction::Kind::VenomSpit; hit.origin = Origin::MagicImpact;
    hit.projectile = 102; inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "Two distinct projectiles were merged");
    hit.attacker = 8; hit.projectile = 103; inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "Simultaneous creatures share contact receipts");
    inbox.SetView(1); token = inbox.Token();
    inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "Changing views retained contact receipts");
    inbox.Clear(); inbox.SetView(1); token = inbox.Token();
    hit.kind = Reaction::Kind::Fire; hit.origin = Origin::Effect; hit.projectile = 0;
    inbox.Push(hit, token);
    hit.origin = Origin::MagicImpact; hit.projectile = 104;
    inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "Instantaneous damage and projectile contact double the same spell");
    hit.time += 1; hit.kind = Reaction::Kind::Force; hit.source = 51; hit.projectile = 0;
    inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "An area stomp has no force contact");
    hit.source = 52; hit.time += .01; inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 0,
        "A stomp carrier and its explosion enchantment produced duplicate force impulses");
    hit.attacker += 1; inbox.Push(hit, token);
    Require(inbox.Drain(hit.time, out) == 1, "Stomps from separate creatures were merged");

    CreatureMagic::Effect web{"ModWebSlow", 34, 30, -1, -1, 1, 2, -1, true, true, true};
    CreatureMagic::Spell spell; spell.Include(web);
    Require(spell.Impact(1, 0) == Reaction::Kind::Web, "A hostile slowing web needs a health tick");
    web.hostile = false; spell = {}; spell.Include(web);
    Require(!spell.Impact(1, 0), "A non-hostile movement penalty became an incoming attack");
    web.hostile = true; web.delivery = 0; spell = {}; spell.Include(web);
    Require(!spell.Impact(1, 0), "A self-applied penalty became an incoming attack");
    web.delivery = 2; spell = {}; spell.Include(web);
    Require(!spell.Impact(1, 1) && !spell.Impact(1, 4), "Disease/ability application became a projectile hit");
    web.continuous = true; spell = {}; spell.Include(web);
    Require(!spell.Impact(1, 0), "A continuous projectile became repeated discrete jolts");
    Require(CreatureMagic::CastNoiseAllowed(1, 0, 2, spell),
        "A fire-and-forget spell with a persistent projectile lost its release noise");
    Require(!CreatureMagic::CastNoiseAllowed(2, 0, 2, spell),
        "Concentration casting acquired a discrete release beat");
    CreatureMagic::Effect frost{"CustomIceDamage", 0, 24, -1, -1, 1, 2, 2, true, true, true, false, false, true};
    Require(CreatureMagic::EffectKind(frost) == Reaction::Kind::Frost,
        "A frost-keyword attack with generic resistance lost its element");
}
static float Magnitude(Reaction::Rotation r) { return std::sqrt(r.pitch*r.pitch + r.yaw*r.yaw + r.roll*r.roll); }
static float Difference(Reaction::Rotation a, Reaction::Rotation b) { return Magnitude({a.pitch-b.pitch,a.yaw-b.yaw,a.roll-b.roll}); }
static constexpr Reaction::Basis basis{{1,0,0},{0,1,0},{0,0,1}};
static Reaction::Contact Hit(Reaction::Kind kind = Reaction::Kind::Blade)
{
    return {kind, {1, 1, 0}, 10, 7, 42};
}

static Reaction::Rotation ImpactPeak(Reaction::Kind kind, Reaction::Vector toward, bool fp, float intensity,
    float fps, bool power = false, bool blocked = false)
{
    Reaction::Mixer mixer;
    auto hit = Hit(kind); hit.towardSource = toward; hit.power = power; hit.blocked = blocked;
    mixer.Add(hit,{intensity},basis,fp,10);
    Reaction::Rotation peak{};
    for (int i = 0; i < static_cast<int>(fps); ++i) {
        const auto r = mixer.Advance(1/fps,fp);
        if (Magnitude(r) > Magnitude(peak)) peak = r;
    }
    return peak;
}

static Reaction::Vector Cross(Reaction::Vector a, Reaction::Vector b)
{
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
static Reaction::Vector Turn(Reaction::Vector v, Reaction::Vector angular)
{
    const float angle = angular.Length();
    if (angle == 0) return v;
    const auto axis = angular * (1/angle);
    return v*std::cos(angle) + Cross(axis,v)*std::sin(angle) + axis*(axis.Dot(v)*(1-std::cos(angle)));
}
static Reaction::Vector RenderedRay(Reaction::Rotation r, Reaction::Basis camera, bool fp)
{
    if (fp) {
        const auto child = HitShake::NiCameraRotationVector(r);
        return Turn(camera.forward,camera.forward*child[0] + camera.up*child[1] + camera.right*child[2]);
    }
    // The third-person compositor post-multiplies pitch, then yaw, then roll.
    auto local = Turn({0,1,0},{0,r.roll,0});
    local = Turn(local,{0,0,r.yaw});
    return camera.ToWorld(Turn(local,{r.pitch,0,0}));
}

static void CheckRenderedDirections()
{
    // Test the viewing ray after the real root/child axis mapping. Checking
    // only opposite yaw scalars missed recoil that looked INTO a side hit.
    for (bool fp : {false,true}) for (float heading : {-2.4f,0.0f,1.3f})
        for (float pitch : {-.8f,0.0f,.8f}) for (float bank : {-.2f,0.0f,.2f}) {
            const Reaction::Vector forward{std::sin(heading)*std::cos(pitch),std::cos(heading)*std::cos(pitch),-std::sin(pitch)};
            const auto right = Turn({std::cos(heading),-std::sin(heading),0},forward*bank);
            const Reaction::Basis camera{right,forward,Cross(right,forward)};
            for (const Reaction::Vector from : {Reaction::Vector{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},
                    {0,0,1},{0,0,-1},{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0}}) {
                Reaction::Mixer mixer;
                auto hit = Hit(Reaction::Kind::HeavyBlade); hit.towardSource = camera.ToWorld(from);
                mixer.Add(hit,{3},camera,fp,10);
                const auto r = mixer.Advance(.036f,fp);
                const auto displacement = camera.ToLocal(RenderedRay(r,camera,fp)-camera.forward);
                if (from.x != 0) Require(displacement.x*from.x < -.065f,
                    "A side hit fails to push the rendered camera away from its source");
                if (from.y != 0) Require(displacement.z*from.y > .065f,
                    "A front/rear hit loses its rendered pitch recoil");
                if (from.z != 0) Require(displacement.z*from.z < -.065f,
                    "An elevated projectile fails to deflect the view along its incoming direction");
            }
        }
}

// Use the controller's real ordering: advance time, receive contacts, render.
// Three seconds of continuous contact followed by two seconds of recovery.
static std::vector<Reaction::Rotation> StreamTrace(Reaction::Kind kind, bool fp, int fps, bool irregular = false, int duplicates = 1)
{
    Reaction::Mixer mixer;
    auto hit = Hit(kind); hit.sustained = true; hit.towardSource = {0,1,0};
    mixer.Add(hit,{3},basis,fp,10);
    std::vector<Reaction::Rotation> trace{mixer.Advance(0,fp)};
    double nextTick = 0;
    constexpr double intervals[]{.08,.12,.04,.10};
    int tick = 0;
    for (int frame = 1; frame <= 5*fps; ++frame) {
        const double elapsed = static_cast<double>(frame)/fps;
        const auto before = mixer.Advance(1.0f/fps,fp);
        if (frame <= 3*fps && (elapsed + 1e-8 >= nextTick || frame == 3*fps)) {
            hit.time = 10 + elapsed;
            for (int n = 0; n < duplicates; ++n) mixer.Add(hit,{3},basis,fp,hit.time);
            Require(Difference(before,mixer.Advance(0,fp)) < 1e-7f, "A magic tick restarted the stream motion");
            nextTick = elapsed + (irregular ? intervals[tick++ % 4] : 0);
        }
        trace.push_back(mixer.Advance(0,fp));
    }
    return trace;
}

#include "DamageReactionAttackChecks.inc"

static void CheckBlockUtilityDuringDamage()
{
    // Recorded source 3C12B39B resolves to Adamant's MAG_BlockSlowdownSpell:
    // its speed effect is detrimental, not hostile, and has magnitude 40.
    // The old amount/flags-only predicate admitted it as a discrete magic hit.
    constexpr float blockSlowdown = -40.0f;
    Require(!Reaction::IsHarmfulEffectTick(blockSlowdown,false,true,false),
        "Adamant's block slowdown was mistaken for an incoming magic hit");
    for (bool hostile : {false,true}) for (bool detrimental : {false,true}) {
        Require(!Reaction::IsHarmfulEffectTick(-40,hostile,detrimental,false),
            "A movement, armor, stamina or magicka modifier generated a damage reaction");
    }

    float legacyExtraPeak = 0;
    for (bool fp : {false,true}) for (int fps : {30,60,144,240}) {
        Reaction::Mixer expected, actual, legacy;
        for (int frame = 0; frame < fps*3; ++frame) {
            const double now = 10 + static_cast<double>(frame)/fps;
            const auto a = expected.Advance(1.0f/fps,fp);
            const auto b = actual.Advance(1.0f/fps,fp);
            const auto c = legacy.Advance(1.0f/fps,fp);
            Require(Difference(a,b) == 0,
                "Raising/releasing block added a reaction during ongoing magic or a weapon hit");
            legacyExtraPeak = (std::max)(legacyExtraPeak,Difference(a,c));

            if (frame < fps*2) {
                auto fire = Hit(Reaction::Kind::Fire);
                fire.time = now; fire.sustained = true; fire.towardSource = {-1,0,0};
                expected.Add(fire,{1},basis,fp,now);
                actual.Add(fire,{1},basis,fp,now);
                legacy.Add(fire,{1},basis,fp,now);
            }
            if (frame == fps || frame == fps*2) {
                auto weapon = Hit(Reaction::Kind::HeavyBlade);
                weapon.time = now; weapon.blocked = true;
                expected.Add(weapon,{1},basis,fp,now);
                actual.Add(weapon,{1},basis,fp,now);
                legacy.Add(weapon,{1},basis,fp,now);
            }
            if (frame % (fps/3) == 0) {
                auto utility = Hit(Reaction::Kind::Magic);
                utility.source = 0x3C12B39B; utility.attacker = 0x14;
                utility.time = now; utility.towardSource = {};
                if (Reaction::IsHarmfulEffectTick(blockSlowdown,false,true,false))
                    actual.Add(utility,{1},basis,fp,now);
                legacy.Add(utility,{1},basis,fp,now);
                Require(!Reaction::IsHarmfulEffectTick(-blockSlowdown,false,true,false),
                    "Releasing block generated a reaction from restored movement speed");
            }
        }
    }
    Require(legacyExtraPeak > .01f, "The recorded block-effect fixture did not reproduce an extra kick");
    std::cout << "Block utility extra reaction: legacy peak=" << legacyExtraPeak
              << " rad, filtered=0 (both views, 30/60/144/240 FPS)\n";
}

int main() try
{
    CheckCreatureSpells();
    CheckMagicContacts();
    CheckBlockUtilityDuringDamage();
    CheckCreatureRoster();
    CheckAttackProfiles();
    CheckFirstPersonTravel();
    for (bool fp : {false,true}) for (float intensity : {1.0f,3.0f}) {
        const auto front = ImpactPeak(Reaction::Kind::HeavyBlade,{0,1,0},fp,intensity,240);
        const auto side = ImpactPeak(Reaction::Kind::HeavyBlade,{1,0,0},fp,intensity,240);
        std::cout << (fp ? "First" : "Third") << " person greatsword, intensity " << intensity
            << ": front pitch " << front.pitch*180/HitShake::kPi << " deg, side yaw "
            << side.yaw*180/HitShake::kPi << " deg, side roll " << side.roll*180/HitShake::kPi << " deg\n";
    }
    CheckRenderedDirections();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto sanitized = Reaction::Sanitize({nan});
    Require(sanitized.intensity == 0 && Reaction::Sanitize({-1}).intensity == 0 && Reaction::Sanitize({8}).intensity == 3,
        "Malformed tuning was not bounded");
    Require(!Reaction::IsHarmfulEffectTick(0,true,true,true) && !Reaction::IsHarmfulEffectTick(10,true,true,true) &&
        !Reaction::IsHarmfulEffectTick(nan,true,true,true) && !Reaction::IsHarmfulEffectTick(-10,false,false,true),
        "Healing, invalid or expiring beneficial effects generated an impact");
    Require(Reaction::IsHarmfulEffectTick(-0.001f,true,false,true) && Reaction::IsHarmfulEffectTick(-100000,true,false,true) &&
        Reaction::IsHarmfulEffectTick(-1,false,true,true),
        "Harmful magic depends on the amount of damage");
    const auto left = Reaction::Direction({-1,0,0},basis,1,false);
    const auto right = Reaction::Direction({1,0,0},basis,1,false);
    const auto front = Reaction::Direction({0,1,0},basis,1,false);
    const auto rear = Reaction::Direction({0,-1,0},basis,1,false);
    Require(left.yaw < 0 && right.yaw > 0 && left.yaw == -right.yaw && left.roll == -right.roll,
        "Left/right impact directions do not mirror");
    Require(front.pitch > 0 && rear.pitch == -front.pitch, "Rear hits did not reverse the forward cue");
    const Reaction::Basis turned{{0,1,0},{-1,0,0},{0,0,1}};
    Require(Reaction::Direction({-1,0,0},turned,1,false).pitch == front.pitch,
        "Direction follows world axes instead of the camera");
    Require(Reaction::Direction({nan,0,0},basis,1,false).yaw == 0 &&
        Reaction::Direction({1,0,0},basis,0,false).yaw == 0, "Unknown/disabled direction invented a sideways hit");

    for (bool fp : {false,true}) for (int kind = 0; kind < static_cast<int>(Reaction::Kind::Count); ++kind) {
        for (float fps : {30.0f,60.0f,144.0f,240.0f}) {
            Reaction::Mixer mixer;
            auto hit = Hit(static_cast<Reaction::Kind>(kind));
            mixer.Add(hit,{1},basis,fp,10);
            Require(Magnitude(mixer.Advance(0,fp)) == 0, "Impact begins with a displacement snap");
            float peak = 0;
            for (int i = 0; i < static_cast<int>(2*fps); ++i) {
                const auto r = mixer.Advance(1/fps,fp);
                Require(std::isfinite(Magnitude(r)), "Reaction became non-finite");
                Require(std::abs(r.pitch) <= (fp ? 0.22001f : 0.12001f) &&
                    std::abs(r.yaw) <= (fp ? 0.20001f : 0.11001f) &&
                    std::abs(r.roll) <= (fp ? 0.02201f : 0.03501f), "Reaction exceeded its rotation limits");
                peak = std::max(peak,Magnitude(r));
            }
            Require(peak > 0.0001f && Magnitude(mixer.Advance(0,fp)) == 0, "Impact missed low FPS or failed to settle");
        }
        Reaction::Mixer a,b;
        a.Add(Hit(static_cast<Reaction::Kind>(kind)),{1},basis,fp,10);
        b.Add(Hit(static_cast<Reaction::Kind>(kind)),{1},basis,fp,10);
        const auto coarse = a.Advance(0.10f,fp);
        Reaction::Rotation fine;
        for (int step = 0; step < 10; ++step) fine = b.Advance(0.01f,fp);
        Require(std::abs(coarse.pitch-fine.pitch) < 1e-6f && std::abs(coarse.yaw-fine.yaw) < 1e-6f,
            "Reaction depends on integration frame rate");
    }
    Require(Reaction::Shape(Reaction::Kind::Arrow).rise < Reaction::Shape(Reaction::Kind::Blunt).rise &&
        Reaction::Shape(Reaction::Kind::Frost).rise > Reaction::Shape(Reaction::Kind::Fire).rise &&
        Reaction::Shape(Reaction::Kind::Shock).frequency > Reaction::Shape(Reaction::Kind::Fire).frequency,
        "Attack types lost their distinct response");
    for (bool fp : {false,true}) for (float fps : {30.0f,60.0f,144.0f,240.0f}) {
        const auto peak = [&](Reaction::Kind kind, float intensity, bool power = false, bool blocked = false) {
            return Magnitude(ImpactPeak(kind,{0,1,0},fp,intensity,fps,power,blocked));
        };
        const auto normal = peak(Reaction::Kind::HeavyBlade,1);
        Require(normal > (fp ? .088f : .044f), "A greatsword impact at intensity one is too faint");
        Require(peak(Reaction::Kind::HeavyBlade,3) > (fp ? .190f : .095f), "Maximum greatsword recoil remains too faint");
        const auto side = ImpactPeak(Reaction::Kind::HeavyBlade,{1,0,0},fp,3,fps);
        Require(side.yaw > (fp ? .180f : .090f) && std::abs(side.roll) < (fp ? .021f : .034f),
            "Maximum side impact produces too little view deflection or too much roll");
        Require(normal > 1.2f * peak(Reaction::Kind::Blade,1), "Two-handed blades have no additional weight");
        Require(peak(Reaction::Kind::HeavyBlade,3) > 1.65f * normal, "Maximum intensity has no useful range");
        Require(peak(Reaction::Kind::HeavyBlade,1,true) > 1.15f * normal, "Power attacks do not produce heavier recoil");
        const auto blocked = peak(Reaction::Kind::HeavyBlade,1,false,true);
        Require(blocked > 0 && blocked < .8f * normal, "Blocked impacts must remain visible but lighter");
    }
    Reaction::Mixer quiet;
    quiet.Add(Hit(),{},basis,false,10);
    Require(Magnitude(quiet.Advance(.02f,false)) == 0, "Zero intensity is not silent");
    quiet.Add(Hit(),{1},basis,false,10.3);
    Require(Magnitude(quiet.Advance(.02f,false)) == 0, "An old damage event replayed");
    auto invalidTime = Hit(); invalidTime.time = nan;
    quiet.Add(invalidTime,{1},basis,false,10);
    Require(Magnitude(quiet.Advance(.02f,false)) == 0, "An invalid timestamp contaminated camera motion");
    for (int i = 0; i < 1000; ++i) quiet.Add(Hit(),{3},basis,true,10);
    Require(Magnitude(quiet.Advance(.02f,true)) < .300f, "A damage storm bypassed bounded mixing");
    Require(Magnitude(quiet.Advance(.4f,true)) == 0, "A stall did not clear old motion");

    for (bool fp : {false,true}) for (auto kind : {Reaction::Kind::Fire,Reaction::Kind::Frost,Reaction::Kind::Shock,Reaction::Kind::Poison,Reaction::Kind::Magic}) {
        const auto reference = StreamTrace(kind,fp,240);
        for (int fps : {30,60,144,240}) {
            const auto trace = StreamTrace(kind,fp,fps);
            const auto irregular = StreamTrace(kind,fp,fps,true,7);
            Require(Magnitude(trace.front()) == 0 && Magnitude(trace.back()) == 0, "Stream snapped on or failed to fade out");
            float minimum = 1, maximum = 0, speed = 0;
            for (int frame = 1; frame <= 5*fps; ++frame) {
                const auto r = trace[frame];
                Require(std::isfinite(Magnitude(r)) && std::abs(r.pitch) <= (fp ? .11001f : .12001f) &&
                    std::abs(r.yaw) <= (fp ? .10001f : .11001f) && std::abs(r.roll) <= (fp ? .02201f : .03501f),
                    "Continuous magic exceeded its rotation limits");
                Require(Difference(r,irregular[frame]) < 2e-6f, "Stream motion depends on tick count or ordinary callback jitter");
                if (frame >= fps && frame <= 3*fps) {
                    minimum = std::min(minimum,r.pitch);
                    maximum = std::max(maximum,r.pitch);
                    speed = std::max(speed,Difference(r,trace[frame-1])*fps);
                }
                if ((frame*240) % fps == 0)
                    Require(Difference(r,reference[frame*240/fps]) < 2e-6f, "Stream attack, phase or release depends on frame rate");
            }
            Require(maximum > .003f && minimum > .5f*maximum, "Sustained magic falls into repeated attack/recovery jolts");
            Require(speed < .10f, "Steady magic contains sharp camera jolts even at maximum intensity");
        }
    }
    Require(Reaction::StreamShape(Reaction::Kind::Shock).rippleFrequency > Reaction::StreamShape(Reaction::Kind::Fire).rippleFrequency &&
        Reaction::StreamShape(Reaction::Kind::Fire).swayFrequency > Reaction::StreamShape(Reaction::Kind::Frost).swayFrequency,
        "Magic streams lost their elemental character");

    // Direction changes and interrupted contact retain position and velocity
    // in both views, including the stronger first-person pressure at maximum.
    for (bool fp : {false,true}) for (float intensity : {1.0f,3.0f}) {
        Reaction::Mixer turning;
        auto stream = Hit(Reaction::Kind::Fire); stream.sustained = true; stream.towardSource = {1,0,0};
        turning.Add(stream,{intensity},basis,fp,10);
        for (int frame = 1; frame <= 120; ++frame) {
            turning.Advance(1.0f/120,fp);
            stream.time = 10 + frame/120.0;
            turning.Add(stream,{intensity},basis,fp,stream.time);
        }
        const auto beforeTurn = turning.Advance(0,fp);
        stream.towardSource = {-1,0,0};
        turning.Add(stream,{intensity},basis,fp,stream.time);
        Require(Difference(beforeTurn,turning.Advance(0,fp)) == 0 && beforeTurn.yaw > 0,
            "A new stream direction snapped the camera");
        Require(Difference(beforeTurn,turning.Advance(1.0f/120,fp)) < .0003f*intensity, "Stream direction changes are abrupt");
        for (int frame = 2; frame <= 60; ++frame) {
            turning.Advance(1.0f/120,fp);
            stream.time = 11 + frame/120.0;
            turning.Add(stream,{intensity},basis,fp,stream.time);
        }
        Require(turning.Advance(0,fp).yaw < 0, "A moving attacker did not update the stream direction");
        turning.Advance(.2f,fp);
        const auto tail = turning.Advance(.1f,fp);
        stream.time += .3;
        turning.Add(stream,{intensity},basis,fp,stream.time);
        Require(Difference(tail,turning.Advance(0,fp)) == 0, "Resuming contact restarted a fading stream");
        Require(Magnitude(turning.Advance(.4f,fp)) == 0, "A stall retained a stream");
    }

    // The reported session included ongoing poison while a blade landed.
    // Sustained pressure must leave room for a readable new weapon impact.
    for (bool fp : {false,true}) {
        Reaction::Mixer mixed;
        auto poison = Hit(Reaction::Kind::Poison); poison.sustained = true; poison.towardSource = {-1,0,0};
        mixed.Add(poison,{3},basis,fp,10);
        for (int frame = 1; frame <= 240; ++frame) {
            mixed.Advance(1.0f/240,fp);
            poison.time = 10 + frame/240.0;
            mixed.Add(poison,{3},basis,fp,poison.time);
        }
        const auto beforeHit = mixed.Advance(0,fp);
        auto blade = Hit(); blade.time = 11; blade.towardSource = {1,0,0};
        mixed.Add(blade,{3},basis,fp,11);
        Require(Difference(beforeHit,mixed.Advance(0,fp)) == 0, "A weapon hit snapped an active stream");
        Require(mixed.Advance(.020f,fp).yaw-beforeHit.yaw > (fp ? .130f : .065f),
            "Ongoing poison masks the direction of a new weapon impact");
    }

    Reaction::Mixer crowd;
    auto stream = Hit(Reaction::Kind::Fire); stream.sustained = true; stream.towardSource = {-1,0,0};
    for (int source = 0; source < 1000; ++source) {
        stream.source = source; stream.time = 10;
        crowd.Add(stream,{3},basis,true,10);
    }
    Require(Magnitude(crowd.Advance(.1f,true)) < .151f, "Simultaneous streams bypassed bounded mixing");
    crowd.Clear();
    Require(Magnitude(crowd.Advance(0,true)) == 0, "Reset retained streaming motion");

    Reaction::Inbox inbox;
    std::array<Reaction::Contact,64> out;
    inbox.Push(Hit(),inbox.Token());
    Require(inbox.Drain(10,out) == 0, "Suppressed camera accepted incoming damage");
    inbox.SetView(0);
    const auto token = inbox.Token();
    auto hit = Hit(Reaction::Kind::HeavyBlade); hit.power = hit.blocked = true;
    inbox.Push(hit,token);
    Require(inbox.Drain(10.02,out) == 1 && out[0].kind == Reaction::Kind::HeavyBlade && out[0].power && out[0].blocked,
        "A weapon impact required a health-change notification");
    hit = Hit(Reaction::Kind::Arrow);
    inbox.Push(hit,token);
    inbox.Describe({{0,-4,2},10.005,7,42},token);
    Require(inbox.Drain(10.02,out) == 1 && out[0].towardSource.y == -4, "Incoming projectile travel was lost");
    inbox.SetView(1);
    inbox.Push(Hit(),token);
    Require(inbox.Drain(10.02,out) == 0, "An in-flight callback survived a camera handoff");
    inbox.Push(hit,inbox.Token());
    inbox.Describe({{0,-4,2},10.005,7,43},inbox.Token());
    Require(inbox.Drain(10.02,out) == 1 && out[0].kind == Reaction::Kind::Arrow && out[0].source == 42,
        "Projectile identity was lost without a health event");
    Require(out[0].towardSource.y == 1, "A different projectile source replaced the hit direction");
    hit = Hit(Reaction::Kind::Fire); hit.sustained = true;
    for (int i = 0; i < 50; ++i) inbox.Push(hit,inbox.Token());
    Require(inbox.Drain(10.02,out) == 1, "Continuous effects multiplied camera contacts per tick");
    inbox.Push(hit,inbox.Token());
    auto latest = hit; latest.time = 10.01; latest.towardSource = {-1,0,0};
    inbox.Push(latest,inbox.Token());
    inbox.Push(hit,inbox.Token());
    Require(inbox.Drain(10.02,out) == 1 && out[0].time == latest.time && out[0].towardSource.x == -1,
        "Coalescing a stream discarded its freshest direction or contact time");
    Reaction::Mixer singleTick,manyTicks;
    singleTick.Add(hit,{1},basis,false,10);
    for (int i = 0; i < 1000; ++i) manyTicks.Add(hit,{1},basis,false,10);
    Require(std::abs(Magnitude(singleTick.Advance(.035f,false))-Magnitude(manyTicks.Advance(.035f,false))) < 1e-6f,
        "Continuous magic strength depends on tick count");
    inbox.Push(Hit(),inbox.Token());
    Require(inbox.Drain(11,out) == 0, "Stale damage survived a menu/loading gap");
    inbox.Clear();
    Require(inbox.Token() == 0, "Reset left event capture armed");
    std::cout << "Damage reaction checks passed (health-independent impacts, continuous streams, cadence/frame-rate invariance, direction, bounds and lifecycle).\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
