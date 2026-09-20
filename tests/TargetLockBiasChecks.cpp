#include "Camera/TargetLockBias.h"
#include "Core/Spring.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <array>

using namespace DietDrCamera;

int main()
{
    try {
        const auto check = [](bool ok) { if (!ok) throw std::runtime_error("Target-lock bias checks failed"); };
        const auto Radians = TargetLockBias::PitchRadians;
        check(Radians(0, 0) == 0 && Radians(600, 1) == 0 && Radians(3000, 1) == 0);
        check(Radians(0, 1) > Radians(200, 1) && Radians(200, 1) > Radians(400, 1));
        check(Radians(100, -1) == -Radians(100, 1));
        check(std::abs(Radians(0, 1) - std::numbers::pi_v<float> / 12) < 1e-6f);
        check(Radians(0, 100) == Radians(0, 1.5f));
        check(Radians(0, std::numeric_limits<float>::quiet_NaN()) == 0);
        check(Radians(std::numeric_limits<float>::infinity(), 1) == 0);
        for (float dt : {1.0f / 30, 1.0f / 60, 1.0f / 144}) {
            float position = 0, velocity = 0;
            for (int i = 0; i < static_cast<int>(3 / dt); ++i)
                CriticalDampedSpringExact(position, velocity, Radians(100, 1), 6.0f, dt);
            check(position > 0 && position <= Radians(100, 1) + 1e-5f);
            for (int i = 0; i < static_cast<int>(3 / dt); ++i)
                CriticalDampedSpringExact(position, velocity, 0.0f, 6.0f, dt);
            check(std::abs(position) < 1e-5f);
        }
        using namespace TargetLockBias;
        struct Channel {
            float CameraProfile::* amount;
            bool CameraProfile::* enabled;
            float Offsets::* offset;
            float full;
        };
        const std::array channels{
            Channel{&CameraProfile::transitionHeightBias, &CameraProfile::transitionSetHeightBias, &Offsets::height, 50.0f},
            Channel{&CameraProfile::transitionZoomBias, &CameraProfile::transitionSetZoomBias, &Offsets::zoom, 10.0f},
            Channel{&CameraProfile::transitionFOVBias, &CameraProfile::transitionSetFOVBias, &Offsets::fov, 15.0f},
            Channel{&CameraProfile::transitionPitchBias, &CameraProfile::transitionSetPitchBias, &Offsets::pitch,
                    std::numbers::pi_v<float> / 12.0f}
        };
        for (const auto& channel : channels) {
            CameraProfile p;
            p.*channel.amount = 1.0f;
            check(Resolve(0, p) == Offsets{}); // stored tuning stays inactive
            p.*channel.enabled = true;
            check(p.ProximityBiasAnySet() && p.TransitionAnySet());
            Offsets expected;
            expected.*channel.offset = channel.full;
            check(Resolve(0, p) == expected); // other channels stay neutral
            check(Resolve(300, p).*channel.offset == channel.full * 0.5f);
            check(Resolve(600, p) == Offsets{} && Resolve(3000, p) == Offsets{});
            check(Resolve(std::numeric_limits<float>::infinity(), p) == Offsets{});
            check(Resolve(std::numeric_limits<float>::quiet_NaN(), p) == Offsets{});
            // Bias depends on target distance, not authored camera framing.
            p.height = 150; p.zoom = 90; p.fov = 120; p.pitchOffset = -20;
            check(Resolve(0, p) == expected);
            p.*channel.amount = -1.0f;
            check(Resolve(0, p).*channel.offset == -channel.full);
            p.*channel.amount = 20.0f;
            check(std::abs(Resolve(0, p).*channel.offset - channel.full * 1.5f) < 1e-5f);
            p.*channel.amount = std::numeric_limits<float>::quiet_NaN();
            check(Resolve(0, p) == Offsets{});

            for (const float dt : {1.0f / 30, 1.0f / 60, 1.0f / 144}) {
                Motion motion;
                for (int i = 0; i < static_cast<int>(3 / dt); ++i) {
                    const float previous = motion.value.*channel.offset;
                    motion.Step(expected, 6.0f, dt);
                    check(motion.value.*channel.offset >= previous && motion.value.*channel.offset <= channel.full);
                }
                check(std::abs(motion.value.*channel.offset - channel.full) < 0.001f);
                Offsets opposite;
                opposite.*channel.offset = -channel.full;
                motion.Step(opposite, 20.0f, dt); // switch target without a cut
                check(motion.value.*channel.offset > -channel.full);
                for (int i = 0; i < static_cast<int>(3 / dt); ++i) motion.Step(opposite, 20.0f, dt);
                check(std::abs(motion.value.*channel.offset + channel.full) < 0.001f);
                motion.Step({}, 6.0f, dt);
                check(motion.value.*channel.offset < 0.0f); // smooth release
                for (int i = 0; i < static_cast<int>(3 / dt); ++i) motion.Step({}, 6.0f, dt);
                check(std::abs(motion.value.*channel.offset) < 0.001f);
                motion = {};
                check(motion.value == Offsets{} && motion.velocity == Offsets{});
            }
        }
        // All four can run together, and clearing one does not reset the others.
        CameraProfile all;
        for (const auto& channel : channels) {
            all.*channel.enabled = true;
            all.*channel.amount = 1.0f;
        }
        Motion mixed;
        const auto target = Resolve(100, all);
        for (int i = 0; i < 180; ++i) mixed.Step(target, 6.0f, 1.0f / 60);
        all.transitionSetHeightBias = false;
        for (int i = 0; i < 180; ++i) mixed.Step(Resolve(100, all), 6.0f, 1.0f / 60);
        check(std::abs(mixed.value.height) < 0.001f && std::abs(mixed.value.zoom - target.zoom) < 0.001f &&
              std::abs(mixed.value.fov - target.fov) < 0.001f && std::abs(mixed.value.pitch - target.pitch) < 0.001f);
        mixed.velocity.fov = std::numeric_limits<float>::quiet_NaN();
        mixed.Step(target, 6.0f, 1.0f / 60);
        check(mixed.value.fov == 0 && mixed.velocity.fov == 0 && mixed.value.zoom > 0);
        std::cout << "Target-lock height/zoom/FOV/pitch bias, independence, switching and release checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
