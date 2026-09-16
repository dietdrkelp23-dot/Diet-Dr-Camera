#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DietDrCamera
{
    // Shared death/ragdoll look driver. NiMatrix3 columns are right, forward,
    // and up. Yaw turns about world Z; pitch changes forward elevation. Roll
    // is captured separately so it cannot couple the two control axes.
    class BleedoutFreeLook
    {
    public:
        struct Input {
            std::int16_t stickX{}, stickY{};
            double mouseX{}, mouseY{};
        };
        struct Stick { double x{}, y{}; };

        static constexpr double kPitchLimit = 1.48;
        static constexpr double kStickSpeed = 2.7;  // rad/real second; old 60 FPS speed
        static constexpr double kMouseSpeed = 0.003;  // rad/device count, never time-scaled

        static Stick FilterStick(std::int16_t x, std::int16_t y)
        {
            constexpr double deadzone = 8689.0;  // XInput right thumb default
            const double magnitude = std::hypot(double(x), double(y));
            if (magnitude <= deadzone) return {};
            const double amount = std::clamp((magnitude - deadzone) / (32767.0 - deadzone), 0.0, 1.0);
            return {x / magnitude * amount, y / magnitude * amount};
        }

        void Reset() { *this = {}; }
        [[nodiscard]] bool IsActive() const { return _active; }
        [[nodiscard]] double Yaw() const { return _yaw; }
        [[nodiscard]] double Pitch() const { return _pitch; }
        [[nodiscard]] double Roll() const { return _roll; }

        void Update(float matrix[3][3], const Input& input, double now, bool inputAllowed)
        {
            if (!std::isfinite(now)) return;
            if (!_active) {
                if (!Capture(matrix)) return;
                _lastTime = now;
            }
            const double elapsed = now - _lastTime;
            _lastTime = now;  // also advance while blocked; menus never bank time
            // Discard the first sample on entry/resume (including queued mouse
            // counts). A long gap can be a paused engine or a load, not play time.
            if (inputAllowed && _inputWasAllowed && elapsed > 0.0 && elapsed <= 0.25) {
                const double dt = (std::min)(elapsed, 0.1);
                const Stick stick = FilterStick(input.stickX, input.stickY);
                _yaw = std::remainder(_yaw + stick.x * kStickSpeed * dt +
                    input.mouseX * kMouseSpeed, 2.0 * kPi);
                const double pitchDelta = stick.y * kStickSpeed * dt - input.mouseY * kMouseSpeed;
                // Preserve the established stick/mouse directions. If vanilla
                // starts beyond our input limit, allow movement back inward
                // without snapping that opening view onto the limit.
                _pitch = std::clamp(_pitch + pitchDelta,
                    (std::min)(_pitch, -kPitchLimit), (std::max)(_pitch, kPitchLimit));
            }
            _inputWasAllowed = inputAllowed;
            Write(matrix);
        }

    private:
        static constexpr double kPi = 3.14159265358979323846;

        bool Capture(const float matrix[3][3])
        {
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    if (!std::isfinite(matrix[row][col])) return false;
            const double fx = matrix[0][1], fy = matrix[1][1], fz = matrix[2][1];
            const double horizontal = std::hypot(fx, fy);
            if (std::hypot(horizontal, fz) < 0.001) return false;
            _yaw = horizontal > 1e-6 ? std::atan2(-fx, fy)
                                    : std::atan2(matrix[1][0], matrix[0][0]);
            _pitch = std::atan2(fz, horizontal);
            const double sy = std::sin(_yaw), cy = std::cos(_yaw);
            const double sp = std::sin(_pitch), cp = std::cos(_pitch);
            // Project the captured right axis onto the unrolled right/up
            // basis. This preserves the complete opening orientation, even
            // for a tilted corpse or a view exactly at a vertical pole.
            const double right = matrix[0][0] * cy + matrix[1][0] * sy;
            const double up = matrix[0][0] * sy * sp - matrix[1][0] * cy * sp + matrix[2][0] * cp;
            _roll = std::atan2(-up, right);
            _active = true;
            return true;
        }

        void Write(float matrix[3][3]) const
        {
            const double sy = std::sin(_yaw), cy = std::cos(_yaw);
            const double sp = std::sin(_pitch), cp = std::cos(_pitch);
            const double sr = std::sin(_roll), cr = std::cos(_roll);
            const double right[3]{cy, sy, 0.0};
            const double forward[3]{-sy * cp, cy * cp, sp};
            const double up[3]{sy * sp, -cy * sp, cp};
            for (int row = 0; row < 3; ++row) {
                matrix[row][0] = float(right[row] * cr - up[row] * sr);
                matrix[row][1] = float(forward[row]);
                matrix[row][2] = float(right[row] * sr + up[row] * cr);
            }
        }

        double _yaw{}, _pitch{}, _roll{}, _lastTime{};
        bool _active{}, _inputWasAllowed{};
    };
}
