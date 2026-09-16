#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace DietDrCamera
{
    // TDM's settled camera pitch is a softened elevation angle. Recover the
    // opening focus height from that pitch, then follow the same focus as the
    // camera moves during Quick Tune. This preserves the user's existing TDM
    // vertical offset without reaching into the other plugin's private state.
    class TargetLockPreview
    {
    public:
        struct Point { float x{}, y{}, z{}; };

        bool Capture(Point camera, Point target, float pitch)
        {
            _valid = false;
            if (!Finite(camera) || !Finite(target) || !std::isfinite(pitch)) return false;
            // The monotonic half of TDM's curve ends at pi/4; near its end the
            // inverse projects to infinity. Keep the existing hold in that case.
            if (std::abs(pitch) >= kPi * 0.25f - 0.001f) return false;
            const float distance = std::hypot(target.x - camera.x, target.y - camera.y);
            if (distance < 1.0f) return false;
            // Rationalized quadratic inverse avoids cancellation near zero.
            const float magnitude = 2.0f * std::abs(pitch) /
                (1.0f + std::sqrt(1.0f - 4.0f * std::abs(pitch) / kPi));
            const float elevation = -std::copysign(magnitude, pitch);
            _focusHeight = camera.z + distance * std::tan(elevation) - target.z;
            _valid = std::isfinite(_focusHeight);
            return _valid;
        }

        [[nodiscard]] std::optional<float> Pitch(Point camera, Point target) const
        {
            if (!_valid || !Finite(camera) || !Finite(target)) return std::nullopt;
            const float distance = std::hypot(target.x - camera.x, target.y - camera.y);
            if (distance < 1.0f) return std::nullopt;
            const float elevation = std::atan2(target.z + _focusHeight - camera.z, distance);
            return -elevation * (1.0f - std::abs(elevation) / kPi);
        }

    private:
        static constexpr float kPi = 3.14159265358979323846f;
        static bool Finite(Point p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        }
        float _focusHeight{};
        bool _valid{};
    };
}
