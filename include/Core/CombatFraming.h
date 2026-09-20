#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace DietDrCamera::CombatFraming
{
    struct Tuning {
        float zoomIntensity = 0, fovIntensity = 0;
        bool operator==(const Tuning&) const = default;
    };
    struct Adjustment {
        float zoom = 0, fov = 0;
    };
    inline constexpr float kMaxIntensity = 3;
    inline constexpr std::size_t kMaxSubjects = 64;
    inline Tuning Sanitize(Tuning tuning)
    {
        tuning.zoomIntensity = std::isfinite(tuning.zoomIntensity) ? std::clamp(tuning.zoomIntensity, 0.0f, kMaxIntensity) : 0;
        tuning.fovIntensity = std::isfinite(tuning.fovIntensity) ? std::clamp(tuning.fovIntensity, 0.0f, kMaxIntensity) : 0;
        return tuning;
    }
    // Count the encounter, not camera-frustum membership. Rotating the view or
    // widening FOV must not change its own cause and create a zoom feedback loop.
    inline float Pressure(std::span<const float> distances)
    {
        float total = 0, primary = 0;
        for (const float distance : distances) {
            if (!std::isfinite(distance) || distance < 0 || distance >= 1800) continue;
            const float x = std::clamp((distance - 600) / 1200, 0.0f, 1.0f);
            const float weight = 1 - x*x*(3 - 2*x);
            total += weight;
            primary = (std::max)(primary, weight);
        }
        // Keep the weighted count instead of saturating at four opponents.
        // One opponent leaves the tuned camera alone; reinforcements add to it.
        return std::clamp(total - primary, 0.0f, float(kMaxSubjects - 1));
    }
    inline Adjustment Evaluate(Tuning tuning, float pressure)
    {
        tuning = Sanitize(tuning);
        pressure = std::isfinite(pressure) ? std::clamp(pressure, 0.0f, float(kMaxSubjects - 1)) : 0;
        const auto response = [pressure](float intensity) {
            // Intensity increases both the ceiling and the crowd size over
            // which the effect builds. Every extra enemy has diminishing
            // influence; there is no fixed small-group saturation threshold.
            return intensity * -std::expm1(-pressure / (2.0f + intensity));
        };
        return {60.0f * response(tuning.zoomIntensity), 10.0f * response(tuning.fovIntensity)};
    }
    struct Envelope {
        float value = 0, velocity = 0;
        void Step(float target, float dt)
        {
            if (!std::isfinite(dt) || dt < 0) return;
            dt = (std::min)(dt, .05f);
            target = std::isfinite(target) ? std::clamp(target, 0.0f, float(kMaxSubjects - 1)) : 0;
            const float omega = target > value ? 7.0f : 3.5f;
            const float offset = value - target, impulse = velocity + omega*offset;
            const float decay = std::exp(-omega*dt);
            value = target + (offset + impulse*dt)*decay;
            velocity = (velocity - omega*impulse*dt)*decay;
            if (value < 0 || value > kMaxSubjects - 1) { value = std::clamp(value,0.0f,float(kMaxSubjects - 1)); velocity = 0; }
        }
    };
}
