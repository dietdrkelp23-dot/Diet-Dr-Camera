#include "PCH.h"
#include "Camera/VanityCamera.h"
#include "Camera/VanityIdleTimer.h"
#include "Camera/VanityInputActivity.h"
#include "Camera/CameraController.h"
#include "Camera/StateResolver.h"
#include "Settings/SettingsManager.h"
#include "UI/MenuUI.h"
#include "LockOn/TDMIntegration.h"

#include <chrono>

namespace DietDrCamera::VanityCamera
{
    namespace
    {
        VanityIdleTimer idle;
        VanityInputActivity inputActivity;
        bool active = false;
        bool restoreFirstPerson = false;
        bool returnReady = false;
        bool permissionOwned = false;
        bool savedPermission = false;
        bool lastPositionValid = false;
        RE::NiPoint3 lastPlayerPosition{};
        double traceReturnUntil = 0.0;
        double traceNext = 0.0;
        unsigned traceRows = 0;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Sample the preceding completed frame, before any new camera writes.
        // The field requested by DDC and the rendered frustum/distance distinguish
        // a slow return from a later writer or a camera-position/collision issue.
        void TracePose(const char* phase, RE::PlayerCamera* cam, RE::PlayerCharacter* player)
        {
            if (!cam || !player || !cam->cameraRoot || traceRows >= 160) return;
            auto* root = cam->cameraRoot->AsNode();
            if (!root) return;
            for (const auto& child : root->GetChildren()) {
                auto* ni = skyrim_cast<RE::NiCamera*>(child.get());
                if (!ni) continue;
                const auto relative = ni->world.translate - player->GetPosition();
                const auto& fr = ni->viewFrustum;
                const float horizontalFov = fr.fNear > 0.0001f
                    ? (std::atan(-fr.fLeft / fr.fNear) + std::atan(fr.fRight / fr.fNear)) * 57.29578f
                    : -1.0f;
                bool appliedValid = false;
                const float applied = CameraController::GetLastAppliedWorldFov(appliedValid);
                const auto& profile = CameraController::GetSingleton().GetTargetProfile();
                ++traceRows;
                spdlog::debug("[VANITY-VIEW] phase={} camera={} gameplay={} requestedFov={:.2f} appliedFov={:.2f} "
                             "worldFov={:.2f} frustumH={:.2f} distance={:.2f} horizontalDistance={:.2f} "
                             "requestedZoom={:.2f} active={} framework={}",
                    phase, cam->currentState ? static_cast<int>(cam->currentState->id) : -1,
                    static_cast<int>(StateResolver::GetSingleton().GetState()),
                    profile.fov, appliedValid ? applied : -1.0f,
                    cam->worldFOV, horizontalFov, relative.Length(), std::hypot(relative.x, relative.y),
                    profile.zoom, active, MenuUI::IsGameInputBlocked());
                break;
            }
        }

        bool NativeMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return true;
            if (ui->GameIsPaused() && !MenuUI::IsGameInputBlocked()) return true;
            for (const char* name : {"Main Menu", "Loading Menu", "Dialogue Menu", "TweenMenu",
                     "InventoryMenu", "MagicMenu", "ContainerMenu", "BarterMenu", "GiftMenu",
                     "MapMenu", "Journal Menu", "Console", "Crafting Menu", "FavoritesMenu",
                     "Book Menu", "Sleep/Wait Menu", "RaceSex Menu"}) {
                if (ui->IsMenuOpen(name)) return true;
            }
            return false;
        }

        bool InThirdPerson(RE::PlayerCamera* cam)
        {
            if (!cam || !cam->currentState) return false;
            if (cam->currentState->id == RE::CameraState::kThirdPerson) return true;
            if (cam->currentState->id == RE::CameraState::kPCTransition) {
                auto* transition = static_cast<RE::PlayerCameraTransitionState*>(cam->currentState.get());
                return transition->transitionTo == cam->cameraStates[RE::CameraState::kThirdPerson].get();
            }
            return false;
        }

        bool PoseAllowed(RE::PlayerCamera* cam, RE::PlayerCharacter* player)
        {
            if (!cam || !cam->currentState || !player || !player->Get3D(false)) return false;
            if (!InThirdPerson(cam) && (active || cam->currentState->id != RE::CameraState::kFirstPerson))
                return false;
            if (!active) {
                const auto* controls = RE::ControlMap::GetSingleton();
                const auto* input = RE::PlayerControls::GetSingleton();
                // The look-control mask can be clear during normal mouse look.
                if (!controls || !controls->IsMovementControlsEnabled() || !input ||
                    input->data.remapMode || input->data.fovSlideMode) return false;
            }
            const auto* state = player->AsActorState();
            const auto& resolver = StateResolver::GetSingleton();
            return state && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal &&
                   !resolver.IsWerewolf() && !resolver.IsVampireLord() && !player->IsInMidair() &&
                   !TDMIntegration::GetSingleton().IsTargetLocked() && !player->IsInKillMove() &&
                   !player->IsDead() && !player->IsInCombat() && !player->IsOnMount() &&
                   !state->IsSwimming() && !player->IsSneaking() && !state->IsWeaponDrawn();
        }

        void Release(bool keepPOVReturn)
        {
            if (active) {
                spdlog::debug("[VANITY] third-person profile released; keepPOVReturn={}", keepPOVReturn);
                traceReturnUntil = Now() + 15.0;
                traceNext = 0.0;
            }
            active = false;
            if (!keepPOVReturn) {
                restoreFirstPerson = returnReady = false;
                CameraController::GetSingleton().CancelVanityTransition();
            }
            idle.Reset();
        }
    }

    bool IsActive() { return active; }

    void FinishReturn()
    {
        // Defer SetState until before the next engine update, never from inside
        // ThirdPersonState::Update while it still owns that frame's output.
        if (!active && restoreFirstPerson) returnReady = true;
    }

    void Reset()
    {
        Release(false);
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (permissionOwned && cam && !cam->allowAutoVanityMode) cam->allowAutoVanityMode = savedPermission;
        permissionOwned = false;
        lastPositionValid = false;
        traceReturnUntil = 0.0;
        inputActivity.Reset();
    }

    void OnInput(RE::InputEvent* events)
    {
        const auto& s = SettingsManager::GetSingleton();
        if (s.disableVanityCamera || MenuUI::IsGameInputBlocked()) return;
        for (auto* ev = events; ev; ev = ev->next) {
            if (auto* button = ev->AsButtonEvent()) {
                const auto code = button->GetIDCode();
                const auto encoded = button->GetDevice() == RE::INPUT_DEVICE::kGamepad ? code | 0x10000000u : code;
                // Opening/closing Quick Tune keeps this profile available to edit.
                if (encoded != 0 && encoded == s.quickTuneHotkey) continue;
                if (button->IsPressed() || button->IsUp()) inputActivity.RecordAction();
            } else if (auto* mouse = ev->AsMouseMoveEvent()) {
                if (mouse->mouseInputX || mouse->mouseInputY) inputActivity.RecordLook();
            } else if (auto* stick = ev->AsThumbstickEvent()) {
                const auto* userEvent = stick->QUserEvent().c_str();
                inputActivity.RecordThumbstick(stick->xValue, stick->yValue, stick->IsRight(),
                                              userEvent ? userEvent : "");
            }
        }
    }

    void Tick()
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto& s = SettingsManager::GetSingleton();
        if (!cam) { Reset(); return; }
        const double now = Now();
        if (!active && now < traceReturnUntil && now >= traceNext) {
            TracePose("return", cam, player);
            traceNext = now + 0.5;
        }
        const auto activity = inputActivity.Consume();
        if (s.diagnosticSuspendOverrides && !s.disableVanityCamera) {
            Release(false);
            if (permissionOwned && !cam->allowAutoVanityMode) cam->allowAutoVanityMode = savedPermission;
            permissionOwned = false;
            return;
        }

        // Replace the vanilla idle camera. Vanity is a profile in the SAME
        // ThirdPersonState, so the native orbit never begins and no pose is saved,
        // rewritten, or fed through a second camera/transition implementation.
        if (!permissionOwned) { savedPermission = cam->allowAutoVanityMode; permissionOwned = true; }
        cam->allowAutoVanityMode = false;
        cam->idleTimer = 0;

        const bool framework = MenuUI::IsGameInputBlocked();
        const bool nativeMenu = NativeMenuOpen();
        if (!nativeMenu && cam->currentState && cam->currentState->id == RE::CameraState::kAutoVanity) {
            cam->ForceThirdPerson();  // Recover an already-committed native idle transition.
            Release(false);
            return;
        }
        bool moved = false;
        if (player) {
            const auto pos = player->GetPosition();
            moved = lastPositionValid && (pos - lastPlayerPosition).Length() > 0.1f;
            lastPlayerPosition = pos;
            lastPositionValid = true;
        } else lastPositionValid = false;

        if (active) {
            if (s.disableVanityCamera || s.diagnosticSuspendOverrides || nativeMenu ||
                !PoseAllowed(cam, player) || (!framework && (activity.endsVanity || moved))) {
                Release(!nativeMenu && InThirdPerson(cam) && !s.diagnosticSuspendOverrides);
            }
            return;
        }

        if (restoreFirstPerson) {
            if (nativeMenu || !InThirdPerson(cam) || !player || player->IsDead() ||
                player->IsInKillMove() || player->IsOnMount()) {
                Release(false); // A menu, script or deliberate POV change owns the camera now.
            } else if (returnReady && !framework) {
                Release(false);
                cam->ForceFirstPerson();
            }
            return;
        }
        const bool eligible = !s.disableVanityCamera && !s.diagnosticSuspendOverrides &&
                              !nativeMenu && !framework && PoseAllowed(cam, player);
        if (idle.Tick(Now(), s.vanityIdleSeconds, eligible, activity.any || moved)) {
            TracePose("before-entry", cam, player);
            restoreFirstPerson = cam->currentState->id == RE::CameraState::kFirstPerson;
            returnReady = false;
            active = true;
            if (restoreFirstPerson) cam->ForceThirdPerson();
            if (!InThirdPerson(cam)) { Release(false); return; }
            idle.Reset();
            spdlog::debug("[VANITY] third-person profile entered; returnFirstPerson={}", restoreFirstPerson);
        }
    }
}
