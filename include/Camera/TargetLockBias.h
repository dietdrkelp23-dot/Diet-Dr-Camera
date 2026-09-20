#pragma once

#include "Core/Spring.h"
#include "Settings/CameraProfile.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace DietDrCamera::TargetLockBias
{
    inline constexpr float kMin = -1.5f;
    inline constexpr float kMax = 1.5f;

    // All channels use horizontal target distance, reach full strength at
    // contact, and fade to zero at 600 game units. Values are additive to the
    // resolved profile; height never changes pitch and zoom never changes FOV.
    inline float Amount(float distance, float bias)
    {
        if (!std::isfinite(distance) || !std::isfinite(bias)) return 0.0f;
        const float close = 1.0f - std::clamp(distance / 600.0f, 0.0f, 1.0f);
        const float weight = close * close * (3.0f - 2.0f * close);
        return std::clamp(bias, kMin, kMax) * weight;
    }

    inline float PitchRadians(float distance, float bias)
    {
        return Amount(distance, bias) * (std::numbers::pi_v<float> / 12.0f);
    }

    struct Offsets
    {
        float height = 0.0f;  // game units; positive raises the camera
        float zoom = 0.0f;    // profile slider units; positive pulls back
        float fov = 0.0f;     // degrees; positive widens the view
        float pitch = 0.0f;   // radians; positive looks up
        bool operator==(const Offsets&) const = default;
    };

    inline Offsets Resolve(float distance, const CameraProfile& profile)
    {
        return {
            profile.transitionSetHeightBias ? Amount(distance, profile.transitionHeightBias) * 50.0f : 0.0f,
            profile.transitionSetZoomBias ? Amount(distance, profile.transitionZoomBias) * 10.0f : 0.0f,
            profile.transitionSetFOVBias ? Amount(distance, profile.transitionFOVBias) * 15.0f : 0.0f,
            profile.transitionSetPitchBias ? PitchRadians(distance, profile.transitionPitchBias) : 0.0f
        };
    }

    struct Motion
    {
        Offsets value{};
        Offsets velocity{};

        // Step even after lock is lost, toward neutral, so releasing the target
        // or disabling a row eases out instead of leaving an offset or snapping.
        void Step(const Offsets& target, float omega, float dt)
        {
            const auto step = [omega, dt](float& position, float& speed, float desired) {
                CriticalDampedSpringExact(position, speed, desired, omega, dt);
                if (!std::isfinite(position) || !std::isfinite(speed)) position = speed = 0.0f;
            };
            step(value.height, velocity.height, target.height);
            step(value.zoom, velocity.zoom, target.zoom);
            step(value.fov, velocity.fov, target.fov);
            step(value.pitch, velocity.pitch, target.pitch);
        }
    };
}
