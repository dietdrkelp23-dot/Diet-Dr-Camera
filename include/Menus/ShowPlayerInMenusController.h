#pragma once

namespace DietDrCamera
{
    // Forces the camera into a third-person framing of the player while
    // certain UI menus are open — Inventory / Container / Barter / Magic /
    // Tween — so the player character is visible inside the menu rather
    // than off-screen behind the panel. Each menu is independently
    // toggleable via SettingsManager.
    //
    // The controller is purely state — it does NOT install hooks. It's
    // driven from two existing call sites:
    //   1. Plugin.cpp's MenuOpenCloseEvent sink calls OnMenuOpenChange
    //      whenever a relevant menu opens or closes.
    //   2. HookManager's HookedThirdPersonUpdate calls ApplyFraming
    //      each frame so the engine's per-frame state writes don't
    //      clobber our overrides.
    class ShowPlayerInMenusController
    {
    public:
        [[nodiscard]] static ShowPlayerInMenusController& GetSingleton();

        // Called from the menu-open sink. menuName is the engine's name
        // (e.g., "InventoryMenu"). Looks up the per-menu enable flag in
        // SettingsManager; if the menu is enabled, captures pre-menu
        // camera state and activates the framing override. On close,
        // schedules a one-frame restore so the engine can re-establish
        // its own state.
        void OnMenuOpenChange(const std::string& menuName, bool a_opening);

        // True while the framing override is active and ApplyFraming
        // should run. Read from HookManager.
        [[nodiscard]] bool IsActive() const { return active_; }

        // True when the active menu's allowCameraControl is on. The
        // per-frame call sites use this to skip the framing writes so
        // the user's mouse / right-stick input drives freeRotation
        // and posOffset uncontested. The initial framing position is
        // still written once on open/swap from OnMenuOpenChange.
        [[nodiscard]] bool IsActiveCameraControlEnabled() const;

        // Called per frame from the third-person update hook AFTER
        // CameraController::Update so our writes win against per-state
        // profile FOV / position. Writes freeRotation, posOffsetExpected,
        // worldFOV, and pins the player's body yaw. No-op if !IsActive().
        void ApplyFraming(RE::PlayerCamera*     a_camera,
                          RE::ThirdPersonState* a_thirdState,
                          RE::PlayerCharacter*  a_player);

        // Render-time matrix override. Some menus (Container) freeze
        // the engine's per-state camera update so writes to
        // posOffsetExpected / freeRotation are dead. This computes
        // the desired world translate + rotate from settings and
        // writes them directly to the NiCamera's world transform —
        // bypassing the frozen pipeline. Must NOT call
        // UpdateDownwardPass from inside the NiCamera::UpdateWorldData
        // hook; that re-enters the hook on the same NiCamera and
        // crashes. Returns true if applied.
        bool ApplyRenderMatrix(RE::NiCamera*        a_niCamera,
                               RE::PlayerCharacter* a_player);

        // Called per frame from the camera-update hook. If a close
        // event marked pendingRestore_ and no sibling-menu open
        // claimed the framing within the same tick, complete the
        // deferred restore here. Cheap no-op otherwise.
        void Tick();

        // Per-frame driver for menus the engine doesn't tick through
        // ThirdPersonState::Update (FavoritesMenu with kCustomRendering,
        // and any other paused-game menu where the normal camera-state
        // update is suspended). Called from UnpauseManager's
        // MainThreadHook which runs every frame regardless of pause
        // state. Idempotent with the third-person Update path — both
        // can fire on the same frame and the writes converge to the
        // same target values.
        void DrivePerFrameFallback();

        // Force the controller back to vanilla, whatever it currently thinks
        // its state is. For the freeze watchdog: while active_ is true the
        // controller pins the player's body yaw every frame and its TDM yaw
        // claim counts as legitimate to the orphan watchdog, so a stuck
        // active_ is indistinguishable from "the player cannot walk". No-op
        // when already inactive.
        void ForceRestoreToVanilla();

    private:
        ShowPlayerInMenusController() = default;

        // Tells whether a particular menu name should trigger framing
        // based on the current SettingsManager bools. Centralized so
        // the sink and any other code agree on the menu name → toggle
        // mapping.
        [[nodiscard]] static bool ShouldHandleMenu(const std::string& menuName);

        // Walk RE::UI::menuStack top-down and return the name of the
        // first menu in our handled set. Empty if none. Used by the
        // per-frame sync to detect a mid-menu settings change (e.g.
        // user toggles Enable Customization while Favorites is open).
        [[nodiscard]] static std::string FindTopmostRelevantMenu();

        // Fresh-activation logic shared between OnMenuOpenChange's
        // open path and the per-frame sync. Captures pre-menu state
        // and applies framing. Pre-condition: !active_.
        void ActivateForMenu(const std::string& menuName);

        // Restore the pre-menu camera state captured at activation and
        // clear active_. Shared between the close path and the
        // "swap to a non-framed menu" branch of the open path (e.g.
        // Tween→Barter where Barter rides the dialogue camera and
        // wants no framing override leaking through).
        void DoRestoreToVanilla();

        bool  active_ = false;
        // The menu name that opened the framing — needed because the
        // user might disable the toggle while the menu is open, in
        // which case we should still close out cleanly when it closes.
        std::string activeMenuName_;

        // WHICH third-person-like state this activation took over:
        // kThirdPerson on foot, kMount on a horse, kDragon while riding one.
        // Latched at activation and used by every later path, so the restore
        // can never write to a different instance than the save read from —
        // which is what stranded the mount camera on 2026-08-23.
        RE::CameraState activeStateId_ = RE::CameraState::kThirdPerson;

        // User camera-orbit offset accumulated WHILE a framing is active
        // (2026-08-15). With Enable Customization on AND camera control on,
        // the engine's look input writes freeRotation.x each frame and the
        // framing used to overwrite it — "isn't letting me move the
        // camera". ApplyFraming folds the per-frame delta into this offset
        // instead, and both the source writes and the render matrix compose
        // entry->yaw + userOrbitYaw_. Reset on activation/deactivation.
        float userOrbitYaw_   = 0.0f;
        float lastWrittenFrX_ = 0.0f;
        bool  frWriteValid_   = false;

        // Pre-menu camera state, captured on activation, restored on
        // deactivation. We only cache fields we touch; the engine
        // owns the rest.
        bool   savedFreeRotationEnabled_ = false;
        bool   savedToggleAnimCam_       = false;
        float  savedFreeRotationX_       = 0.0f;
        float  savedFreeRotationY_       = 0.0f;
        float  savedPosOffsetExpectedX_  = 0.0f;
        float  savedPosOffsetExpectedY_  = 0.0f;
        float  savedPosOffsetExpectedZ_  = 0.0f;
        float  savedPosOffsetActualX_    = 0.0f;
        float  savedPosOffsetActualY_    = 0.0f;
        float  savedPosOffsetActualZ_    = 0.0f;
        float  savedPlayerAngleZ_        = 0.0f;
        float  savedPlayerAngleX_        = 0.0f;
        float  savedWorldFOV_            = 80.0f;
        // Stored as an offset from the player, not a world point, because the
        // Tween menu can be UNPAUSED - restoring an absolute position after
        // the player walked would teleport the camera to where they used to
        // be.
        RE::NiPoint3 savedCamOffsetFromPlayer_{};
        bool         savedCamOffsetValid_       = false;
        // ZOOM. ThirdPersonState::Begin resets these too, and the 3p update
        // eases current toward target INSIDE the engine tick — so both have to
        // be pinned or the Tween open snaps in distance. Same lesson as the
        // 1p->3p dialogue-exit zoom snap.
        float  savedCurrentZoomOffset_   = 0.0f;
        float  savedTargetZoomOffset_    = 0.0f;
        bool   forcedThirdPerson_        = false;
        // True when activation applied framing (Enable was on). False
        // in camera-control-only mode. Gates the body-yaw restore on
        // close — must NOT snap the player back to pre-menu yaw if the
        // player rotated freely during the menu.
        bool   framingApplied_           = false;
        // True when activation wrote vanilla-pose TPS/body values
        // (force3p && !enabled). Triggers the same restore path
        // framingApplied_ does — without this flag the body-yaw
        // overwrite from ApplyFraming wouldn't be undone on close,
        // leaving the player rotated.
        bool   wroteVanillaPose_         = false;
        // Body-yaw target the framing math locks to (writes to
        // player.data.angle.z and feeds TDM::SetPlayerYaw, also the
        // base of ApplyRenderMatrix's world yaw). Initialized to
        // savedPlayerAngleZ_ on first activation. Re-derived on
        // swap-into-framing so the camera direction the user
        // established during camera-control-only carries through —
        // otherwise the freeRotation.x reset would snap the view.
        // Distinct from savedPlayerAngleZ_, which stays at the
        // pre-activation yaw for the close-path restore.
        float  lockAngleZ_               = 0.0f;
        // Set true when the active menu closes — defers the actual
        // restore by one frame so a sibling-menu open in the same tick
        // (e.g., select-button Inventory→Magic) can pick up the
        // framing without going through vanilla. Cleared either by
        // the next open event (treated as a swap) or by Tick() which
        // then runs the real restore.
        bool   pendingRestore_           = false;
    };
}
