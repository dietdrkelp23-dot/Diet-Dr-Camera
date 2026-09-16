#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace DietDrCamera::TargetLockPitchBias
{
    inline constexpr float kMin = -1.5f;
    inline constexpr float kMax = 1.5f;

    // Signed additional pitch: positive looks up, negative looks down.
    // Full strength at contact, smoothly fading to zero at 600 game units.
    // Independent of camera height, shoulder, zoom and the target's elevation.
    inline float Radians(float distance, float bias)
    {
        if (!std::isfinite(distance) || !std::isfinite(bias)) return 0.0f;
        const float close = 1.0f - std::clamp(distance / 600.0f, 0.0f, 1.0f);
        const float weight = close * close * (3.0f - 2.0f * close);
        return std::clamp(bias, kMin, kMax) * weight * (std::numbers::pi_v<float> / 12.0f);
    }
}
