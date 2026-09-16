#pragma once

#include <chrono>

namespace DietDrCamera
{
    class PauseTimeline
    {
    public:
        using Clock = std::chrono::steady_clock;

        void SetPaused(bool a_paused, Clock::time_point a_now)
        {
            if (paused == a_paused) return;
            if (a_paused) pauseStart = a_now;
            else excluded += a_now - pauseStart;
            paused = a_paused;
        }

        [[nodiscard]] Clock::time_point Now(Clock::time_point a_now) const
        {
            return (paused ? pauseStart : a_now) - excluded;
        }

        [[nodiscard]] bool IsPaused() const { return paused; }

    private:
        bool paused = false;
        Clock::time_point pauseStart{};
        Clock::duration excluded{};
    };
}
