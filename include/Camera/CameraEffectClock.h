#pragma once

#include <chrono>

namespace DietDrCamera::CameraEffectClock
{
    void Sync();
    void Resume();
    [[nodiscard]] bool IsPaused();
    [[nodiscard]] std::chrono::steady_clock::time_point Now();
}
