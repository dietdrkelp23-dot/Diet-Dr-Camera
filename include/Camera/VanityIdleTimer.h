#pragma once

#include <algorithm>
#include <cmath>

namespace DietDrCamera
{
    class VanityIdleTimer
    {
    public:
        void Reset() { elapsed = 0.0; sampled = false; }
        [[nodiscard]] double Elapsed() const { return elapsed; }
        bool Tick(double now, double delay, bool eligible, bool activity)
        {
            if (!std::isfinite(now)) { Reset(); return false; }
            const double dt = sampled ? now - last : 0.0;
            sampled = true;
            last = now;
            // Loading, pausing, and long stalls never become banked idle time.
            if (!eligible || activity || dt < 0.0 || dt > 0.5) {
                elapsed = 0.0;
                return false;
            }
            elapsed += dt;
            return elapsed >= std::clamp(std::isfinite(delay) ? delay : 120.0, 5.0, 600.0);
        }
    private:
        double elapsed{}, last{};
        bool sampled{};
    };
}
