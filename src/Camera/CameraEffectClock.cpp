#include "PCH.h"
#include "Camera/CameraEffectClock.h"
#include "Core/PauseTimeline.h"
#include "UI/MenuUI.h"

#include <mutex>

namespace DietDrCamera::CameraEffectClock
{
    namespace
    {
        PauseTimeline timeline;
        std::mutex timelineMutex;
    }

    void Sync()
    {
        auto* main = RE::Main::GetSingleton();
        const bool paused = main && main->freezeTime && MenuUI::IsGameInputBlocked();
        bool changed = false;
        {
            std::lock_guard lock(timelineMutex);
            changed = timeline.IsPaused() != paused;
            timeline.SetPaused(paused, PauseTimeline::Clock::now());
        }
        if (changed) spdlog::debug("[CAMERA-CLOCK] effects {} with framework pause", paused ? "held" : "resumed");
    }

    void Resume()
    {
        std::lock_guard lock(timelineMutex);
        timeline.SetPaused(false, PauseTimeline::Clock::now());
    }

    bool IsPaused()
    {
        std::lock_guard lock(timelineMutex);
        return timeline.IsPaused();
    }

    std::chrono::steady_clock::time_point Now()
    {
        std::lock_guard lock(timelineMutex);
        return timeline.Now(PauseTimeline::Clock::now());
    }
}
