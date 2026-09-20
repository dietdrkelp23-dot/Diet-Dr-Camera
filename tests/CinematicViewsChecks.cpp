#include "Core/CinematicViews.h"
#include "Camera/VanityTransition.h"
#include "Core/CinematicViewHistory.h"
#include "Core/CombatFraming.h"
#include "Core/CinematicSubject.h"
#include "Core/CinematicViewSightline.h"
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace DietDrCamera;
using namespace CinematicViews;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static bool Near(float a, float b, float eps = .001f) { return std::abs(a-b) < eps; }

int main() try
{
    View view;
    view.id = "test-view"; view.name = "Chillrend"; view.space = {"Skyrim.esm",0x123};
    Require(Valid(view),"Valid view rejected");
    auto invalid = view; invalid.point = {1.0e30f,0,0};
    Require(!Valid(invalid),"Out-of-world coordinates accepted for a physics ray");
    invalid = view; invalid.id = std::string("bad\0id",6);
    Require(!Valid(invalid),"Unserializable encounter identity accepted");
    auto named = view; named.name = std::string(95,'x') + "\xE6\xA3\xAE";
    Require(Sanitize(named).name == std::string(95,'x'),"Long subject name was cut inside a UTF-8 character");
    Require(view.profile == CameraProfile::Default3p(),"New views are not normal third-person entries");
    Require(Eligible(view,false,200,true,0),"First radius entry waited for idle");
    Require(Eligible(view,false,view.radius,true,0) && !Eligible(view,false,view.radius+1,true,10),"Radius boundary is not respected");
    Require(!Eligible(view,true,200,true,0) && Eligible(view,true,200,true,5),"Later visits do not use the entry's idle timer");
    auto otherView = view; otherView.idleDelay = 15;
    Require(Eligible(view,true,200,true,5) && !Eligible(otherView,true,200,true,5),"Independent view timers interfere");
    Require(!Eligible(view,false,200,false,10),"Obstructed subject triggered automatically");
    {
        auto stones = view;
        stones.id = "lake-from-stones";
        stones.point = {150000,90000,5000};
        stones.triggerPoint = Point{100,200,300};
        auto cliff = stones;
        cliff.id = "lake-from-cliff";
        cliff.triggerPoint = Point{6000,-4000,2300};
        cliff.profile.fov = 65;
        Require(Valid(stones) && !stones.target.Valid(),"Distant scenery requires a loaded object identity");
        Require(MatchesSpace(stones,stones.space,true) &&
            !MatchesSpace(stones,stones.space,false) &&
            !MatchesSpace(stones,{"Skyrim.esm",0x456},true) &&
            !MatchesSpace(stones,{},true),
            "A trigger can activate at matching coordinates in another worldspace or interior");
        const auto canStart = [](const View& v, Point player, bool seen = false, double idle = 0) {
            return Eligible(v,seen,TriggerDistance(v,player,v.point),true,idle);
        };
        Require(canStart(stones,*stones.triggerPoint) && !canStart(cliff,*stones.triggerPoint),
            "The same lake starts its cliff view at the stones");
        Require(canStart(cliff,*cliff.triggerPoint) && !canStart(stones,*cliff.triggerPoint),
            "The same lake starts its stones view at the cliff");
        Require(!canStart(stones,stones.point) && !canStart(cliff,cliff.point),
            "Approaching a distant target starts a view outside its trigger area");
        Require(canStart(stones,*stones.triggerPoint+Point{500,0,0}) &&
            !canStart(stones,*stones.triggerPoint+Point{501,0,0}) &&
            !canStart(stones,*stones.triggerPoint+Point{0,0,501}),
            "Trigger radius ignores its horizontal or vertical boundary");
        History visits{stones.id};
        Require(!canStart(stones,*stones.triggerPoint,visits.contains(stones.id)) &&
            canStart(cliff,*cliff.triggerPoint,visits.contains(cliff.id)),
            "Two trigger areas for the same subject share encounter history");
        auto relocated = stones;
        relocated.triggerPoint = cliff.triggerPoint;
        Require(!canStart(relocated,*stones.triggerPoint) && canStart(relocated,*cliff.triggerPoint) &&
            relocated.point == stones.point && relocated.profile == stones.profile,
            "Relocating a trigger changes its target or leaves the previous trigger active");
        auto legacy = stones; legacy.triggerPoint.reset();
        Require(TriggerDistance(legacy,stones.point,stones.point) == 0 &&
            TriggerDistance(legacy,stones.point,stones.point+Point{100,0,0}) == 100,
            "Legacy subject-centered activation no longer follows its subject");
        auto broken = stones; broken.triggerPoint = Point{std::numeric_limits<float>::quiet_NaN(),0,0};
        Require(!Valid(broken),"Non-finite trigger position accepted");
        broken.triggerPoint = Point{1e30f,0,0};
        Require(!Valid(broken),"Out-of-world trigger position accepted");
        Require(CinematicSubject::RayMeetsBound({}, {0,1,0}, {0,150000,0},1000,CinematicSubject::kSceneryRange) &&
            !CinematicSubject::RayMeetsBound({}, {0,1,0}, {0,150000,0},1000),
            "Distant scenery picking still uses the nearby-object range");
        Require(CinematicSubject::ForwardHitDistance({}, {0,1,0}, {0,150000,0},CinematicSubject::kSceneryRange) == 150000 &&
            !CinematicSubject::ForwardHitDistance({}, {0,1,0}, {0,-150000,0},CinematicSubject::kSceneryRange) &&
            !CinematicSubject::ForwardHitDistance({}, {0,1,0}, {0,2000000,0},CinematicSubject::kSceneryRange) &&
            !CinematicSubject::ForwardHitDistance({}, {0,1,0}, {0,std::numeric_limits<float>::infinity(),0},CinematicSubject::kSceneryRange),
            "Scenery picker accepted an invalid, behind-camera or out-of-range hit");
    }
    Require(StickMoved(.001f,0) && StickMoved(0,-.01f) && !StickMoved(0,0) &&
        !StickMoved(std::numeric_limits<float>::quiet_NaN(),0),"Small real camera movement is discarded");
    for (const float fps : {15.0f,30.0f,60.0f,144.0f,240.0f}) {
        for (float delay : {.5f,5.0f,120.0f,600.0f}) {
            IdleTimer idle;
            view.idleDelay = delay;
            int frames = 0;
            while (!Eligible(view,true,200,true,idle.Seconds()) && frames < fps*(delay+1)) {
                idle.Step(false,1/fps); ++frames;
            }
            Require(std::abs(frames/fps-delay) < 1.1f/fps,"Idle activation depends on frame rate or adds another delay");
            idle.Step(StickMoved(.001f,0),1/fps);
            Require(idle.Seconds() == 0 && !Eligible(view,true,200,true,idle.Seconds()),"Camera movement did not restart the full idle wait");
            idle.Step(false,20);
            Require(idle.Seconds() == 0,"A paused frame immediately triggered an idle view");
        }
        Playback idlePlay; idlePlay.Start(view,false);
        Point player{};
        for (int frame=0; frame<static_cast<int>(fps*60); ++frame) {
            player.x += 100/fps; // Walking past and outside the acquisition radius.
            const auto tracked = FocusProfile(view.profile,Point{0,1000,0}-player,0,0);
            Require(std::isfinite(tracked.rotation),"Walking lost a usable subject heading");
            idlePlay.Step(1/fps,false,false);
        }
        Require(idlePlay.active && player.x > view.radius,"Walking or leaving the radius dropped the idle lock");
        idlePlay.Step(0,true,false);
        Require(!idlePlay.active,"Manual look did not release the view");
        Playback first; first.Start(view,true);
        int frames = 0;
        while (first.active && frames < fps*12) { first.Step(1/fps,false,false); ++frames; }
        Require(std::abs(frames/fps-10) < 1.1f/fps,"First radius encounter is not exactly ten seconds");
        first.Start(view,true);
        first.Step(.25f,false,false);
        const float age = first.age;
        for (int frame=0; frame<static_cast<int>(fps*60); ++frame) first.Step(1/fps,true,true,true);
        Require(first.active && first.age == age,"Quick Tune expired or dismissed the held first encounter");
        first.Step(1/fps,false,false);
        Require(first.active && first.age > age,"First encounter did not resume after tuning");
        first.Step(0,false,true);
        Require(!first.active,"Combat input did not release the view");
    }
    const auto normal = CameraProfile::Default3p();
    const auto focused = FocusProfile(normal,{500,1000,100},0,0);
    Require(focused.rotation > 5 && focused.rotation < 26.6f && focused.pitchOffset > 0,
        "Subject focus does not supply a loose native rotation/pitch target");
    Require(focused.zoom == normal.zoom && focused.fov == normal.fov && focused.height == normal.height && focused.sideOffset == normal.sideOffset,
        "Focus secretly changes the authored framing channels");
    for (int i=0; i<1000; ++i)
        Require(FocusProfile(normal,{500,1000,100},0,0) == focused,"Focus accumulates into the native profile");
    Require(FocusProfile(normal,{1,100,0},0,0) == normal,"Nearly centered subject is forcibly recentered");
    Require(FocusProfile(normal,{-500,1000,100},0,0).rotation < 0,"Moving past a subject does not update focus direction");
    Require(std::abs(FocusProfile(normal,{-.01f,-1000,0},3.14158f,0).rotation) < .1f,"Yaw seam caused a full camera revolution");
    Require(FocusProfile(normal,{},0,0) == normal,"Degenerate subject changed camera pose");
    constexpr float radians = .0174532925199433f;
    for (float heading : {-179.0f,-140.0f,-90.0f,0.0f,90.0f,140.0f,179.0f}) {
        const Point subject{1000*std::sin(heading*radians),1000*std::cos(heading*radians),100};
        const float nativeYaw = .2f, nativePitch = -.1f;
        const auto target = FocusProfile(normal,subject,nativeYaw,nativePitch);
        Require(std::abs(std::remainder(nativeYaw+target.rotation*radians-heading*radians,6.28318530718f)) <= 8*radians+.001f,
            "Lock loses the subject when walking past the old 90-degree focus limit");
        auto tuned = normal; tuned.rotation = 25; tuned.pitchOffset = 12;
        const auto adjusted = FocusProfile(tuned,subject,nativeYaw,nativePitch);
        Require(Near(std::remainder(adjusted.rotation-target.rotation,360.0f),25) && Near(adjusted.pitchOffset-target.pitchOffset,12),
            "Rotation/Pitch edits do not change the live bound view's angle");
    }

    // Tightness changes allowed framing drift without weakening full-heading
    // tracking, changing the authored angle, or adding frame-dependent state.
    for (float fov : {20.0f,80.0f,120.0f}) {
        auto authored = normal; authored.fov = fov;
        authored.rotation = 25; authored.pitchOffset = 12;
        for (float heading : {-179.0f,-90.0f,-1.0f,1.0f,90.0f,179.0f}) {
            const Point subject{1000*std::sin(heading*radians),1000*std::cos(heading*radians),150};
            const float subjectPitch = std::atan2(subject.z,std::hypot(subject.x,subject.y));
            float previousError = 1000;
            for (float tightness : {0.0f,.25f,.5f,.75f,1.0f}) {
                const auto target = FocusProfile(authored,subject,0,0,tightness);
                const float yawError = std::remainder((target.rotation-authored.rotation-heading)*radians,6.28318530718f);
                const float pitchError = (target.pitchOffset-authored.pitchOffset)/100-subjectPitch;
                const float error = std::hypot(yawError,pitchError);
                Require(error <= previousError+.0001f && error <= 20*radians+.0001f,
                    "Tightening the lock increases drift or the loose lock loses a subject behind the player");
                if (tightness == 1) Require(error < .0001f,"Maximum tightness leaves framing drift");
                Require(target.zoom == authored.zoom && target.fov == authored.fov &&
                    target.sideOffset == authored.sideOffset && target.height == authored.height,
                    "Lock tightness changes unrelated camera channels");
                previousError = error;
            }
        }
    }
    Require(FocusProfile(normal,{1,100,0},0,0,1).rotation > .5f,
        "A tight lock retains the loose lock's centered dead zone");
    Require(FocusProfile(normal,{500,1000,100},0,0,.5f) == focused,
        "Default tightness changes existing view framing");
    Require(FocusProfile(normal,{500,1000,100},0,0,std::numeric_limits<float>::quiet_NaN()) == focused &&
        FocusProfile(normal,{500,1000,100},0,0,std::numeric_limits<float>::infinity()) == focused,
        "Invalid live tightness corrupts camera targets");
    auto looseView = view, tightView = view;
    looseView.lockOnTightness = -1; tightView.lockOnTightness = 5;
    Require(Sanitize(looseView).lockOnTightness == 0 && Sanitize(tightView).lockOnTightness == 1,
        "Out-of-range lock tightness was not bounded");

    auto invalidProfile = view;
    invalidProfile.lockOnTightness = std::numeric_limits<float>::quiet_NaN();
    invalidProfile.profile.zoom = 10000; invalidProfile.profile.fov = std::numeric_limits<float>::quiet_NaN();
    invalidProfile.profile.transitionZoom = std::numeric_limits<float>::infinity();
    const auto safe = Sanitize(invalidProfile);
    Require(safe.lockOnTightness == kDefaultLockOnTightness,"Invalid saved lock tightness lost its default");
    Require(safe.profile.zoom == Defaults::Zoom.max && safe.profile.fov == normal.fov && safe.profile.transitionZoom == .5f,
        "Invalid entry channels escaped the normal third-person limits");
    // Entry and return use the same transition ownership helper as Vanity;
    // only spring targets change, never the gameplay entry's stored values.
    auto entry = normal; entry.zoom = 0; entry.fov = 55;
    entry.transitionSetZoom = entry.transitionSetFOV = true;
    entry.transitionZoom = .2f; entry.transitionFOV = .3f; entry.SyncTransitionOverride();
    VanityTransition transition;
    transition.Update(true,entry,normal,false);
    Require(transition.Source(normal).transitionZoom == .2f,"Entry lost its transition override");
    transition.Update(false,entry,normal,false);
    Require(transition.IsReturning() && transition.Source(normal).transitionFOV == .3f && normal.fov == 80,
        "Return lost its speed or changed the gameplay profile");
    transition.Update(false,entry,normal,true);
    Require(!transition.IsReturning() && &transition.Source(normal) == &normal,"Entry transition leaked after returning");

    for (const float fps : {30.0f,60.0f,144.0f,240.0f}) {
        ResumeGate gate;
        for (int frame=0; frame<static_cast<int>(fps); ++frame)
            Require(!gate.Step(true,1/fps),"Preview resumed while a menu still owned the controls");
        Require(!gate.Step(false,1/fps),"Menu-closing input could start or immediately cancel a preview");
        for (int frame=0; frame<static_cast<int>(fps*.25f); ++frame) gate.Step(false,1/fps);
        Require(gate.Step(false,1/fps),"Queued preview did not become ready after returning to gameplay");
        gate.Reset();
        Require(!gate.Step(false,1/fps),"Requeuing a preview skipped its resume delay");
        Require(!gate.Step(true,1/fps),"Reopening a menu left the resume gate ready");
    }

    struct SightHit { bool valid = true, hit = true; float fraction = .2f; int ref = 1; };
    constexpr float noMeshHit = std::numeric_limits<float>::infinity();
    const auto sightline = [&](std::span<const SightHit> hits, std::span<const float> meshes, int target = 9) {
        return ClearSightline<int>(100,
            [&](std::span<const int> ignored) {
                for (const auto& hit : hits)
                    if (std::find(ignored.begin(),ignored.end(),hit.ref) == ignored.end()) return hit;
                return SightHit{true,false,1,0};
            },
            [&](int ref) { return ref == target; },
            [&](int ref) -> std::optional<float> {
                if (ref <= 0 || static_cast<std::size_t>(ref) > meshes.size()) return std::nullopt;
                return meshes[ref-1];
            });
    };
    const std::array wordRock{SightHit{true,true,.2f,1}};
    Require(sightline(wordRock,std::array{noMeshHit}),"Oversized rock collision hid a visually exposed word effect");
    Require(sightline(wordRock,std::array{125.0f}),"Triangles beyond the focus point blocked the subject");
    Require(!sightline(wordRock,std::array{40.0f}),"A real wall between camera and subject was ignored");
    const std::array layered{SightHit{true,true,.2f,1},SightHit{true,true,.6f,2}};
    Require(!sightline(layered,std::array{noMeshHit,60.0f}),"Ignoring a coarse hull also ignored a real wall behind it");
    Require(sightline(layered,std::array{noMeshHit,noMeshHit}),"Two coarse hulls blocked a clear visual ray");
    Require(sightline(std::array{SightHit{true,true,.95f,0}},std::array<float,0>{}),"A surface focus point blocked itself");
    Require(sightline(wordRock,std::array{20.0f},1),"The subject's own geometry obstructed its focus point");
    Require(!sightline(std::array{SightHit{false,false,1,0}},std::array<float,0>{}),"Unavailable physics was treated as clear");
    Require(!sightline(wordRock,std::array<float,0>{}),"An unavailable mesh was silently ignored");
    const std::array tooMany{SightHit{true,true,.1f,1},SightHit{true,true,.2f,2},SightHit{true,true,.3f,3},
        SightHit{true,true,.4f,4},SightHit{true,true,.5f,5}};
    Require(!sightline(tooMany,std::array{noMeshHit,noMeshHit,noMeshHit,noMeshHit,noMeshHit}),"Visibility refinement exceeded its bounded query limit");

    History original{"one","two","different-preset-entry"}, restored{"old-save"};
    const auto bytes = EncodeHistory(original);
    Require(DecodeHistory(bytes,restored) && restored == original,"Encounter history did not survive co-save round trip");
    auto truncated = bytes; truncated.pop_back();
    Require(!DecodeHistory(truncated,restored) && restored == original,"Damaged history partially replaced the current save");
    Require(DecodeHistory({},restored) && restored.empty(),"New character inherited another character's encounters");
    Require(!DecodeHistory(std::array<std::uint8_t,2>{65,0},restored),"Oversize history ID accepted");

    namespace Crowd = CombatFraming;
    Require(Crowd::Pressure(std::array<float,1>{100}) == 0,"A single enemy widened camera");
    Require(Near(Crowd::Pressure(std::array<float,2>{100,500}),1.0f),"Second nearby enemy did not contribute to framing");
    Require(Crowd::Pressure(std::array<float,4>{100,200,300,400}) == 3,"Crowd count saturated before larger groups could contribute");
    Require(Crowd::Pressure(std::array<float,3>{100,1800,5000}) == 0,"Distant enemies widened camera");
    Require(Crowd::Pressure(std::array<float,3>{100,-2,std::numeric_limits<float>::quiet_NaN()}) == 0,"Invalid enemy distance changed framing");
    Require(Crowd::Pressure(std::array<float,3>{1500,100,1600}) < Crowd::Pressure(std::array<float,3>{300,100,400}),
        "Distant reinforcements count as much as close attackers");
    for (float intensity : {.25f,1.0f,3.0f}) {
        float previous = 0, previousGain = 1000;
        for (int enemies=2; enemies<=20; ++enemies) {
            const auto amount = Crowd::Evaluate({intensity,intensity},float(enemies-1));
            const float gain = amount.zoom-previous;
            Require(gain > 0 && gain < previousGain,"Additional enemies must keep contributing with diminishing returns");
            Require(amount.zoom <= 180 && amount.fov <= 30,"Large groups exceeded the framing limits");
            previous = amount.zoom; previousGain = gain;
        }
    }
    const auto crowdNormal = Crowd::Evaluate({1,1},3), strong = Crowd::Evaluate({3,3},3);
    Require(strong.zoom > crowdNormal.zoom && strong.fov > crowdNormal.fov,"Higher intensity weakened the same encounter");
    Require(strong.zoom/180 < crowdNormal.zoom/60,"Intensity failed to extend the crowd size over which framing builds");
    Require(Crowd::Evaluate({0,3},6).zoom == 0 && Crowd::Evaluate({3,0},6).fov == 0,
        "Disabling one framing channel disabled or leaked the other");
    float crowdSample = 0;
    for (const float fps : {30.0f,60.0f,144.0f,240.0f}) {
        Crowd::Envelope envelope;
        for (int frame=0; frame<static_cast<int>(fps); ++frame) envelope.Step(1,1/fps);
        if (crowdSample == 0) crowdSample = envelope.value;
        Require(Near(crowdSample,envelope.value),"Combat framing differs with frame rate");
        const float before = envelope.value;
        envelope.Step(0,1/fps);
        Require(std::abs(envelope.value-before) < .02f,"Enemy leaving caused an abrupt zoom");
        for (int frame=0; frame<static_cast<int>(fps*4); ++frame) envelope.Step(0,1/fps);
        Require(envelope.value < .0001f,"Combat framing did not return to tuned profile");
    }
    Require(Crowd::Sanitize({3,0}) == Crowd::Tuning{3,0} && Crowd::Sanitize({0,3}) == Crowd::Tuning{0,3},
        "Zoom and FOV cannot be enabled independently");
    Require(Crowd::Sanitize({std::numeric_limits<float>::quiet_NaN(),8}) == Crowd::Tuning{0,3},"Invalid framing intensity escaped sanitization");

    namespace Subjects = CinematicSubject;
    Require(Subjects::RayMeetsBound({0,0,0},{0,1,0},{250,1000,0},300),
        "Large structure missed because its origin is not under the aim ray");
    Require(Subjects::RayMeetsBound({0,0,0},{0,1,0},{0,100,0},2),"Small item under aim was rejected");
    Require(!Subjects::RayMeetsBound({0,0,0},{0,1,0},{300,1000,0},200) &&
        !Subjects::RayMeetsBound({0,0,0},{0,1,0},{0,-1000,0},50),"Off-ray or behind-camera object claimed an aim hit");
    Require(!Subjects::RayMeetsBound({0,0,0},{0,0,0},{0,1000,0},50),"Invalid aim direction selected a subject");
    Require(Subjects::ModelName("Dungeons\\Nordic\\WordWall01.nif") == "Word Wall01" &&
        Subjects::Fold("Word Wall01").find("word wall") != std::string::npos,"Unnamed word-wall mesh cannot be found by name");
    std::cout << "Cinematic Views and Crowd Modifier checks passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
