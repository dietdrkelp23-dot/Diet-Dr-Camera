#include "Camera/MountedTargetLock.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace DietDrCamera;
using Q = MountedTargetLock::Quaternion;
constexpr float kPi = 3.14159265358979323846f;

static void Check(bool ok, const char* message)
{
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static Q Multiply(Q a, Q b)
{
    return {a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w};
}

static Q Rotation(float yaw, float pitch, float roll)
{
    return Multiply(Multiply({std::cos(yaw/2), 0, 0, std::sin(yaw/2)},
                             {std::cos(pitch/2), std::sin(pitch/2), 0, 0}),
                             {std::cos(roll/2), 0, std::sin(roll/2), 0});
}

static float Heading(Q q)
{
    return std::atan2(2*(q.x*q.y-q.w*q.z), 1-2*(q.x*q.x+q.z*q.z));
}

int main()
{
    // Reproduce the mounted chain: DDC supplies a world-bearing offset, then
    // TDM preserves the view across the horse's heading change. The rider can
    // turn independently. Neither rider motion nor the TDM compensation may
    // perturb the bearing after DDC's post-UpdateRotation correction.
    for (const int fps : {30, 60, 144, 240}) {
        float previousHorse = 0.0f;
        MountedTargetLock filter;
        double legacyError = 0.0;
        for (int i = 0; i < fps * 3; ++i) {
            const float t = float(i) / fps;
            const float horse = std::remainder(2.0f*t, 2*kPi);
            const float rider = horse + .18f*std::sin(2*kPi*4*t);
            const float desired = std::remainder(3.0f + .2f*t, 2*kPi);
            const float freeYawBeforeTDM = desired - rider;
            const float tdmYaw = previousHorse + freeYawBeforeTDM;
            const float pitch = .12f + .03f*std::sin(2*kPi*5*t);
            Q fresh = Rotation(-tdmYaw, pitch, .25f);
            legacyError += std::abs(std::remainder(Heading(fresh)-desired, 2*kPi));
            Q fixed = MountedTargetLock::AimYaw(fresh, desired);
            Check(std::abs(std::remainder(Heading(fixed)-desired, 2*kPi)) < .00001f,
                "horse/rider heading leaked through the mounted chain");
            Check(std::abs(*MountedTargetLock::Elevation(fixed)-pitch) < .00001f,
                "yaw correction changed TDM pitch");
            const float freeYawAfter = MountedTargetLock::RelativeYaw(desired, horse);
            Check(std::abs(std::remainder(horse+freeYawAfter-desired, 2*kPi)) < .00001f,
                "mounted offset was not rebased for the next TDM frame");
            const float nextHorse = horse + .06f;
            const float releasedOffset = freeYawAfter + horse - nextHorse;
            Check(std::abs(std::remainder(nextHorse+releasedOffset-desired, 2*kPi)) < .00001f,
                "unlock did not preserve the visible bearing through a horse turn");
            const float correction = filter.Sample(i, pitch, true, 1.0f/fps);
            const Q cached = MountedTargetLock::Apply(fixed, correction);
            Check(std::abs(std::remainder(Heading(cached)-desired, 2*kPi)) < .00001f,
                "combined stabilization disturbed the target bearing");
            // Engine placement and rendering query the SAME corrected cache.
            // Repeated fresh UpdateRotation calls must also use one time step.
            const Q repeated = MountedTargetLock::Apply(fixed, filter.Sample(i, pitch, true, 1.0f/fps));
            Check(std::abs(*MountedTargetLock::Elevation(repeated)-*MountedTargetLock::Elevation(cached)) < .00001f,
                "repeated rotation evaluation doubled the pitch correction");
            previousHorse = horse;
        }
        Check(legacyError/(fps*3) > .08f, "fixture did not reproduce the original heading error");
    }

    for (const int fps : {30, 60, 120, 144, 240}) {
        const float dt = 1.0f / fps;
        MountedTargetLock filter;
        double rawEnergy = 0, filteredEnergy = 0;
        // Two degrees of stride bob at 5 Hz around a steady target elevation.
        for (int i = 0; i < fps * 5; ++i) {
            const float bob = 0.034906585f * std::sin(2*kPi*5*i*dt);
            const float raw = 0.15f + bob;
            const float offset = filter.Sample(i, raw, true, dt);
            Check(std::abs(offset) <= MountedTargetLock::kMaxCorrection, "unbounded bob correction");
            if (i > fps) {
                rawEnergy += bob*bob;
                const float output = raw + offset - 0.15f;
                filteredEnergy += output*output;
            }
            Check(filter.Sample(i, raw, true, dt) == offset,
                "repeated rotation update advanced the filter twice");
        }
        const double amplitudeRatio = std::sqrt(filteredEnergy/rawEnergy);
        std::cout << fps << " FPS: bob amplitude retained " << amplitudeRatio << '\n';
        Check(amplitudeRatio < 0.50, "insufficient stride-bob attenuation");

        // A hill or moving target demands a real, sustained elevation change.
        // The view may lag by at most 2.5 degrees, never a full extra transition.
        for (int i = 0; i < fps; ++i) {
            const float raw = 0.15f + 0.5f * i * dt;
            const float offset = filter.Sample(fps*5+i, raw, true, dt);
            Check(std::isfinite(offset) && std::abs(offset) <= MountedTargetLock::kMaxCorrection,
                "sustained tracking became sluggish or non-finite");
        }
        for (int i = 0; i < fps; ++i) filter.Sample(fps*6+i, 0.65f, true, dt);
        Check(std::abs(filter.Sample(fps*7, 0.65f, true, dt)) < 0.0001f,
            "steady target retains a framing offset");

        filter.Sample(fps*7+1, 0.7f, true, dt);
        const float paused = filter.Sample(fps*7+2, 0.7f, true, 0);
        Check(filter.Sample(fps*7+3, 0.7f, false, 0) == paused, "pause changed camera correction");
        float previous = paused;
        for (int i = 0; i < fps; ++i) {
            const float next = filter.Sample(fps*8+i, 0.9f, false, dt);
            Check(std::abs(next) <= std::abs(previous) + 0.000001f && next*previous >= 0,
                "switch/unlock handoff overshot instead of retiring correction");
            previous = next;
        }
        Check(previous == 0, "correction leaked after unlock/dismount");
        Check(filter.Sample(fps*9, -0.3f, true, dt) == 0, "new tracking session reused stale pitch");
        filter.Sample(fps*9+1, 0.3f, true, dt);
        Check(filter.Sample(fps*9+2, 0.1f, true, 2.0f) == 0, "long frame chased stale geometry");
        filter.Reset();
        Check(filter.Sample(fps*9+3, 0.4f, false, dt) == 0, "reset leaked an output correction");
    }

    // Independent quaternion geometry: pitch correction must not change the
    // target's horizontal bearing, even when the engine supplied camera roll.
    for (const float yaw : {-3.14f, -2.0f, 0.0f, 2.0f, 3.14f}) {
        for (const float pitch : {-1.2f, -0.4f, 0.0f, 0.5f, 1.2f}) {
            for (const float roll : {-0.6f, 0.0f, 0.6f}) {
                const Q raw = Rotation(yaw, pitch, roll);
                Check(std::abs(*MountedTargetLock::Elevation(raw) - pitch) < 0.00001f,
                    "raw elevation convention is wrong");
                for (const float offset : {-0.04f, 0.0f, 0.04f}) {
                    const Q changed = MountedTargetLock::Apply(raw, offset);
                    const float difference = std::remainder(Heading(changed)-Heading(raw), 2*kPi);
                    Check(std::abs(difference) < 0.00001f, "pitch stabilization changed yaw");
                    Check(std::abs(*MountedTargetLock::Elevation(changed)-pitch-offset) < 0.00001f,
                        "output did not apply the requested pitch correction");
                    const float norm = changed.w*changed.w+changed.x*changed.x+changed.y*changed.y+changed.z*changed.z;
                    Check(std::abs(norm-1) < 0.00001f, "quaternion lost normalization");
                }
            }
        }
    }
    MountedTargetLock invalid;
    invalid.Sample(1, 0, true, .016f);
    invalid.Sample(2, .02f, true, .016f);
    Check(invalid.Sample(3, std::numeric_limits<float>::quiet_NaN(), true, .016f) == 0,
        "invalid pitch retained correction");
    Check(!MountedTargetLock::Elevation({0, 0, 0, 0}), "zero quaternion accepted");
    Check(!MountedTargetLock::Elevation(Rotation(0, kPi/2, 0)), "vertical singularity accepted");
    Check(MountedTargetLock::TrackingDuration(.2f, false) == .2f, "on-foot tracking changed");
    Check(MountedTargetLock::TrackingDuration(.2f, true) == .3f, "mounted damping floor missing");
    Check(MountedTargetLock::TrackingDuration(.8f, true) == .8f, "custom looseness was overridden");
    std::cout << "Mounted target-lock checks passed: bob rejection at 30-240 FPS, bounded lag, handoffs, pause/reset, quaternion geometry\n";
}
