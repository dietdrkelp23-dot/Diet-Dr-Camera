#pragma once

namespace DietDrCamera
{
    class HookManager
    {
    public:
        [[nodiscard]] static HookManager& GetSingleton();

        void Install();

        // Cancels the shield-charge animation's camera injection (pitch +
        // height) at cameraRoot->local — the same post-compose, pre-propagate
        // stage every 3p noise layer writes, which is the only stage where a
        // camera correction reaches the WORLD and the SKY together. Writing
        // the NiCamera's world transform instead (the first attempt) sheared
        // the two apart — "the entire sky shakes" — because the sky samples
        // the camera at a different point than scene geometry. PUBLIC because
        // it is called from CameraNoiseController's TESCamera::Update thunk,
        // before the noise layers so they still ride on top.
        static void EnforceShieldSprintCamera(RE::TESCamera* a_camera);

        // ----- The animated-camera window (shield charge) ------------------
        //
        // THE MECHANISM, found 2026-08-27 and confirmed in the user's own log:
        // the shield-charge animation fires the anim event
        // 'StartAnimatedCameraDelta'; the engine's registered handler
        // (AnimatedCameraDeltaStartHandler, RTTI in SkyrimSE.exe) then lets
        // the animation's Camera3rd[Cam3] track drive the camera until
        // 'EndAnimatedCamera', blended on iAnimatedTransitionMillis. Every
        // consumer-stage correction failed BECAUSE this is an input-side
        // mode, not a camera-state computation.
        //
        // The fix is dispatch-side to match (CONFIRMED 2026-08-28): the
        // handler's vtable is found by RTTI name and its ExecuteHandler
        // suppressed for the player's charge (recent-block gate), and — the
        // last leak — suppression also executes the engine's own
        // EndAnimatedCamera functor from the HandlerDictionary, because a
        // charge entered from an open plain-sprint stride-bob window needs
        // no new Start. Plain sprint keeps its bob; no other camera
        // animation anywhere is affected. The bone hold below survives only
        // as a probe: both it and toggleAnimCam were measured inert while
        // the tilt rendered.
        static void NotifyAnimatedCameraEvent(bool a_start);   // from the graph sink
        static void ApplyAnimCamBoneHold(RE::TESCamera* a_camera);  // thunk-site write

        // Called from Plugin.cpp's menu-open sink when the main menu opens,
        // so we can release the death-free-look raw-input registration even
        // though BleedoutCameraState::End() is never invoked by the engine
        // on the death-to-main-menu transition.
        static void ReleaseDeathFreeLookInput();

        // Request the ragdoll-cinematic slow motion to fade back to normal early.
        // Called from the input sink when the ragdoll fade hotkey fires; the
        // bleedout Update driver consumes it on the next frame.
        static void RequestRagdollSlowmoFade();

        // Whether the current bleedout was entered by an ACTUAL player death
        // (true) vs a recoverable ragdoll/knockdown (false). Latched at
        // BleedoutCameraState::Begin — the same value that decides whether the
        // death-cam or ragdoll-cam effects run. Quick Tune reads it to show the
        // right (Death vs Ragdoll) live-tuning layout. Meaningless outside
        // bleedout; gate on the camera state first.
        [[nodiscard]] static bool IsBleedoutRealDeath();

        // (Killcams: the custom "Diet Dr Killcam" was cut 2026-08-16, and the
        // whole Kill Camera feature — disablers, Custom Angles, guaranteed
        // executions — was removed 2026-08-17. DDC does not touch killcams.
        // Three attempts; read the killcam-module notes before a fourth.)

        // Called from Plugin.cpp's MenuCloseSink on Dialogue Menu open/close.
        // Drives the face-lock gate's lastSpeaker fallback: the fallback is
        // only valid while the menu is actually open.
        static void OnDialogueMenuOpenChange(bool a_open);

        // Called from Plugin.cpp's MenuCloseSink on MapMenu open/close.
        // While the map is up, none of DDC's FOV pipelines tick (the camera
        // sits in the Map state, and the paused game stops the 1p/3p state
        // updates), so whatever worldFOV the last pipeline wrote stays
        // latched and the map renders with it — most visibly a zoomed
        // dialogue FOV when Better Carriage Destinations opens the map
        // mid-conversation. On open this restores the game's default world
        // FOV for the map; on close the pipelines resume within a frame.
        static void OnMapMenuOpenChange(bool a_open);

        // Last non-Tween POV the engine ticked. Updated each frame from
        // HookedFirstPersonUpdate / HookedThirdPersonUpdate. kTween (menu
        // overlay) doesn't update it, so menu-open code paths can read
        // this to recover the player's actual POV preference.
        [[nodiscard]] static bool WasLastFrameFirstPerson();

        static void PreserveMenuCameraAnimation();
        static void RestoreMenuCameraAnimationSource(RE::ThirdPersonState* state);
        static void ResetMenuCameraAnimation();

        // Announce a deliberate re-entry into kFirstPerson so the follow
        // spring survives it. FirstPersonState::Begin drops the camera's
        // follow anchor (lastPosition) onto the player, and the running trail
        // — 21 units at speed — is paid back in a single frame. Any code that
        // calls SetState(kFirstPerson) while the player may be moving must
        // call this first; the next first-person update then carries the
        // anchor across instead of starting from a collapsed one.
        static void ArmFirstPersonSpringCarry();

        // True while UnpauseManager's MainThreadHook is inside the unpaused UI
        // drive. Any camera update that runs under that flag is the EXTRA
        // pass our own drive caused, not the game's — it must not advance the
        // camera. This is the causal test for the double pump; a movement
        // test is not, because a standing player looks identical on both
        // passes and freezing BOTH is what made stopping stutter.
        static void SetUiDriveActive(bool a_active);
        [[nodiscard]] static bool IsUiDriveActive();

        // True while the 3p dialogue body-face holds a TDM yaw-control claim
        // (keeping the player oriented at a non-Actor speaker like the Statue
        // of Mara). A fourth legitimate yaw holder the orphan watchdog in
        // CameraController must know about, or it reaps the claim every frame.
        [[nodiscard]] static bool IsDialogueFacingBodyYaw();

        // (Adaptive collision / Cinematic Space Awareness state removed
        // 2026-08-16 — the feature was cut at user request; location
        // overrides cover per-place framing.)

    private:
        HookManager() = default;

        static void HookedThirdPersonUpdate(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static inline REL::Relocation<decltype(&HookedThirdPersonUpdate)> _originalThirdPersonUpdate;

        static void HookedGetRotation(RE::ThirdPersonState* a_this, RE::NiQuaternion& a_rotation);
        static inline REL::Relocation<decltype(&HookedGetRotation)> _originalGetRotation;

        static void HookedGetTranslation(RE::ThirdPersonState* a_this, RE::NiPoint3& a_translation);
        static inline REL::Relocation<decltype(&HookedGetTranslation)> _originalGetTranslation;

        // PER-VTABLE ORIGINALS. HorseCameraState and DragonCameraState derive
        // from ThirdPersonState but each has its OWN vtable, so the pointer we
        // displace in slot 0x3/0x4/0x5 is not necessarily ThirdPersonState's —
        // it is that class's override, or ANOTHER MOD'S HOOK if they installed
        // first. The horse patch used to throw the returned pointer away and
        // call _originalThirdPersonUpdate for every state, which silently
        // unhooked anyone already there. That is what crashed True
        // Directional Movement on dismount
        // (see the comment at the install site).
        static inline REL::Relocation<decltype(&HookedThirdPersonUpdate)> _originalHorseUpdate;
        static inline REL::Relocation<decltype(&HookedGetRotation)>       _originalHorseGetRotation;
        static inline REL::Relocation<decltype(&HookedGetTranslation)>    _originalHorseGetTranslation;
        static void HookedTrackingUpdateRotation(RE::ThirdPersonState* a_this);
        static inline REL::Relocation<decltype(&HookedTrackingUpdateRotation)> _originalTrackingUpdateRotation;
        static inline REL::Relocation<decltype(&HookedTrackingUpdateRotation)> _originalHorseTrackingUpdateRotation;
        static inline REL::Relocation<decltype(&HookedThirdPersonUpdate)> _originalDragonUpdate;
        static inline REL::Relocation<decltype(&HookedGetRotation)>       _originalDragonGetRotation;

        // Call the original for the vtable THIS state actually came from.
        // Every hook body must go through these instead of naming
        // _originalThirdPersonUpdate / _originalGetRotation / _originalGetTranslation
        // directly.
        static void CallOriginalUpdate(RE::ThirdPersonState* a_this,
                                       RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static void CallOriginalGetRotation(RE::ThirdPersonState* a_this, RE::NiQuaternion& a_rotation);
        static void CallOriginalGetTranslation(RE::ThirdPersonState* a_this, RE::NiPoint3& a_translation);
        static void CallOriginalProcessWeaponDrawnChange(RE::ThirdPersonState* a_this, bool a_drawn);

        // ThirdPersonState::ProcessWeaponDrawnChange (vtable 0x0B) — the engine's
        // sheathe/draw edge handler. See the definition for why we need it.
        static void HookedProcessWeaponDrawnChange(RE::ThirdPersonState* a_this, bool a_drawn);
        static inline REL::Relocation<decltype(&HookedProcessWeaponDrawnChange)> _originalProcessWeaponDrawnChange;
        // HorseCameraState genuinely DOES override this one (see its header:
        // `void ProcessWeaponDrawnChange(bool) override; // 0B`), which is why
        // the draw-edge guard never ran while mounted.
        static inline REL::Relocation<decltype(&HookedProcessWeaponDrawnChange)>
            _originalHorseProcessWeaponDrawnChange;

        // TogglePOVHandler::ProcessButton (vtable slot 4). Called when the
        // engine delivers a "Toggle POV" user event (keyboard F, controller
        // R3-hold). We catch it and call PlayerCamera::ForceFirstPerson/
        // ForceThirdPerson directly — that route swaps POV without going
        // through the zoom axis, so the zoom hard-lock can stay in place.
        static void HookedTogglePOVProcessButton(RE::TogglePOVHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data);
        static inline REL::Relocation<decltype(&HookedTogglePOVProcessButton)> _originalTogglePOVProcessButton;
        // Vtable slot 5 on HeldStateHandler — called per-frame from
        // PlayerControls. Vanilla's "hold past fPOVSwitchHoldDelay →
        // toggle POV" logic lives here, NOT in ProcessButton. Without
        // hooking this slot, swallowing ProcessButton alone isn't
        // enough to prevent the mid-hold toggle for keyboard F.
        static void HookedTogglePOVUpdateHeldState(RE::TogglePOVHandler* a_this, const RE::ButtonEvent* a_event);
        static inline REL::Relocation<decltype(&HookedTogglePOVUpdateHeldState)> _originalTogglePOVUpdateHeldState;

        // Camera-state input is separate from TogglePOVHandler. Preserve the
        // current orbit across R3 release while chaining all other input work.
        // The secondary vtable receives the PlayerInputHandler subobject.
        static void HookedOrbitProcessButton(RE::PlayerInputHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data);
        static inline REL::Relocation<decltype(&HookedOrbitProcessButton)> _originalOrbitProcessButton;
        static inline REL::Relocation<decltype(&HookedOrbitProcessButton)> _originalHorseOrbitProcessButton;

    public:
        // Raw gamepad R3-release sink. Subscribes to the BSInputDeviceManager's
        // InputEvent source and fires PlayerCamera::ForceFirst/ThirdPerson
        // on the release edge of the right-stick click, but only if the
        // press was held long enough to be a "hold" gesture (not a tap —
        // taps are reserved for TDM target-lock). This gives the POV
        // toggle an "on release" feel without depending on the user-event
        // Toggle POV channel (which some setups don't deliver to the
        // TogglePOVHandler — see session log Apr 21).
        class R3ReleaseSink : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override;
        };
        static void InstallR3ReleaseSink();
        // True while the user is currently holding R3 (right-stick click).
        // Read from HookedFirstPersonUpdate to suppress the engine's
        // auto-zoom-out-to-3p behavior during the hold, so the POV toggle
        // only fires on release.
        static inline bool s_r3Held = false;
        // POV state captured at the moment R3 was pressed. While R3 is
        // held, camera-state hooks force the camera to keep matching this
        // value — counteracting the engine's built-in R3-hold auto-toggle
        // so the POV only flips when R3 is released.
        static inline bool s_r3PressWasFirst = false;
        // M&K "Toggle POV" mirror of the R3 pair. Set when the user
        // presses F (or whatever they bound) in keyboard/mouse, cleared
        // on release. Camera-state hooks revert any POV change back to
        // s_fKeyPressWasFirst while s_fKeyHeld is true — same counter-
        // attack used for R3, but for vanilla's keyboard hold-fire
        // path which lives somewhere outside both TogglePOVHandler
        // vtable slots we hook.
        static inline bool s_fKeyHeld         = false;
        static inline bool s_fKeyPressWasFirst = false;

    private:
        static inline R3ReleaseSink _r3ReleaseSink{};

        // FirstPersonState hook — drives the first-person world/hands FOV overrides.
        static void HookedFirstPersonUpdate(RE::FirstPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static inline REL::Relocation<decltype(&HookedFirstPersonUpdate)> _originalFirstPersonUpdate;

        // Trampoline at PlayerCharacter::UpdateCamera +0xD7 — the same site
        // ICSE-NG uses. This call runs the engine's NiAVObject::Update on
        // firstPersonCameraObj, which finalises the 1p camera node's world
        // rotation for the frame. By hooking the call, we run immediately
        // AFTER the engine's pitch integrator — the last opportunity to
        // overwrite cameraNI->world.rotate before the renderer samples it.
        // Writing here is what bypasses the ~1s spring lag.

        // Trampoline at PlayerCamera::Update +0x1A6 (RelocationID 49852/50784).
        // Same site ICSE-NG uses for its camera-matrix patches. Runs AFTER
        // all per-state camera logic (pitch spring, rotation resolvers, etc.)
        // has set cameraNI->world.rotate for the frame — the final moment
        // we can overwrite it before the renderer samples. Hooking earlier
        // (e.g., PlayerCharacter::UpdateCamera+0xD7) leaves our writes
        // vulnerable to being overwritten by downstream engine logic.
        static void HookedUpdateCameraPost(RE::TESCamera* a_camera);
        static inline REL::Relocation<decltype(&HookedUpdateCameraPost)> _originalUpdateCameraPost;
        static void InstallUpdateCameraPostHook();

        // NiCamera::UpdateWorldData vtable hook (slot 0x30). The engine's
        // scene-graph recompose for the camera. Runs AFTER PlayerCamera::Update
        // returns, so writing world.rotate here is the last word before
        // the renderer reads it.
        static void HookedNiCameraUpdateWorldData(RE::NiCamera* a_this, RE::NiUpdateData* a_data);
        static inline REL::Relocation<decltype(&HookedNiCameraUpdateWorldData)> _originalNiCameraUpdateWorldData;
        static void InstallNiCameraUpdateWorldDataHook();

        // PlayerCameraTransitionState::Update (slot 0x3). We hook this so we
        // can detect an in-flight 1p→3p transition and abort it by calling
        // ForceFirstPerson when the user is holding R3 from a 1p press —
        // prevents the engine's auto-toggle from showing any mid-transition
        // 3p frames during the hold.
        static void HookedTransitionUpdate(RE::PlayerCameraTransitionState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static inline REL::Relocation<decltype(&HookedTransitionUpdate)> _originalTransitionUpdate;

        // BleedoutCameraState hooks.
        // Begin (slot 0x1) writes fPlayerDeathReloadTime *before* the engine
        // caches it for the reload-prompt countdown — the only way to make
        // the duration slider take effect on the very first death of a
        // session. Update (slot 0x3) applies FOV and drives free look.
        static void HookedBleedoutBegin(RE::BleedoutCameraState* a_this);
        static inline REL::Relocation<decltype(&HookedBleedoutBegin)> _originalBleedoutBegin;

        static void HookedBleedoutEnd(RE::BleedoutCameraState* a_this);
        static inline REL::Relocation<decltype(&HookedBleedoutEnd)> _originalBleedoutEnd;

        static void HookedBleedoutUpdate(RE::BleedoutCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static inline REL::Relocation<decltype(&HookedBleedoutUpdate)> _originalBleedoutUpdate;

        // TweenMenuCameraState::Update — the engine's tween state EASES
        // worldFOV toward its own tween view angle every tick, recomputing
        // from its internal state (measured 2026-08-31: 90.35 -> 100.00 at
        // ~0.35°/frame), so no later-in-frame hold can win. Post-original,
        // re-assert the menu FOV freeze so an unconfigured Tween menu
        // behaves like a pause.
        static void HookedTweenMenuUpdate(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState);
        static inline REL::Relocation<decltype(&HookedTweenMenuUpdate)> _originalTweenMenuUpdate;

        // Furniture camera kill switch. Patches the `call` inside
        // TESFurniture::Activate that asks PlayerCamera to enter
        // kFurniture; our replacement skips the request entirely for
        // normal chairs/thrones/beds and just keeps the player in
        // their current POV (or forces a specific POV if the furniture
        // has FurnitureForces1stPerson keyword). Workbenches pass
        // through to vanilla. Port of Ersh's NoFurnitureCamera (MIT).
        static void InstallFurnitureCameraKill();
        static void EnterFurniture(RE::PlayerCamera* a_camera, RE::TESFurniture* a_furniture);
        static inline REL::Relocation<decltype(EnterFurniture)> _originalEnterFurniture;

        // MenuControls::ProcessEvent (slot 0x01). Relabels movement IDEvents
        // (forward/back/strafe/leftStick) while the Dialogue Menu is open so
        // PlayerControls' MovementHandler keeps receiving them, plus Jump and
        // Sneak so their handlers do too. Pattern from Vermunds'
        // DialogueMovementEnabler-SE (MIT). Gated on
        // SettingsManager::dialogueMovementEnabled for live toggle.
        static RE::BSEventNotifyControl HookedMenuControlsProcessEvent(RE::MenuControls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source);
        static inline REL::Relocation<decltype(&HookedMenuControlsProcessEvent)> _originalMenuControlsProcessEvent;

        // Engine camera collision raycast (RELOCATION_ID 32270/33007).
        // Three mutually-exclusive entry states, swapped live by
        // UpdateCameraCasterPatch via REL::safe_write:
        //   Original   - restore the engine's swept-hull cast (collision fully on).
        //   FalseStub  - "xor eax,eax; ret" so the cast always reports no hit
        //                (master disable, no exceptions).
        //   Divert     - 5-byte JMP to an abs-jump stub that lands in
        //                HookedCameraCaster, which scopes per-contact filtering
        //                around the native sweep (Ground/Doors/Trees/Walls).
        // Calls the original by temporarily restoring its saved entry bytes;
        // no unverified relocated-prologue/forward-pointer is used.
        enum class CasterPatchState { kOriginal, kFalseStub, kDivert };
        static inline std::uintptr_t    cameraCasterAddr       = 0;
        static inline std::uint8_t      cameraCasterOrigBytes[6]{};
        static inline std::uintptr_t    cameraCasterDivertStub = 0;
        static inline CasterPatchState  cameraCasterState      = CasterPatchState::kOriginal;
        static void UpdateCameraCasterPatch();

        // SmoothCam's verified typedef: bool(__fastcall*)(void* physics,
        //     bhkWorld*, glm::vec4& start, glm::vec4& end, uint32_t* resultInfo,
        //     Character** hitChar, float hullSize). On a hit the engine clips
        //     `end` to the impact point (read back by the caller) and returns true.
        //     The vec4s are passed by REFERENCE; CCVec4 mirrors glm::vec4's layout.
        struct CCVec4 { float x, y, z, w; };
        static bool HookedCameraCaster(
            void* a_physics, RE::bhkWorld* a_world,
            CCVec4& a_start, CCVec4& a_end,
            std::uint32_t* a_resultInfo, RE::Character** a_hitChar,
            float a_hullSize);
    };
}
