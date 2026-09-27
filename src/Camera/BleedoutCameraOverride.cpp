#include "PCH.h"
#include "Camera/BleedoutCameraOverride.h"
#include "Camera/BleedoutCameraPolicy.h"
#include "Camera/BleedoutFreeLook.h"
#include "Camera/NativeGameplayLook.h"
#include "Hooks/RuntimeHooks.h"
#include "LockOn/TDMIntegration.h"
#include "Camera/StateResolver.h"
#include "Core/BleedoutEffects.h"
#include "Hooks/CameraStateEntry.h"
#include "Hooks/HookManager.h"
#include "Settings/SettingsManager.h"
#include "UI/MenuUI.h"
#include "UI/SKSEMenuFramework.h"

#include <mutex>

namespace DietDrCamera::BleedoutCameraOverride
{
    namespace
    {
        using Action = BleedoutCameraPolicy::Action;
        using Event = BleedoutEffects::Event;
        using Update = void (*)(RE::TESCameraState*, RE::BSTSmartPointer<RE::TESCameraState>&);
        REL::Relocation<void (*)(RE::TESCamera*, RE::TESCameraState*)> originalSetState;
        std::array<Update, 13> originalUpdates{};
        using GetRotation = void (*)(RE::TESCameraState*, RE::NiQuaternion&);
        GetRotation originalFirstPersonRotation = nullptr;
        bool installed = false;

        // Begin can arrive on an animation worker, while poses and time are
        // written on the camera thread. Only copy state under this lock; never
        // call native state transitions or scene-graph updates while holding it.
        std::mutex stateMutex;
        BleedoutCameraPolicy::Session session;
        BleedoutEffects::SlowMotion slowMotion;
        std::chrono::steady_clock::time_point slowMotionLast{};
        bool ownsTime = false;
        bool eventRequested = false;
        BleedoutFreeLook gameplayLook;
        RE::NiMatrix3 lookMatrix{};
        RE::NiQuaternion lookRotation{}, lastGameplayRotation{};
        bool haveGameplayRotation = false;
        bool driveGameplayLook = false;
        bool gameplayTargetLocked = false;
        bool gameplayInputAllowed = false;
        NativeGameplayLook::Delta pendingLook;
        bool normalOrbit = false;
        float normalTurnSpeed = 0.0f;
        struct HeadingWrite {
            RE::TESCameraState* state{};
            float offset{}, currentYaw{}, writtenOffset{}, writtenYaw{};
        } headingWrite;
        using ControlsInput = RE::BSEventNotifyControl (*)(RE::PlayerControls*, RE::InputEvent* const*, RE::BSTEventSource<RE::InputEvent*>*);
        using MouseInput = void (*)(RE::LookHandler*, RE::MouseMoveEvent*, RE::PlayerControlsData*);
        using StickInput = void (*)(RE::LookHandler*, RE::ThumbstickEvent*, RE::PlayerControlsData*);
        ControlsInput originalControlsInput = nullptr;
        MouseInput originalMouseInput = nullptr;
        StickInput originalStickInput = nullptr;
        thread_local GameplayCameraInput::Delivered* dispatchedInput = nullptr;
        struct InputCapture {
            GameplayCameraInput::Delivered* previous;
            explicit InputCapture(GameplayCameraInput::Delivered* next) : previous(std::exchange(dispatchedInput, next)) {}
            ~InputCapture() { dispatchedInput = previous; }
        };
        bool RealDeath(RE::PlayerCharacter* player)
        {
            return BleedoutCameraPolicy::RealDeath(player, RE::ActorValue::kHealth);
        }

        bool KnockedDown(RE::PlayerCharacter* player)
        {
            const auto* actor = player ? player->AsActorState() : nullptr;
            return actor && (actor->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal ||
                actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kBleedout ||
                actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kEssentialDown);
        }

        bool Ready(RE::PlayerCamera* camera, RE::PlayerCharacter* player)
        {
            auto* ui = RE::UI::GetSingleton();
            return !(ui && (ui->IsMenuOpen("Main Menu") || ui->IsMenuOpen("Loading Menu"))) &&
                camera && camera->cameraRoot && player && player->GetActorBase() && player->Get3D(false);
        }

        bool Disabled(bool dead)
        {
            const auto& settings = SettingsManager::GetSingleton();
            return BleedoutCameraPolicy::Disabled(dead, settings.disableDeathCamera,
                settings.disableRagdollCamera, settings.diagnosticSuspendOverrides);
        }

        bool IsGameplay(const RE::TESCameraState* state)
        {
            return state && (state->id == RE::CameraState::kFirstPerson ||
                state->id == RE::CameraState::kThirdPerson);
        }

        bool LookInputAllowed(bool gameplay)
        {
            if (!gameplay || HookManager::IsUiDriveActive() || MenuUI::IsQuickTuneOpen() ||
                (SKSEMenuFramework::IsInstalled() && SKSEMenuFramework::IsAnyBlockingWindowOpened())) return false;
            DWORD foregroundProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            if (foregroundProcess != GetCurrentProcessId()) return false;
            if (auto* ui = RE::UI::GetSingleton(); ui && (ui->GameIsPaused() ||
                ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen() ||
                ui->IsMenuOpen("Console") || ui->IsMenuOpen("Dialogue Menu") ||
                ui->IsMenuOpen("TweenMenu") || ui->IsMenuOpen("FavoritesMenu"))) return false;
            return true;
        }

        float INIFloat(const char* name, float fallback)
        {
            const auto* setting = RE::GetINISetting(name);
            return setting && std::isfinite(setting->data.f) ? setting->data.f : fallback;
        }

        void RestoreHeadingOffset()
        {
            auto* camera = RE::PlayerCamera::GetSingleton();
            if (camera && headingWrite.state == camera->cameraStates[RE::CameraState::kFirstPerson].get()) {
                auto* state = static_cast<RE::FirstPersonState*>(headingWrite.state);
                if (state && state->sittingRotation == headingWrite.writtenOffset) state->sittingRotation = headingWrite.offset;
            } else if (camera && headingWrite.state == camera->cameraStates[RE::CameraState::kThirdPerson].get()) {
                auto* state = static_cast<RE::ThirdPersonState*>(headingWrite.state);
                if (state && state->freeRotation.x == headingWrite.writtenOffset) state->freeRotation.x = headingWrite.offset;
                if (state && state->currentYaw == headingWrite.writtenYaw) state->currentYaw = headingWrite.currentYaw;
            }
            headingWrite = {};
        }

        void SyncGameplayHeading(RE::TESCameraState* state)
        {
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!IsGameplay(state) || !camera || camera->currentState.get() != state || !player) return;
            const float targetHeading = player->GetHeading(false);
            std::lock_guard lock(stateMutex);
            if (!driveGameplayLook || !gameplayLook.IsActive() || gameplayTargetLocked) return;
            const float offset = NativeGameplayLook::HeadingOffset(gameplayLook.Yaw(), targetHeading);
            const float heading = targetHeading + offset;
            if (state->id == RE::CameraState::kFirstPerson) {
                auto* first = static_cast<RE::FirstPersonState*>(state);
                if (!headingWrite.state) headingWrite = {state, first->sittingRotation};
                first->sittingRotation = offset;
            } else {
                auto* third = static_cast<RE::ThirdPersonState*>(state);
                if (!headingWrite.state) headingWrite = {state, third->freeRotation.x, third->currentYaw};
                third->freeRotation.x = offset;
                third->currentYaw = heading;
            }
            headingWrite.writtenOffset = offset;
            headingWrite.writtenYaw = heading;
            // Native PlayerCamera::Update recomputes this immediately after
            // TESCamera::Update using precisely the state offset above.
            camera->GetRuntimeData2().yaw = heading;
        }

        void CacheNativeLookMode(RE::PlayerCamera* camera, RE::PlayerCharacter* player)
        {
            const auto* state = camera->currentState.get();
            if (!IsGameplay(state)) return;
            RE::Movement::MaxSpeeds speeds{};
            auto* actorState = player->AsActorState();
            // IMovementState slot 8 fills MaxSpeeds. The normal player movement
            // controller takes the run rotation rate at +0x24 from this data.
            const bool gotSpeeds = actorState && REL::RelocateVirtual<bool(RE::ActorState*, RE::Movement::MaxSpeeds&)>(
                0x8, 0x8, actorState, speeds);
            float turnSpeed = gotSpeeds ? speeds.speeds[RE::Movement::SPEED_DIRECTIONS::kRotations][RE::Movement::MaxSpeeds::kRun] : 0.0f;
            if (!std::isfinite(turnSpeed) || turnSpeed <= 0.0f) {
                if (auto* settings = RE::GameSettingCollection::GetSingleton())
                    if (auto* setting = settings->GetSetting("fActorDefaultTurningSpeed")) turnSpeed = setting->data.f;
            }
            const bool orbit = state->id == RE::CameraState::kThirdPerson &&
                static_cast<const RE::ThirdPersonState*>(state)->GetFreeRotationMode();
            std::lock_guard lock(stateMutex);
            normalOrbit = orbit;
            if (std::isfinite(turnSpeed) && turnSpeed > 0.0f) normalTurnSpeed = turnSpeed;
        }

        void MouseLook(RE::LookHandler* handler, RE::MouseMoveEvent* event, RE::PlayerControlsData* data)
        {
            ObserveInput(event, GameplayCameraInput::Route::Look);
            originalMouseInput(handler, event, data);
        }

        void StickLook(RE::LookHandler* handler, RE::ThumbstickEvent* event, RE::PlayerControlsData* data)
        {
            ObserveInput(event, GameplayCameraInput::Route::Look);
            originalStickInput(handler, event, data);
        }

        RE::BSEventNotifyControl ControlsLook(RE::PlayerControls* controls, RE::InputEvent* const* events,
            RE::BSTEventSource<RE::InputEvent*>* source)
        {
            bool active;
            {
                // Startup input precedes player initialization. Only a ready
                // camera update may arm this fallback; never read health here.
                std::lock_guard lock(stateMutex);
                active = driveGameplayLook && gameplayLook.IsActive();
            }
            GameplayCameraInput::Delivered delivered;
            InputCapture capture(active ? &delivered : nullptr);
            const auto result = originalControlsInput(controls, events, source);
            if (!active) return result;
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!Ready(camera, player) || !LookInputAllowed(IsGameplay(camera->currentState.get())) ||
                controls->data.remapMode) return result;

            const bool nativeLook = delivered.HasLook();
            RE::PlayerControlsData data = controls->data;
            if (!nativeLook) data.lookInputVec = {};
            for (auto* event = events ? *events : nullptr; event; event = event->next) {
                auto* look = controls->lookHandler;
                if (!nativeLook && (event->AsMouseMoveEvent() || event->AsThumbstickEvent()) &&
                    look && look->IsInputEventHandlingEnabled() && look->CanProcess(event) &&
                    !delivered.Contains(event, GameplayCameraInput::Route::Look)) {
                    // Keep TDM's existing mouse/stick target-switch handling.
                    if (auto* mouse = event->AsMouseMoveEvent()) look->ProcessMouseMove(mouse, &data);
                    else if (auto* stick = event->AsThumbstickEvent()) look->ProcessThumbstick(stick, &data);
                    continue;
                }
                auto* button = event->AsButtonEvent();
                if (!button) continue;
                const auto* name = button->QUserEvent().c_str();
                const auto route = GameplayCameraInput::ButtonRoute(name ? name : "");
                if (route == GameplayCameraInput::Route::None || delivered.Contains(event, route)) continue;
                if (route == GameplayCameraInput::Route::TogglePOV) {
                    auto* handler = controls->togglePOVHandler;
                    if (!handler || !handler->IsInputEventHandlingEnabled() || !handler->CanProcess(event)) continue;
                    if (!delivered.Contains(event, GameplayCameraInput::Route::POVHeld)) handler->UpdateHeldStateActive(button);
                    handler->ProcessButton(button, &data);
                } else {
                    // Camera-state input uses the secondary PlayerInputHandler
                    // base, not the TESCameraState pointer at offset zero.
                    RE::PlayerInputHandler* handler = nullptr;
                    auto* state = camera->currentState.get();
                    if (state && state->id == RE::CameraState::kThirdPerson)
                        handler = static_cast<RE::ThirdPersonState*>(state);
                    else if (state && state->id == RE::CameraState::kFirstPerson)
                        handler = static_cast<RE::FirstPersonState*>(state);
                    if (handler && handler->IsInputEventHandlingEnabled() && handler->CanProcess(event))
                        handler->ProcessButton(button, &data);
                }
            }
            if (!nativeLook) {
                using Normalize = void (*)(RE::PlayerControls*, RE::NiPoint2&);
                static REL::Relocation<Normalize> normalize{REL::RelocationID(41275, 42354)};
                normalize(controls, data.lookInputVec);
            }
            // Reuse normal native dispatch without a second normalization.
            // Local fallback data never enables movement or combat actions.
            static REL::Relocation<float*> inputFrameSeconds{REL::RelocationID(523661, 410200)};
            const float defaultFov = INIFloat("fDefaultWorldFOV:Display", 80.0f);
            const float fovScale = defaultFov > 0.0f ? std::clamp(camera->worldFOV / defaultFov, 0.0f, 1.0f) : 1.0f;
            std::lock_guard lock(stateMutex);
            if (!driveGameplayLook) return result;
            const auto delta = NativeGameplayLook::Rotation(data.lookInputVec.x, data.lookInputVec.y, *inputFrameSeconds,
                normalOrbit, INIFloat("fFreeRotationSpeed:Camera", 3.0f), normalTurnSpeed,
                INIFloat("fLookingSpeed:Camera", 0.1f), fovScale);
            pendingLook.yaw += delta.yaw;
            pendingLook.pitch += delta.pitch;
            return result;
        }

        void UpdateGameplayLook(RE::PlayerCamera* camera, RE::PlayerCharacter* player, bool dead, bool knockedDown)
        {
            if (HookManager::IsUiDriveActive()) return;
            const auto* current = camera->currentState.get();
            const bool gameplay = IsGameplay(current);
            const bool drive = Disabled(dead) && (dead || knockedDown);
            const bool inputAllowed = drive && LookInputAllowed(gameplay);
            const bool targetLocked = current && current->id == RE::CameraState::kThirdPerson &&
                TDMIntegration::GetSingleton().IsTargetLocked();
            if (!dead && !knockedDown) CacheNativeLookMode(camera, player);
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            bool handoff = false;
            float heading = 0.0f, pitch = 0.0f;
            {
                std::lock_guard lock(stateMutex);
                const auto delta = std::exchange(pendingLook, {});
                driveGameplayLook = drive;
                gameplayTargetLocked = drive && targetLocked;
                gameplayInputAllowed = inputAllowed;
                if (!drive) {
                    const bool recovered = !dead && !knockedDown && gameplay;
                    handoff = gameplayLook.IsActive() && recovered && !targetLocked &&
                        !SettingsManager::GetSingleton().diagnosticSuspendOverrides;
                    if (handoff) {
                        heading = static_cast<float>(-gameplayLook.Yaw());
                        pitch = static_cast<float>(-gameplayLook.Pitch());
                    }
                    if (recovered || dead || knockedDown || SettingsManager::GetSingleton().diagnosticSuspendOverrides)
                        gameplayLook.Reset();
                } else {
                    if (!gameplayLook.IsActive()) {
                        lookMatrix = haveGameplayRotation ? lastGameplayRotation.ToRotation() :
                            camera->cameraRoot->local.rotate;
                    }
                    if (targetLocked) gameplayLook.Follow(lookMatrix.entry, now, inputAllowed);
                    else gameplayLook.UpdateRadians(lookMatrix.entry, delta.yaw, delta.pitch, now, inputAllowed);
                    lookRotation.SetRotation(lookMatrix);
                }
            }
            if (handoff) {
                auto* third = static_cast<RE::ThirdPersonState*>(camera->cameraStates[RE::CameraState::kThirdPerson].get());
                if (current->id == RE::CameraState::kThirdPerson && third &&
                    third->GetFreeRotationMode() && third->applyOffsets) {
                    third->freeRotation.x = std::remainder(heading - player->GetHeading(false), 6.28318530718f);
                    third->freeRotation.y = player->data.angle.x - pitch;
                } else {
                    player->SetHeading(heading);
                    player->SetLooking(pitch);
                    if (third) third->freeRotation = {};
                }
                if (third) third->targetYaw = third->currentYaw = heading;
                spdlog::debug("[CameraOverride] Gameplay look returned after recovery");
            }
        }

        void FirstPersonRotation(RE::TESCameraState* state, RE::NiQuaternion& rotation)
        {
            originalFirstPersonRotation(state, rotation);
            ApplyGameplayRotation(state, rotation);
        }

        RE::TESCameraState* GameplayState(RE::PlayerCamera* camera)
        {
            const auto& resolver = StateResolver::GetSingleton();
            const bool firstPerson = HookManager::WasLastFrameFirstPerson() &&
                !resolver.IsWerewolf() && !resolver.IsVampireLord();
            return camera->cameraStates[firstPerson ? RE::CameraState::kFirstPerson : RE::CameraState::kThirdPerson].get();
        }

        void SyncReloadTime()
        {
            const auto& s = SettingsManager::GetSingleton();
            const float seconds = s.deathCameraInfiniteDuration ? 86400.0f : s.deathCameraHoldDuration;
            if (auto* settings = RE::GameSettingCollection::GetSingleton())
                if (auto* setting = settings->GetSetting("fPlayerDeathReloadTime")) setting->data.f = seconds;
            if (auto* settings = RE::INISettingCollection::GetSingleton())
                if (auto* setting = settings->GetSetting("fPlayerDeathReloadTime:GamePlay")) setting->data.f = seconds;
        }

        // Called only with stateMutex held. Changing view modes does not rearm.
        void EnterEffect(Event event)
        {
            const auto& s = SettingsManager::GetSingleton();
            const bool dead = event == Event::Death;
            if (slowMotion.Enter(event, dead ? s.deathCameraSlowmoStrength : s.ragdollCamSlowmoStrength,
                    dead ? s.deathCameraSlowmoDuration : s.ragdollCamSlowmoDuration)) {
                slowMotionLast = std::chrono::steady_clock::now();
            }
        }

        Action Filter(RE::TESCamera* camera, RE::TESCameraState* requested)
        {
            if (!requested || requested->id != RE::CameraState::kBleedout) return Action::PassThrough;
            auto* playerCamera = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!camera || camera != playerCamera || !Ready(playerCamera, player)) return Action::PassThrough;
            const bool dead = RealDeath(player);
            const auto* current = camera->currentState.get();
            const bool gameplay = current && (current->id == RE::CameraState::kFirstPerson ||
                current->id == RE::CameraState::kThirdPerson);
            auto* ui = RE::UI::GetSingleton();
            const bool menu = current && current->id == RE::CameraState::kTween && ui &&
                (ui->IsMenuOpen("TweenMenu") || ui->IsMenuOpen("FavoritesMenu"));
            const auto action = BleedoutCameraPolicy::Resolve(true, true, true, Disabled(dead), gameplay, menu);
            if (action != Action::PassThrough && GameplayState(playerCamera)) {
                BeginEvent(dead);
                if (dead) SyncReloadTime();
                return action;
            }
            return Action::PassThrough;
        }

        void SetState(RE::TESCamera* camera, RE::TESCameraState* requested)
        {
            const auto action = Filter(camera, requested);
            // Native SetState runs End/Begin even when both pointers match.
            if (action == Action::KeepCurrent) return;
            if (action == Action::ReturnToGameplay) requested = GameplayState(RE::PlayerCamera::GetSingleton());
            originalSetState(camera, requested);
        }

        template <RE::CameraState ID>
        void StateUpdate(RE::TESCameraState* state, RE::BSTSmartPointer<RE::TESCameraState>& next)
        {
            originalUpdates[static_cast<std::size_t>(ID)](state, next);
            SyncGameplayHeading(state);
            // TESCamera::Update inlines this transition instead of SetState.
            const auto action = Filter(state->camera, next.get());
            if (action == Action::KeepCurrent) next.reset();
            else if (action == Action::ReturnToGameplay) next.reset(GameplayState(RE::PlayerCamera::GetSingleton()));
        }

        template <RE::CameraState ID>
        void Wrap(REL::VariantID table)
        {
            REL::Relocation<std::uintptr_t> vtable{table};
            originalUpdates[static_cast<std::size_t>(ID)] =
                reinterpret_cast<Update>(vtable.write_vfunc(0x3, &StateUpdate<ID>));
        }

    }

    bool IsAvailable() { return installed; }

    void Reset()
    {
        RestoreHeadingOffset();
        bool restore;
        {
            std::lock_guard lock(stateMutex);
            session.Reset();
            slowMotion.Reset();
            eventRequested = false;
            gameplayLook.Reset();
            pendingLook = {};
            normalOrbit = false;
            normalTurnSpeed = 0.0f;
            haveGameplayRotation = driveGameplayLook = false;
            gameplayTargetLocked = gameplayInputAllowed = false;
            restore = std::exchange(ownsTime, false);
        }
        if (restore)
            if (auto* timer = RE::BSTimer::GetSingleton()) timer->SetGlobalTimeMultiplier(1.0f, true);
    }

    void ObserveInput(const RE::InputEvent* event, GameplayCameraInput::Route route)
    {
        if (dispatchedInput) dispatchedInput->Record(event, route);
    }

    void ApplyGameplayRotation(RE::TESCameraState* state, RE::NiQuaternion& rotation)
    {
        const auto* camera = RE::PlayerCamera::GetSingleton();
        if (!installed || !IsGameplay(state) || !camera || camera->currentState.get() != state) return;
        std::lock_guard lock(stateMutex);
        if (driveGameplayLook && gameplayTargetLocked) {
            // Preserve the existing TDM/profile rotation and continuously keep
            // free look aligned with it for unlock, target loss and recovery.
            lookMatrix = rotation.ToRotation();
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            gameplayLook.Follow(lookMatrix.entry, now, gameplayInputAllowed);
            lookRotation = lastGameplayRotation = rotation;
            haveGameplayRotation = true;
        } else if (driveGameplayLook && gameplayLook.IsActive()) rotation = lookRotation;
        else if (!gameplayLook.IsActive()) {
            lastGameplayRotation = rotation;
            haveGameplayRotation = true;
        }
    }

    void BeginEvent(bool dead)
    {
        if (SettingsManager::GetSingleton().diagnosticSuspendOverrides) return;
        const bool suppress = installed && Disabled(dead);
        std::lock_guard lock(stateMutex);
        EnterEffect(dead ? Event::Death : Event::Ragdoll);
        eventRequested = true;
        if (suppress) {
            if (!session.active) spdlog::info("[CameraOverride] Keeping gameplay camera for {}", dead ? "death" : "ragdoll");
            session.Blocked();
        }
    }

    void RequestSlowMotionFade()
    {
        std::lock_guard lock(stateMutex);
        slowMotion.RequestFade();
    }

    bool IsSlowMotionActive()
    {
        std::lock_guard lock(stateMutex);
        return slowMotion.Active();
    }

    bool IsEventActive(bool dead)
    {
        if (SettingsManager::GetSingleton().diagnosticSuspendOverrides) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || RealDeath(player) != dead) return false;
        auto* camera = RE::PlayerCamera::GetSingleton();
        return dead || KnockedDown(player) || (camera && camera->currentState &&
            camera->currentState->id == RE::CameraState::kBleedout);
    }

    void UpdateEffects(RE::TESCamera* camera)
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (camera != cam) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!Ready(cam, player) || SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            Reset();
            return;
        }
        const bool dead = RealDeath(player);
        const bool ragdoll = KnockedDown(player) || (cam->currentState &&
            cam->currentState->id == RE::CameraState::kBleedout);
        auto* ui = RE::UI::GetSingleton();
        const bool paused = (ui && ui->GameIsPaused()) || MenuUI::IsQuickTuneOpen() ||
            (SKSEMenuFramework::IsInstalled() && SKSEMenuFramework::IsAnyBlockingWindowOpened());
        std::optional<float> multiplier;
        {
            std::lock_guard lock(stateMutex);
            if (dead || ragdoll) EnterEffect(dead ? Event::Death : Event::Ragdoll);
            else if (!eventRequested) slowMotion.Reset();
            eventRequested = false;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - slowMotionLast).count();
            slowMotionLast = now;
            multiplier = slowMotion.Advance(dt, paused);
            if (!multiplier && ownsTime) multiplier = 1.0f;
            if (multiplier) ownsTime = *multiplier != 1.0f;
        }
        if (multiplier)
            if (auto* timer = RE::BSTimer::GetSingleton()) timer->SetGlobalTimeMultiplier(*multiplier, true);
    }

    void Install()
    {
        if (installed) return;
        const auto entry = REL::RelocationID(32290, 33026).address();
        const auto gateway = CameraStateEntry::Gateway(
            {reinterpret_cast<const std::uint8_t*>(entry), CameraStateEntry::prologue.size()}, entry + 5);
        if (!gateway) {
            spdlog::warn("[CameraOverride] Death/ragdoll blocking unavailable: TESCamera::SetState entry was changed; leaving it untouched");
            return;
        }
        auto& trampoline = SKSE::GetTrampoline();
        auto* displaced = trampoline.allocate(gateway->size());
        std::memcpy(displaced, gateway->data(), gateway->size());
        FlushInstructionCache(GetCurrentProcess(), displaced, gateway->size());
        originalSetState = reinterpret_cast<std::uintptr_t>(displaced);
        trampoline.write_branch<5>(entry, &SetState);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entry), 5);

        // Installed last: each wrapper chains the engine/other mod/OmniCam
        // update already in that exact primary vtable, with its real object.
        // kAnimated has no distinct engine camera-state object/vtable.
        Wrap<RE::CameraState::kFirstPerson>(RE::VTABLE_FirstPersonState[0]);
        Wrap<RE::CameraState::kAutoVanity>(RE::VTABLE_AutoVanityState[0]);
        Wrap<RE::CameraState::kVATS>(RE::VTABLE_VATSCameraState[0]);
        Wrap<RE::CameraState::kFree>(RE::VTABLE_FreeCameraState[0]);
        Wrap<RE::CameraState::kIronSights>(RE::VTABLE_IronSightsState[0]);
        Wrap<RE::CameraState::kFurniture>(RE::VTABLE_FurnitureCameraState[0]);
        Wrap<RE::CameraState::kPCTransition>(RE::VTABLE_PlayerCameraTransitionState[0]);
        Wrap<RE::CameraState::kTween>(RE::VTABLE_TweenMenuCameraState[0]);
        Wrap<RE::CameraState::kThirdPerson>(RE::VTABLE_ThirdPersonState[0]);
        Wrap<RE::CameraState::kMount>(RE::VTABLE_HorseCameraState[0]);
        Wrap<RE::CameraState::kBleedout>(RE::VTABLE_BleedoutCameraState[0]);
        Wrap<RE::CameraState::kDragon>(RE::VTABLE_DragonCameraState[0]);
        REL::Relocation<std::uintptr_t> firstPersonTable{RE::VTABLE_FirstPersonState[0]};
        originalFirstPersonRotation = reinterpret_cast<GetRotation>(firstPersonTable.write_vfunc(0x4, &FirstPersonRotation));
        REL::Relocation<std::uintptr_t> controlsTable{RE::VTABLE_PlayerControls[0]};
        originalControlsInput = reinterpret_cast<ControlsInput>(controlsTable.write_vfunc(0x1, &ControlsLook));
        REL::Relocation<std::uintptr_t> lookTable{RE::VTABLE_LookHandler[0]};
        originalMouseInput = reinterpret_cast<MouseInput>(lookTable.write_vfunc(RuntimeHooks::InputSlot(0x3), &MouseLook));
        originalStickInput = reinterpret_cast<StickInput>(lookTable.write_vfunc(RuntimeHooks::InputSlot(0x2), &StickLook));
        installed = true;
        spdlog::info("[CameraOverride] Death/ragdoll camera entry guards installed");
    }

    void Tick(RE::TESCamera* camera)
    {
        if (!installed || camera != RE::PlayerCamera::GetSingleton()) return;
        RestoreHeadingOffset();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!Ready(cam, player)) { Reset(); return; }
        const bool dead = RealDeath(player);
        const bool disabled = Disabled(dead);
        const bool knockedDown = KnockedDown(player);
        const auto* current = camera->currentState.get();
        const bool gameplay = current && (current->id == RE::CameraState::kFirstPerson ||
            current->id == RE::CameraState::kThirdPerson);
        if (Disabled(true)) SyncReloadTime();

        // Keep the normal first/third-person camera updating during this event.
        if (disabled && (dead || knockedDown) && gameplay) BeginEvent(dead);
        bool resume;
        {
            std::lock_guard lock(stateMutex);
            resume = session.ResumeNative(dead, knockedDown, disabled);
        }
        if (resume) {
            // A menu/killmove keeps its state until its own return request.
            if (gameplay) {
                cam->SetState(cam->cameraStates[RE::CameraState::kBleedout].get());
                std::lock_guard lock(stateMutex);
                session.Reset();
            }
        } else if (disabled && current && current->id == RE::CameraState::kBleedout) {
            if (auto* normal = GameplayState(cam)) {
                BeginEvent(dead); // also covers engine paths that inline SetState
                if (dead) SyncReloadTime();
                // Native End still cleans up audio and free look. Slow motion
                // belongs to the event and survives this camera-only transition.
                cam->SetState(normal);
                spdlog::info("[CameraOverride] Left active {} camera; gameplay control retained", dead ? "death" : "ragdoll");
            }
        }
        UpdateGameplayLook(cam, player, dead, knockedDown);
    }
}
