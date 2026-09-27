#include "PCH.h"
#include "Camera/CombatFOVController.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/StateResolver.h"
#include "Camera/VanityCamera.h"
#include "Settings/SettingsManager.h"
#include "UI/MenuUI.h"

namespace DietDrCamera::CombatFOVController
{
    namespace {
        CombatFOV::Envelope envelope;
        std::chrono::steady_clock::time_point lastTick{};
    }

    void Reset()
    {
        envelope.Reset();
        lastTick = {};
    }

    float Apply(float normalFOV, bool gameplayCamera)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto& settings = SettingsManager::GetSingleton();
        const auto state = camera && camera->currentState ? camera->currentState->id : RE::CameraState::kTotal;
        if (!gameplayCamera || !player || !player->Is3DLoaded() || !camera || !ui ||
            player->IsDead() || player->IsInKillMove() || settings.diagnosticSuspendOverrides ||
            ui->IsMenuOpen("Dialogue Menu") || VanityCamera::IsActive() ||
            StateResolver::GetSingleton().IsParagliding() ||
            StateResolver::GetSingleton().IsBowZoomed() ||
            (state != RE::CameraState::kFirstPerson && state != RE::CameraState::kThirdPerson &&
             state != RE::CameraState::kMount)) {
            Reset();
            return normalFOV;
        }

        CameraEffectClock::Sync();
        // The effect clock deliberately freezes in the settings menu. Use real
        // time here and let the envelope distinguish editing from a game pause.
        const auto now = std::chrono::steady_clock::now();
        CombatFOV::Frame frame;
        frame.inCombat = player->IsInCombat();
        frame.paused = ui->GameIsPaused() || CameraEffectClock::IsPaused();
        frame.editorOpen = MenuUI::IsMainMenuOpen() || MenuUI::IsQuickTuneWindowOpen();
        frame.preview = MenuUI::IsCombatFOVPreviewActive();
        frame.dt = lastTick.time_since_epoch().count() ? std::chrono::duration<float>(now-lastTick).count() : 0;
        lastTick = now;
        return envelope.Update(settings.combatFov, normalFOV, frame);
    }
}
