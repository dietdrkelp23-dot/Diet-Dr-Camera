#include "Camera/FleeFraming.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;

struct Point { float x, y, z; };

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static bool Near(Point a, Point b)
{
    return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f && std::abs(a.z - b.z) < 0.01f;
}

struct Scene
{
    float tighten = 0.0f, velocity = 0.0f;

    Point Step(Point engine, Point loose, float approachSpeed, float strength, float dt)
    {
        FleeFraming::UpdateTighten(tighten, velocity,
            strength * FleeFraming::ApproachFactor(approachSpeed), dt);
        return FleeFraming::ApplyTighten(engine, loose, tighten);
    }
};

static void CheckThresholdDoesNotSnap()
{
    const Point engine{0, -240, 100};
    for (float fps : {30.0f, 60.0f, 120.0f, 240.0f}) {
        const float dt = 1.0f / fps;
        for (float lag : {45.0f, 140.0f, 600.0f}) {
            Scene scene;
            const Point loose{0, engine.y + lag, 100};
            Point previous = loose;
            for (int frame = 0; frame < static_cast<int>(fps * 2); ++frame) {
                const Point rendered = scene.Step(engine, loose, 150, 1, dt);
                const float move = previous.y - rendered.y;
                if (frame == 0) {
                    Require(move < lag * 0.08f,
                        "Crossing the approach threshold bypasses the spring and snaps the camera");
                }
                Require(move >= -0.001f && move <= lag * 5.0f * dt + 0.001f,
                    "Engagement reverses or jumps within its transition");
                Require(rendered.y >= engine.y - 0.001f, "Engagement overshoots the native camera");
                previous = rendered;
            }
            Require(Near(previous, engine), "Full strength never settles at normal framing");
            for (int frame = 0; frame < static_cast<int>(fps * 3); ++frame) {
                const Point rendered = scene.Step(engine, loose, 0, 1, dt);
                const float move = rendered.y - previous.y;
                if (frame == 0) Require(move < lag * 0.08f, "Ending approach snaps the camera back");
                Require(move >= -0.001f && move <= lag * 5.0f * dt + 0.001f,
                    "Release reverses or jumps within its transition");
                Require(rendered.y <= loose.y + 0.001f, "Release overshoots ordinary follow lag");
                previous = rendered;
            }
            Require(Near(previous, loose), "Ending flee movement leaves framing correction behind");
        }
    }
}

static void CheckMovementFromRest()
{
    float closest = 10000.0f;
    for (float fps : {30.0f, 60.0f, 120.0f, 240.0f}) {
        for (float speed : {90.0f, 150.0f, 300.0f, 600.0f, 800.0f}) {
            Scene scene;
            for (int frame = 0; frame < static_cast<int>(fps * 2); ++frame) {
                const float time = (frame + 1) / fps;
                const float playerY = -speed * time;
                const Point engine{0, playerY - 120.0f, 100};
                // Worst-case follow lag: the tracking anchor does not catch up
                // at all. The normal camera and player continue to move.
                const Point loose{0, -120, 100};
                const Point rendered = scene.Step(engine, loose, speed, 1, 1.0f / fps);
                const float clearance = playerY - rendered.y;
                closest = std::min(closest, clearance);
                Require(clearance >= 55.0f, "Ordinary approach from rest runs through the camera during engagement");
                if (frame == static_cast<int>(fps * 2) - 1) {
                    Require(Near(rendered, engine), "Walking/running approach never reaches normal framing");
                }
            }
        }
    }
    std::cout << "Minimum approach clearance from rest: " << closest << " units\n";
}

static void CheckStrengthAndOffsets()
{
    const Point engine{1060, -580, 340}, loose{1030, -540, 300};
    for (float speed : {-600.0f, -90.0f, 0.0f, 20.0f, 40.0f}) {
        Scene scene;
        Require(Near(scene.Step(engine, loose, speed, 1, 1.0f / 60.0f), loose),
            "Idle, sideways or departing movement engages flee framing");
    }
    Scene off;
    Require(Near(off.Step(engine, loose, 150, 0, 1.0f / 60.0f), loose),
        "Disabled flee framing changes the camera");
    Scene partial;
    Point rendered{};
    for (int frame = 0; frame < 180; ++frame) rendered = partial.Step(engine, loose, 150, 0.5f, 1.0f / 60.0f);
    Require(Near(rendered, Point{1045, -560, 320}), "Partial strength changes the shoulder/pitch blend");
    for (int frame = 0; frame < 180; ++frame) rendered = partial.Step(engine, loose, 150, 0, 1.0f / 60.0f);
    Require(Near(rendered, loose), "Disabling flee framing leaves a stale blend");
    const Point close{0, -25, 100};
    for (float blend : {0.0f, 0.5f, 1.0f}) {
        Require(Near(FleeFraming::ApplyTighten(close, close, blend), close),
            "Framing pushes an already-correct close/collision-limited camera outward");
    }
}

static void CheckRepeatedCrossings()
{
    const Point engine{0, -240, 100}, loose{0, -100, 100};
    for (float fps : {30.0f, 60.0f, 120.0f, 240.0f}) {
        Scene scene;
        Point previous = loose;
        for (int frame = 0; frame < static_cast<int>(fps * 4); ++frame) {
            // Alternate across both approach endpoints and include reversals.
            const float speeds[]{39, 41, 89, 91, 150, 0, -150, 45};
            const float dt = 1.0f / fps;
            const Point rendered = scene.Step(engine, loose, speeds[frame % 8], 1, dt);
            Require(std::abs(rendered.y - previous.y) <= 140.0f * 6.0f * dt + 0.001f,
                "Repeated threshold crossings reintroduce an instantaneous distance correction");
            Require(scene.tighten >= 0 && scene.tighten <= 1 && std::isfinite(rendered.y),
                "Rapid reversals destabilize flee framing");
            previous = rendered;
        }
    }
}

// Unlike the static blend checks, these scenes advance the actual following
// anchor. A hidden offset can be thousands of units during a movement shout.
struct MovingScene
{
    Point player{}, lagged{};
    FleeFraming::FollowState flee;
    float smoothedSpeed = 0.0f;

    Point Step(Point motion, float strength, float dt, float looseness = 1.0f,
               std::uint64_t movementImpulse = 0)
    {
        player.x += motion.x * dt;
        player.y += motion.y * dt;
        player.z += motion.z * dt;
        const float speed = std::sqrt(motion.x * motion.x + motion.y * motion.y + motion.z * motion.z);
        smoothedSpeed += (std::min(speed, 6000.0f) - smoothedSpeed) * (1.0f - std::exp(-dt / 0.25f));
        const Point delta{player.x - lagged.x, player.y - lagged.y, player.z - lagged.z};
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        const float leashSpeed = std::max(smoothedSpeed, 600.0f);
        const float leashStart = leashSpeed * (500.0f / 600.0f);
        const float leashEnd = leashSpeed * (2000.0f / 600.0f);
        const float baseRate = std::pow(0.01f, std::sqrt(looseness));
        const float rate = baseRate + (1.0f - baseRate) *
            std::clamp((distance - leashStart) / (leashEnd - leashStart), 0.0f, 1.0f);
        const float lambda = 1.0f - std::pow(1.0f - rate, dt * 60.0f);
        lagged.x += delta.x * lambda;
        lagged.y += delta.y * lambda;
        lagged.z += delta.z * lambda;

        // The current native hook caps velocity to 800 before projecting it.
        const float approach = -motion.y * (speed > 800.0f ? 800.0f / speed : 1.0f);
        flee.Update(lagged, player, approach, speed, strength, dt, movementImpulse);
        return FleeFraming::ApplyTighten(player, lagged, flee.tighten);
    }
};

static void CheckHiddenTrailIsNotRestored()
{
    for (float fps : {30.0f, 60.0f, 120.0f, 144.0f, 240.0f}) {
        MovingScene scene;
        const float dt = 1.0f / fps;
        for (int frame = 0; frame < static_cast<int>(fps * 3); ++frame)
            scene.Step(Point{0, -3000, 0}, 1, dt);
        const Point held = FleeFraming::ApplyTighten(scene.player, scene.lagged, scene.flee.tighten);
        Require(Near(held, scene.player), "Fast approach does not settle at the ordinary camera");
        Require(std::abs(scene.lagged.y - scene.player.y) > 1000,
            "Release regression did not build a substantial hidden trail");
        for (int frame = 0; frame < static_cast<int>(fps * 4); ++frame) {
            const Point rendered = scene.Step(Point{}, 1, dt);
            Require(std::abs(rendered.y - held.y) < 0.1f,
                "Ending approach restores the hidden follow trail and throws the player out of frame");
        }
        Require(scene.flee.tighten < 0.001f, "Stopped motion keeps flee framing engaged");
    }
}

static void CheckFastTurnStaysInFrame()
{
    float largestOffset = 0.0f;
    for (float fps : {30.0f, 60.0f, 120.0f, 144.0f, 240.0f}) {
        for (float speed : {1500.0f, 3000.0f, 6000.0f}) {
            for (float angle : {90.0f, 110.0f, 145.0f, 180.0f}) {
                MovingScene scene;
                const float dt = 1.0f / fps;
                for (int frame = 0; frame < static_cast<int>(fps * 3); ++frame)
                    scene.Step(Point{0, -speed, 0}, 1, dt);
                const float radians = angle * 3.14159265358979323846f / 180.0f;
                Point previous{};
                for (int frame = 0; frame < static_cast<int>(fps * 8); ++frame) {
                    const float time = frame * dt;
                    const float remaining = std::clamp(1.0f - (time - 0.45f) / 0.8f, 0.0f, 1.0f);
                    const Point motion{std::sin(radians) * speed * remaining,
                                       -std::cos(radians) * speed * remaining, 0};
                    const Point rendered = scene.Step(motion, 1, dt);
                    const Point offset{rendered.x - scene.player.x, rendered.y - scene.player.y, 0};
                    largestOffset = std::max(largestOffset, std::sqrt(offset.x * offset.x + offset.y * offset.y));
                    // A centered actor at a close 120-unit camera distance and
                    // 40-degree horizontal FOV must remain in front and visible.
                    const float depth = 120.0f - offset.y;
                    if (!(depth > 60.0f && std::abs(offset.x) < depth * 0.36397f))
                        std::cerr << "Fast-turn failure: fps=" << fps << " speed=" << speed
                                  << " angle=" << angle << " time=" << time << " tighten=" << scene.flee.tighten
                                  << " offset=(" << offset.x << "," << offset.y << ")\n";
                    Require(depth > 60.0f && std::abs(offset.x) < depth * 0.36397f,
                        "A fast turnaround releases flee protection while the camera is still unsafe");
                    const float previousX = previous.x;
                    previous = offset;
                    if (frame > 0) Require(std::abs(offset.x - previousX) < 250.0f * dt,
                        "Fast-turn release introduces a lateral kick");
                }
                Require(scene.flee.tighten < 0.001f, "Fast-turn protection never releases after settling");
                // At these large world coordinates, the ordinary float follow
                // integrator stops below a fraction of a unit of error.
                Require(std::abs(scene.lagged.x - scene.player.x) < 0.5f &&
                        std::abs(scene.lagged.y - scene.player.y) < 0.5f,
                    "Fast-turn recovery leaves a stale follow offset");
            }
        }
    }
    std::cout << "Maximum follow offset through fast turns: " << largestOffset << " units\n";
}

static void CheckReleaseKeepsVisiblePosition()
{
    const Point player{1200, -3400, 570};
    const Point engine{1320, -3290, 650};
    const Point originalLag{-700, -1500, 1470};
    for (float previous : {0.15f, 0.5f, 0.9f, 0.9999f, 1.0f}) {
        for (float next : {0.0f, previous * 0.5f, previous * 0.999f}) {
            Point lag = originalLag;
            FleeFraming::RetireHiddenLag(lag, player, previous, next);
            for (float otherFade : {0.0f, 0.3f, 1.0f}) {
                const auto Camera = [&](Point anchor) {
                    return Point{engine.x + (anchor.x - player.x) * otherFade,
                                 engine.y + (anchor.y - player.y) * otherFade,
                                 engine.z + (anchor.z - player.z) * otherFade};
                };
                Require(Near(FleeFraming::ApplyTighten(engine, Camera(originalLag), previous),
                             FleeFraming::ApplyTighten(engine, Camera(lag), next)),
                    "Releasing hidden lag moves the visible camera or breaks lock/dialogue composition");
            }
        }
    }
    Point unchanged = originalLag;
    FleeFraming::RetireHiddenLag(unchanged, player, 0.2f, 0.8f);
    Require(Near(unchanged, originalLag), "Engagement changes the configured follow tracker");
}

static void CheckMomentumEligibilityAndReset()
{
    const Point player{};
    for (float approach : {-800.0f, 0.0f, 40.0f}) {
        FleeFraming::FollowState state;
        Point lag{3000, 0, 0};
        for (int frame = 0; frame < 240; ++frame) state.Update(lag, player, approach, 6000, 1, 1.0f / 60.0f);
        Require(state.tighten == 0 && state.heldApproach == 0 && Near(lag, Point{3000, 0, 0}),
            "High speed alone engages flee framing or alters ordinary sideways/away looseness");
    }
    for (float strength : {0.0f, 0.25f, 0.5f, 1.0f}) {
        FleeFraming::FollowState state;
        Point lag{3000, 1000, 400};
        for (int frame = 0; frame < 240; ++frame) state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f);
        for (int frame = 0; frame < 120; ++frame) {
            state.Update(lag, player, -800, 3000, strength, 1.0f / 60.0f);
            Require(std::abs(state.tighten - strength) < 0.001f,
                "Fast-turn hold changes the authored partial/off strength");
        }
        const Point beforeReset = FleeFraming::ApplyTighten(player, lag, state.tighten);
        FleeFraming::RetireHiddenLag(lag, player, state.tighten, 0);
        state.Reset();
        Require(Near(beforeReset, lag), "Clearing a camera handoff reveals hidden lag");
        state.Update(lag, player, 0, 6000, strength, 1.0f / 60.0f);
        Require(state.tighten == 0 && state.heldApproach == 0,
            "A reset carries the previous view's approach into unrelated fast motion");

        for (int frame = 0; frame < 240; ++frame) state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f);
        for (int frame = 0; frame < 240; ++frame) state.Update(lag, player, 800, 3000, 0, 1.0f / 60.0f);
        Require(state.tighten < 0.001f && state.heldApproach == 0,
            "Disabling flee framing retains its fast-motion hold");
    }

    FleeFraming::FollowState ordinary;
    Point lag{0, 1000, 0};
    for (int frame = 0; frame < 180; ++frame) ordinary.Update(lag, player, 600, 600, 1, 1.0f / 60.0f);
    ordinary.Update(lag, player, -600, 600, 1, 1.0f / 60.0f);
    Require(ordinary.tighten < 0.999f && ordinary.heldApproach == 0,
        "Ordinary sprint reversals acquire the fast-motion hold");
    const auto before = ordinary;
    const Point lagBefore = lag;
    for (float dt : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()})
        ordinary.Update(lag, player, 800, 6000, 1, dt);
    Require(ordinary.tighten == before.tighten && ordinary.velocity == before.velocity &&
            ordinary.heldApproach == before.heldApproach && Near(lag, lagBefore),
        "Invalid or zero frame time advances flee framing");
    ordinary.Update(lag, player, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1.0f / 60.0f);
    Require(std::isfinite(ordinary.tighten) && std::isfinite(lag.x), "Invalid input poisons flee framing");
}

static void CheckUnevenFramesAndShoutExit()
{
    MovingScene scene;
    for (int frame = 0; frame < 180; ++frame) scene.Step(Point{0, -3000, 0}, 1, 1.0f / 60.0f);
    const float steps[]{1.0f / 240.0f, 1.0f / 30.0f, 1.0f / 144.0f, 1.0f / 60.0f, 0.05f};
    float time = 0.0f;
    int frame = 0;
    while (time < 8.0f) {
        const float dt = steps[frame++ % 5];
        const float speed = 3000.0f * std::clamp(1.0f - (time - 0.4f) / 0.8f, 0.0f, 1.0f);
        // The shout exits into the ordinary 0.15-looseness profile while the
        // player still has momentum; production eases this change over 0.4s.
        const float looseness = 0.15f + 0.85f * std::exp(-time / 0.4f);
        const Point camera = scene.Step(Point{speed, 0, 0}, 1, dt, looseness);
        const float depth = 120.0f - (camera.y - scene.player.y);
        Require(depth > 60.0f && std::abs(camera.x - scene.player.x) < depth * 0.36397f,
            "Uneven frames or the shout-profile exit reveal an unsafe follow offset");
        time += dt;
    }
    Require(scene.flee.tighten < 0.001f && Near(scene.player, scene.lagged),
        "Shout exit leaves flee framing or its trail behind");
}

static void CheckChainedForwardDashReleases()
{
    for (float fps : {30.0f, 60.0f, 120.0f, 144.0f, 240.0f}) {
        const float dt = 1.0f / fps;
        for (float speed : {1500.0f, 3000.0f, 6000.0f}) {
            for (float gap : {0.0f, 0.04f, 0.15f}) {
                MovingScene scene;
                for (int frame = 0; frame < static_cast<int>(fps * 3); ++frame)
                    scene.Step(Point{0, -speed, 0}, 1, dt, 1, 1);
                for (int frame = 0; frame < static_cast<int>(fps * gap); ++frame)
                    scene.Step(Point{}, 1, dt, 1, 1);
                float expectedTighten = scene.flee.tighten;
                float expectedVelocity = scene.flee.velocity;
                for (int frame = 0; frame < static_cast<int>(fps * 3); ++frame) {
                    // A new windup/fire sequence while speed remains high.
                    // Later forward shouts must not restart the old hold.
                    const auto impulse = static_cast<std::uint64_t>(2 + frame / static_cast<int>(fps * 0.2f));
                    const Point rendered = scene.Step(Point{0, speed, 0}, 1, dt, 1, impulse);
                    FleeFraming::UpdateTighten(expectedTighten, expectedVelocity, 0, dt);
                    Require(std::abs(scene.flee.tighten - expectedTighten) < 0.0001f &&
                            scene.flee.heldApproach == 0,
                        "A new forward dash retains or restarts protection instead of using the release spring");
                    if (frame >= static_cast<int>(fps)) {
                        Require(scene.flee.tighten < 0.02f,
                            "A chained forward Whirlwind Sprint inherits the previous dash's flee hold");
                    }
                    const float depth = 120.0f - (rendered.y - scene.player.y);
                    Require(depth >= 119.9f && std::abs(rendered.x - scene.player.x) < 0.1f,
                        "Releasing the hold on a forward dash reveals the old trail in front of the player");
                }
                Require(scene.flee.tighten < 0.001f,
                    "Repeated forward dashes keep flee framing active at sustained high speed");
                Require(scene.player.y - scene.lagged.y > 500.0f,
                    "The new forward dash does not regain its configured camera looseness");
            }
        }
    }
}

static void CheckMovementImpulseLifecycle()
{
    FleeFraming::MovementImpulseTracker events;
    events.Observe(0, 0);
    Require(events.serial == 0, "No shout produces a movement boundary");
    events.Observe(100, 0);
    const auto windup = events.serial;
    Require(windup != 0, "A new movement shout has no windup boundary");
    events.Observe(100, 90);
    Require(events.serial == windup, "A stale launch belongs to the new windup");
    events.Observe(100, 110);
    const auto launched = events.serial;
    Require(launched != windup, "The launch does not clear approach reacquired during windup");
    for (std::uint64_t duplicate : {110, 111, 115, 125}) events.Observe(100, duplicate);
    Require(events.serial == launched, "Duplicate graph/SKSE fire reports restart the movement hold");
    events.Observe(0, 0);
    Require(events.serial == launched, "Ending a shout discards its residual-momentum protection");
    events.Observe(200, 210);
    Require(events.serial != launched, "A back-to-back shout missed between camera samples has no boundary");

    for (float strength : {0.0f, 0.25f, 0.5f, 1.0f}) {
        FleeFraming::FollowState state;
        Point player{}, lag{3000, 1000, 400};
        for (int frame = 0; frame < 180; ++frame)
            state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f, 1);
        const Point before = FleeFraming::ApplyTighten(player, lag, state.tighten);
        // The windup sample still contains the previous dash's approach.
        state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f, 2);
        Require(state.heldApproach == 0,
            "The boundary sample immediately re-arms the previous dash's hold");
        Require(Near(before, FleeFraming::ApplyTighten(player, lag, state.tighten)),
            "A new shout resets the visible framing instead of preserving the transition");
        // Residual approach can legitimately recur during the new windup.
        state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f, 2);
        Require(strength == 0 || state.heldApproach > 0,
            "The new dash cannot earn its own approach protection");
        float expected = state.tighten, expectedVelocity = state.velocity;
        for (int frame = 0; frame < 180; ++frame) {
            state.Update(lag, player, -800, 6000, strength, 1.0f / 60.0f, 3);
            FleeFraming::UpdateTighten(expected, expectedVelocity, 0, 1.0f / 60.0f);
            Require(std::abs(state.tighten - expected) < 0.0001f && state.heldApproach == 0,
                "The forward launch keeps the windup's old approach or changes partial/off strength");
        }
        // A later true approach must still engage, including its fast turn.
        for (int frame = 0; frame < 180; ++frame)
            state.Update(lag, player, 800, 3000, strength, 1.0f / 60.0f, 4);
        for (int frame = 0; frame < 30; ++frame)
            state.Update(lag, player, 0, 3000, strength, 1.0f / 60.0f, 4);
        Require(std::abs(state.tighten - strength) < 0.001f,
            "A fresh approach loses protection on its own fast turn");
        const auto previousImpulse = state.lastMovementImpulse;
        for (float dt : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()})
            state.Update(lag, player, -800, 3000, strength, dt, 5);
        Require(state.lastMovementImpulse == previousImpulse,
            "Invalid frame time consumes the new movement boundary before it can be applied");
    }
}

int main()
{
    try {
        CheckThresholdDoesNotSnap();
        CheckMovementFromRest();
        CheckStrengthAndOffsets();
        CheckRepeatedCrossings();
        CheckHiddenTrailIsNotRestored();
        CheckFastTurnStaysInFrame();
        CheckReleaseKeepsVisiblePosition();
        CheckMomentumEligibilityAndReset();
        CheckUnevenFramesAndShoutExit();
        CheckChainedForwardDashReleases();
        CheckMovementImpulseLifecycle();
        std::cout << "Flee framing checks passed (smooth engagement/release, hidden trail, fast turns, chained shouts, strength, handoffs)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Flee framing check failed: " << e.what() << '\n';
        return 1;
    }
}
