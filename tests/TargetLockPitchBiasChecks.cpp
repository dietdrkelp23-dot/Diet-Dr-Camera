#include "Camera/TargetLockPitchBias.h"
#include "Core/Spring.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;

int main()
{
    try {
        const auto check = [](bool ok) { if (!ok) throw std::runtime_error("Pitch Bias checks failed"); };
        using TargetLockPitchBias::Radians;
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
        std::cout << "Pitch Bias proximity and release checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
