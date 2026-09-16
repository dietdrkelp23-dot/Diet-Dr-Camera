#include "PCH.h"
#include "Settings/SettingsManager.h"
#include "Camera/HitShakeController.h"

namespace DietDrCamera
{
    void SettingsManager::ApplyEngineSettings()
    {
        HitShakeController::Reset();
        // Push engine-global settings eagerly so they're in place before
        // any event can read them â€” notably fPlayerDeathReloadTime, which
        // the engine reads at BleedoutCameraState::Begin (once per death,
        // before our per-frame hook gets a chance to run).
        //
        // Infinite Duration overrides the slider with a day's worth of
        // seconds â€” the engine's countdown effectively never expires
        // and the user must use the skip hotkey (or ESC menu â†’ Load)
        // to leave bleedout.
        const float deathReloadInit = deathCameraInfiniteDuration
            ? 86400.0f : deathCameraHoldDuration;
        if (auto* gsc = RE::GameSettingCollection::GetSingleton()) {
            if (auto* setting = gsc->GetSetting("fPlayerDeathReloadTime")) {
                setting->data.f = deathReloadInit;
            }
        }
        if (auto* ini = RE::INISettingCollection::GetSingleton()) {
            if (auto* setting = ini->GetSetting("fPlayerDeathReloadTime:GamePlay")) {
                setting->data.f = deathReloadInit;
            }
        }
        spdlog::info("SettingsManager: fPlayerDeathReloadTime primed to {:.1f}s at load (infinite={})",
                     deathReloadInit, deathCameraInfiniteDuration);

    }
}
