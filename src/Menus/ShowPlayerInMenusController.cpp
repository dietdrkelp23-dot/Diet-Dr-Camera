#include "PCH.h"
#include "Menus/ShowPlayerInMenusController.h"

#include "Hooks/HookManager.h"
#include "Hooks/TweenCameraTrace.h"
#include "LockOn/TDMIntegration.h"
#include "Settings/SettingsManager.h"
#include "Unpause/UnpauseManager.h"

#include <RE/B/BarterMenu.h>
#include <RE/C/ContainerMenu.h>
#include <RE/F/FavoritesMenu.h>
#include <RE/H/HorseCameraState.h>
#include <RE/I/InventoryMenu.h>
#include <RE/M/MagicMenu.h>
#include <RE/T/TweenMenu.h>
#include <RE/U/UI.h>

#include <chrono>

namespace DietDrCamera
{
    ShowPlayerInMenusController& ShowPlayerInMenusController::GetSingleton()
    {
        static ShowPlayerInMenusController instance;
        return instance;
    }

    namespace
    {
        // Defined further down (next to the mount-state reasoning it belongs
        // with); declared here because the menu-swap path above needs it to
        // re-validate a latched mount state before calling SetState.
        RE::ThirdPersonState* PlayerThirdPersonLikeState(RE::PlayerCamera* a_cam,
                                                         RE::PlayerCharacter* a_ply,
                                                         RE::CameraState* a_outId);

        // ===================================================================
        // NEVER `SetState` INTO A MOUNT CAMERA STATE. THIS IS THE CRASH.
        // ===================================================================
        //
        // Four identical crashes on 2026-08-23 (18:04, 18:19, 18:36, 18:55),
        // every one:
        //
        //   EXCEPTION_ACCESS_VIOLATION  subss xmm0,[rdi+0x50]  rdi = 0
        //   Hooks::HorseCameraStateHook::OnEnterState
        //
        // `HorseCameraState::horseRefHandle` is populated by the ENGINE's own
        // mount flow when IT enters the state. When DDC calls SetState on that
        // state object from a menu event, Begin runs with the handle still
        // empty — and True Directional Movement hooks that entry and
        // dereferences the horse without checking
        // (DirectionalMovementHandler.cpp:252 / :772; only its :1430 branch
        // null-checks). Result: instant CTD, in TDM's code, caused by DDC.
        //
        // The mounted camera is ALREADY in kMount during normal mounted
        // gameplay — the engine put it there — so the SetState was never
        // buying anything while mounted. It is pure downside. Field writes on
        // the state object are still fine and still happen; only the ENTRY is
        // forbidden.
        //
        // My three earlier attempts (re-validating the latched id, requiring
        // GetMount() to resolve, forcing third person from the mount Update)
        // all failed because they assumed the problem was a STALE mount. It
        // was not. A perfectly live mount crashes just the same — the handle
        // on the camera state is simply not DDC's to assume.
        // REFUSING OUTRIGHT WAS TOO BLUNT — it fixed the crash and broke the
        // feature. With the mount states banned, opening the Tween menu on a
        // horse left the camera behind again (the whole point of the mounted
        // SPIM rework the user signed off on). The crash was never "entering
        // kMount is wrong", it was "entering kMount with an EMPTY
        // horseRefHandle is wrong", because that is the reference TDM
        // dereferences unchecked on entry.
        //
        // So: make the entry SAFE instead of forbidden. Populate the handle
        // from the player's actual mount first, and only then allow SetState.
        // If there is no mount to point it at, refuse — that is the genuinely
        // broken case and third person is the correct fallback.
        bool CanEnterState(RE::PlayerCamera* a_cam, RE::CameraState a_id, const char* a_site)
        {
            if (a_id == RE::CameraState::kThirdPerson ||
                a_id == RE::CameraState::kFirstPerson) {
                return true;
            }
            if (!a_cam) return false;

            if (a_id == RE::CameraState::kMount) {
                auto* horseSt = skyrim_cast<RE::HorseCameraState*>(
                    a_cam->cameraStates[RE::CameraState::kMount].get());
                if (!horseSt) return false;

                // Already resolvable — the engine has been here and set it up.
                if (horseSt->horseRefHandle && horseSt->horseRefHandle.get()) return true;

                auto* ply = RE::PlayerCharacter::GetSingleton();
                RE::NiPointer<RE::Actor> mount;
                if (ply && ply->IsOnMount() && ply->GetMount(mount) && mount) {
                    horseSt->horseRefHandle = mount->CreateRefHandle();
                    static int sSeeded = 0;
                    if (sSeeded < 6) {
                        ++sSeeded;
                        spdlog::debug("[SPIM] seeded HorseCameraState::horseRefHandle from the "
                                     "player's mount before entering it at '{}' — without this "
                                     "TDM dereferences an empty handle on entry and CTDs "
                                     "({}/6)", a_site, sSeeded);
                    }
                    return true;
                }

                static int sRefused = 0;
                if (sRefused < 8) {
                    ++sRefused;
                    spdlog::warn("[SPIM] refusing SetState into the mount camera at '{}' — no "
                                 "live mount to point horseRefHandle at (refusal {}/8)",
                                 a_site, sRefused);
                }
                return false;
            }

            // kDragon and anything else derived: no way to validate what they
            // hold, so field writes only.
            static int sRefusedOther = 0;
            if (sRefusedOther < 8) {
                ++sRefusedOther;
                spdlog::warn("[SPIM] refusing SetState into camera state {} at '{}' "
                             "(refusal {}/8)",
                             static_cast<int>(a_id), a_site, sRefusedOther);
            }
            return false;
        }

        // One-shot hint from DoRestoreToVanilla → ActivateForMenu. When
        // a swap drops force3p, DoRestoreToVanilla calls
        // PlayerCamera::ForceFirstPerson, but HookManager's
        // WasLastFrameFirstPerson tracker only updates on the next
        // FirstPersonState::Update tick — which hasn't happened yet by
        // the time DrivePerFrameFallback re-activates the new menu
        // the very next frame. Without this hint, the re-activation
        // reads the stale "3p" tracker value and the new menu's
        // force3p=false carve-out doesn't trigger. The hint is
        // consumed (cleared) on the next ActivateForMenu call.
        bool s_pendingForce1pHint = false;

        // Resolve the per-menu settings entry from a menu name. Returns
        // nullptr if the name isn't one we handle. Centralized so every
        // call site agrees on the menu-name → entry mapping.
        const SettingsManager::ShowPlayerInMenuEntry* EntryFor(const std::string& menuName)
        {
            const auto& s = SettingsManager::GetSingleton();
            if (menuName == "InventoryMenu")    return &s.showPlayerInInventory;
            if (menuName == "ContainerMenu")    return &s.showPlayerInContainer;
            if (menuName == "MagicMenu")        return &s.showPlayerInMagic;
            if (menuName == "TweenMenu")        return &s.showPlayerInTween;
            if (menuName == "FavoritesMenu")    return &s.showPlayerInFavorites;
            // BarterMenu intentionally absent — Barter rides the dialogue
            // camera. Only its unpauseGame flag is consulted (read directly
            // by UnpauseManager); no framing override is applied here.
            // Sleep/Wait Menu also absent — its render pipeline can't be
            // coerced into 3p mid-menu (see notes on the gutted attempt).
            return nullptr;
        }
    }

    namespace
    {
        // IS THE CUSTOMIZATION IN FORCE RIGHT NOW?
        //
        // "Enable Customization" AND, when the entry is dragon-scoped, the
        // player is actually riding a dragon. Every gate that used to read
        // `entry->enabled` reads this instead, so a dragon-only entry behaves
        // EXACTLY like an unticked one everywhere else — no framing, no
        // render-matrix override, and camera-control-only mode still available
        // — without a second flag threaded through ten call sites.
        //
        // Unpause / allow-movement deliberately do NOT consult this: the
        // toggle sits beside Enable Customization and scopes the camera work,
        // not whether the world keeps running.
        bool FramingOn(const SettingsManager::ShowPlayerInMenuEntry* a_entry)
        {
            if (!a_entry || !a_entry->enabled) return false;
            if (!a_entry->dragonOnly) return true;
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (!ply || !ply->IsOnMount()) return false;
            RE::NiPointer<RE::Actor> mount;
            if (!ply->GetMount(mount) || !mount) return false;
            // Race keyword, never TESObjectREFR::IsDragon() — that one
            // dereferences an unchecked null inside CommonLibSSE and crashed
            // this file twice on 2026-08-23.
            auto* base = mount->GetActorBase();
            return base && base->race && base->race->HasKeywordString("ActorTypeDragon");
        }
    }

    bool ShowPlayerInMenusController::IsActiveCameraControlEnabled() const
    {
        if (!active_ || activeMenuName_.empty()) return false;
        const auto* entry = EntryFor(activeMenuName_);
        if (!entry) return false;
        // Enable Customization wins: when the user explicitly enables
        // framing for this menu, allowCameraControl is interpreted as
        // "let mouse/right-stick input through to UnpauseManager's
        // LookHandler gate" — not as "skip framing per frame". Without
        // this, every menu with the (default) allowCameraControl=true
        // had the per-frame ApplyFraming/ApplyRenderMatrix call sites
        // gated off, so slider edits never propagated.
        return entry->allowCameraControl && !FramingOn(entry);
    }

    bool ShowPlayerInMenusController::ShouldHandleMenu(const std::string& menuName)
    {
        // We activate the controller when either:
        //   * Enable is on (full framing override — apply offsets/yaw/FOV)
        //   * allowCameraControl is on (force kThirdPerson so look input
        //     orbits the camera instead of routing to the engine's menu
        //     camera state which spins the player model)
        // ApplyFraming / ApplyRenderMatrix gate their actual writes on
        // FramingOn(entry) so the camera-control-only path doesn't lock
        // anything — the user gets a vanilla 3p shot they can orbit.
        const auto* entry = EntryFor(menuName);
        if (!entry) return false;
        // FavoritesMenu carries kCustomRendering and runs its own
        // camera pipeline. The camera-control-only path's
        // SetState(kThirdPerson) snaps the camera to vanilla 3p,
        // clobbering Favorites' pose ("snap forward" complaint). Only
        // activate when the user explicitly wants framing OR force3p —
        // customization-off + force3p-off behaves like vanilla Favorites.
        if (menuName == "FavoritesMenu") {
            return FramingOn(entry) || entry->force3pFromFirstPerson;
        }
        // A disabled feature must leave the menu's camera fully vanilla.
        // allowCameraControl defaults to true (mirrored from allowMovement)
        // and is only ever user-settable under Unpause, so it must NOT
        // trigger handling on its own -- otherwise the controller forces
        // kThirdPerson on every menu open even when the user opted out. For
        // menus that render through a non-3p menu camera (TweenMenu's kTween
        // especially) the camera-control-only SetState(kThirdPerson)
        // re-enters ThirdPersonState::Begin and snaps freeRotation with
        // nothing to mask it (the visible Tween open-flicker). So only
        // handle when the user actually opted in: framing (enabled),
        // unpause, or force-3p-from-1p. When Unpause is on, the unpaused
        // camera-control / movement sub-behaviour is then governed by
        // allowCameraControl / allowMovement as before.
        return FramingOn(entry) || entry->unpauseGame ||
               entry->force3pFromFirstPerson;
    }

    void ShowPlayerInMenusController::OnMenuOpenChange(const std::string& menuName,
                                                       bool                a_opening)
    {
        TweenCameraTrace::OnMenuEvent(menuName, a_opening);
        if (const auto* entry = EntryFor(menuName);
            entry && !FramingOn(entry) && ShouldHandleMenu(menuName)) {
            HookManager::PreserveMenuCameraAnimation();
        }
        // DIAG-stutter: time the whole handler. We're interested in the
        // swap branch (active_ && opening) — that's the SkyUI select-
        // button path.
        const auto diagT0 = std::chrono::steady_clock::now();
        const bool diagWasActive = active_;
        const std::string diagPrevMenu = activeMenuName_;
        auto diagLog = [&](const char* a_phase) {
            const double us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - diagT0).count();
            spdlog::debug("[DIAG-stutter] OnMenuOpenChange({},{}) {} prev={} wasActive={} took={:.0f}us",
                         menuName, a_opening ? "open" : "close",
                         a_phase, diagPrevMenu, diagWasActive, us);
        };
        if (a_opening) {
            // A pending restore from a prior close in this same tick
            // gets canceled here — this open re-claims the framing.
            // Common case: select-button switch from one framed menu
            // to another (Inventory ↔ Magic). The previous menu's
            // close event marked pendingRestore_; we drop it and fall
            // through to the swap branch below to apply the new menu's
            // framing without bouncing through vanilla.
            if (pendingRestore_) {
                pendingRestore_ = false;
            }
            // If we're already active for a different menu, treat this
            // open as a SWAP: switch activeMenuName_ to the new menu and
            // re-apply its framing on the persistent ThirdPersonState.
            // The original saved state (captured on the first activation)
            // stays put so the eventual close still restores to vanilla.
            // This handles the Tween→Inventory transition: TweenMenu opens
            // first, then InventoryMenu opens on top before TweenMenu's
            // close fires. Without this swap, Inventory's open would bail
            // and Tween's framing would persist under Inventory's name.
            if (active_) {
                if (menuName == activeMenuName_) { diagLog("same-name early-out"); return; }
                if (!ShouldHandleMenu(menuName)) {
                    // The new menu isn't configured for framing
                    // (e.g. Barter routes through the dialogue camera).
                    // Fully restore vanilla state so the new menu starts
                    // from a clean baseline — otherwise Tween's framing
                    // would persist under Barter and fight the dialogue
                    // camera.
                    DoRestoreToVanilla();
                    diagLog("unhandled-new-restore");
                    return;
                }
                // POV-flip on swap: if we were forced into 3p by the
                // previous menu's force3pFromFirstPerson AND the new
                // menu doesn't want that, restore vanilla so the
                // camera drops back to the pre-menu 1p before the new
                // menu re-applies its own framing (which in this case
                // is "stay in 1p"). Without this the controller would
                // stay active in 3p and the new menu's force3p=false
                // toggle would be ignored.
                if (forcedThirdPerson_) {
                    const auto* swapEntryPov = EntryFor(menuName);
                    if (!swapEntryPov || !swapEntryPov->force3pFromFirstPerson) {
                        DoRestoreToVanilla();
                        diagLog("swap-drops-force3p");
                        return;
                    }
                }
                activeMenuName_ = menuName;
                auto* camSwap = RE::PlayerCamera::GetSingleton();
                auto* plySwap = RE::PlayerCharacter::GetSingleton();
                if (camSwap && plySwap) {
                    // RE-RESOLVE THE STATE ID, do not trust the latch.
                    //
                    // activeStateId_ is latched when the menu opens, and the
                    // field-restore paths below deliberately keep using it so
                    // saved values go back to the instance they came from.
                    // But this path calls SetState, and SetState into a MOUNT
                    // state whose mount no longer exists is fatal: TDM hooks
                    // HorseCameraState::OnEnterState and reads the horse
                    // unconditionally. CRASH 2026-08-23 18:04:08 —
                    // EXCEPTION_ACCESS_VIOLATION at
                    // `subss xmm0,[rdi+0x50]`, rdi = 0, inside
                    // Hooks::HorseCameraStateHook::OnEnterState, on a
                    // dismount. Writing stale FIELDS is harmless; ENTERING a
                    // stale state is not.
                    RE::CameraState liveSwapId = activeStateId_;
                    auto* tpsLive = PlayerThirdPersonLikeState(camSwap, plySwap, &liveSwapId);
                    if (liveSwapId != activeStateId_) {
                        spdlog::debug("[SPIM] menu swap: latched camera state {} is stale "
                                     "(player is now {}) — retargeting to {}",
                                     static_cast<int>(activeStateId_),
                                     plySwap->IsOnMount() ? "mounted" : "on foot",
                                     static_cast<int>(liveSwapId));
                        activeStateId_ = liveSwapId;
                    }
                    if (auto* tpsSwap = tpsLive)
                    {
                        // Detect mode transition. The new menu might be
                        // framed (enabled on) or camera-control-only.
                        const auto* swapEntry  = EntryFor(menuName);
                        const bool newFraming  = FramingOn(swapEntry);
                        const bool wasFraming  = framingApplied_;

                        // Transitioning INTO framing for the first time:
                        // grab TDM yaw control and derive lockAngleZ_
                        // from the user's CURRENT camera world yaw so
                        // the framing's entry->yaw offset lands the
                        // camera right where they were looking (no
                        // visible snap). World camera yaw = body.angle.z
                        // + freeRotation.x; we want body + entry->yaw
                        // (post-framing) to equal that same value, so
                        // body = world_yaw - entry->yaw. Leave
                        // savedPlayerAngleZ_ alone so close-restore
                        // returns the player to the pre-activation yaw.
                        if (newFraming && !framingApplied_) {
                            framingApplied_ = true;
                            const float currentCameraYaw =
                                plySwap->data.angle.z + tpsSwap->freeRotation.x;
                            lockAngleZ_ = currentCameraYaw - swapEntry->yaw;
                            auto& tdm = TDMIntegration::GetSingleton();
                            if (tdm.IsAvailable()) {
                                tdm.RequestDisableDirectionalMovement();
                                tdm.RequestYawControl(0.0f);
                                tdm.SetPlayerYaw(lockAngleZ_);
                            }
                            plySwap->data.angle.z = lockAngleZ_;
                            plySwap->data.angle.x = 0.0f;
                            plySwap->Update3DPosition(true);
                        }
                        // Transitioning OUT of framing into a camera-
                        // control-only menu: release TDM so the body
                        // can rotate naturally, and RESTORE the fields
                        // framing wrote — the new menu is uncustomized,
                        // so nothing per-frame will ever overwrite them,
                        // and without this the old menu's framing pose
                        // (rotated body, offset camera, FOV) visibly
                        // lingered under the new menu. The user ruling
                        // (2026-08-15): a swap from a customized menu to
                        // an uncustomized one should look exactly like
                        // the previous menu was never customized and the
                        // game just unpaused. framingApplied_ clears with
                        // the restore, so the eventual close-restore
                        // won't re-snap values the user has since orbited
                        // (the same reasoning DoRestoreToVanilla's gate
                        // documents).
                        else if (!newFraming && framingApplied_) {
                            auto& tdm = TDMIntegration::GetSingleton();
                            if (tdm.HasYawControl()) tdm.ReleaseYawControl();
                            if (tdm.HasDirectionalMovementDisabled())
                                tdm.ReleaseDisableDirectionalMovement();
                            tpsSwap->freeRotationEnabled = savedFreeRotationEnabled_;
                            tpsSwap->toggleAnimCam       = savedToggleAnimCam_;
                            tpsSwap->freeRotation.x      = savedFreeRotationX_;
                            tpsSwap->freeRotation.y      = savedFreeRotationY_;
                            tpsSwap->posOffsetExpected.x = savedPosOffsetExpectedX_;
                            tpsSwap->posOffsetExpected.y = savedPosOffsetExpectedY_;
                            tpsSwap->posOffsetExpected.z = savedPosOffsetExpectedZ_;
                            tpsSwap->posOffsetActual.x   = savedPosOffsetActualX_;
                            tpsSwap->posOffsetActual.y   = savedPosOffsetActualY_;
                            tpsSwap->posOffsetActual.z   = savedPosOffsetActualZ_;
                            plySwap->data.angle.z = savedPlayerAngleZ_;
                            plySwap->data.angle.x = savedPlayerAngleX_;
                            camSwap->worldFOV     = savedWorldFOV_;
                            framingApplied_   = false;
                            wroteVanillaPose_ = false;
                        }

                        // Only run the synchronous apply when the
                        // mode actually changed (camera-control→framing
                        // or framing→camera-control). Same-mode swap
                        // (e.g., Inventory→Magic, both framed): the
                        // per-frame hooks will pick up the new entry's
                        // values on the next frame, and skipping these
                        // inline calls avoids a one-frame Scaleform-
                        // transition stutter from the redundant work
                        // happening in the event handler.
                        const bool modeChanged = (newFraming != wasFraming);
                        if (modeChanged) {
                            // SetState before ApplyFraming — Begin resets
                            // freeRotation, so framing written first would be
                            // wiped for the first frames of the new menu (same
                            // ordering bug as the activation path).
                            if (camSwap->currentState.get() != tpsSwap &&
                                CanEnterState(camSwap, activeStateId_, "menu-swap")) {
                                camSwap->SetState(tpsSwap);
                                if (!newFraming && !forcedThirdPerson_) {
                                    HookManager::RestoreMenuCameraAnimationSource(tpsSwap);
                                }
                            }
                            ApplyFraming(camSwap, tpsSwap, plySwap);
                            if (camSwap->cameraRoot) {
                                auto* asNode = camSwap->cameraRoot->AsNode();
                                if (asNode && !asNode->GetChildren().empty()) {
                                    if (auto* niCam = skyrim_cast<RE::NiCamera*>(asNode->GetChildren()[0].get())) {
                                        if (ApplyRenderMatrix(niCam, plySwap)) {
                                            using fn_t = void(*)(RE::NiCamera*);
                                            static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                                            updateW2S(niCam);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                diagLog("swap-branch");
                return;
            }

            if (!ShouldHandleMenu(menuName)) { diagLog("not-handled"); return; }
            ActivateForMenu(menuName);
            diagLog("fresh-activation");
        } else {
            // Close — only restore if we activated for this menu.
            // Other relevant menus closing while we're inactive is a
            // no-op; the user might have toggled off the feature.
            if (!active_) { diagLog("close-not-active"); return; }
            if (menuName != activeMenuName_) { diagLog("close-name-mismatch"); return; }
            // Defer the restore by one frame so a sibling-menu open
            // event in the same tick (e.g., select-button switch from
            // Inventory to Magic) can claim the framing without bouncing
            // through vanilla state. Tick() completes the restore on
            // the next frame if no claim arrived.
            pendingRestore_ = true;
            diagLog("close-deferred-restore");
        }
    }

    void ShowPlayerInMenusController::Tick()
    {
        if (!pendingRestore_) return;
        // No sibling-menu open consumed the pending restore — the user
        // genuinely closed out of the framed menu. Complete the
        // deferred restore now.
        pendingRestore_ = false;
        if (active_) DoRestoreToVanilla();
    }

    std::string ShowPlayerInMenusController::FindTopmostRelevantMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return {};
        // menuStack is bottom-to-top; iterate by index in reverse so
        // we pick the topmost menu we recognize. (BSTArray doesn't
        // expose reverse iterators.)
        for (std::size_t i = ui->menuStack.size(); i > 0; --i) {
            auto* menu = ui->menuStack[i - 1].get();
            if (!menu) continue;
            // Engine doesn't expose menu->name on IMenu directly; we
            // identify by skyrim_cast against the concrete types we
            // care about. Same set as Plugin.cpp's MenuOpenCloseEvent
            // filter.
            if (skyrim_cast<RE::FavoritesMenu*>(menu))  return "FavoritesMenu";
            if (skyrim_cast<RE::InventoryMenu*>(menu))  return "InventoryMenu";
            if (skyrim_cast<RE::ContainerMenu*>(menu))  return "ContainerMenu";
            if (skyrim_cast<RE::MagicMenu*>(menu))      return "MagicMenu";
            if (skyrim_cast<RE::TweenMenu*>(menu))      return "TweenMenu";
            if (skyrim_cast<RE::BarterMenu*>(menu))     return "BarterMenu";
        }
        return {};
    }

    namespace
    {
        // THE THIRD-PERSON-ISH STATE THE PLAYER ACTUALLY CAME FROM.
        //
        // Everything in this file used to reach for cameraStates[kThirdPerson]
        // by name, which is only correct on foot: a horse is kMount and a
        // ridden dragon is kDragon. Both DERIVE from ThirdPersonState
        // (`class HorseCameraState : public ThirdPersonState`, and DDC's own
        // hook install log says the same of DragonCameraState), so every field
        // this controller writes — freeRotation, posOffsetExpected/Actual,
        // toggleAnimCam, the zoom offsets — exists on them and means the same
        // thing. Picking the right instance is the whole of "make the menu
        // camera follow a mount the way it follows you on foot".
        RE::ThirdPersonState* PlayerThirdPersonLikeState(RE::PlayerCamera* a_cam,
                                                         RE::PlayerCharacter* a_ply,
                                                         RE::CameraState* a_outId)
        {
            if (!a_cam) return nullptr;
            auto id = RE::CameraState::kThirdPerson;
            // BOTH tests, and the mount pointer is REQUIRED. IsOnMount() alone
            // is not enough: during a dismount it can still read true while
            // GetMount() has already stopped resolving, and the old code set
            // id = kMount regardless of whether the pointer came back. Callers
            // then SetState into a horse camera with no horse — which is fatal,
            // because TDM hooks HorseCameraState::OnEnterState and dereferences
            // the mount unconditionally (crash 2026-08-23 18:04:08, rdi = 0).
            //
            // No mount pointer, no mount camera. Plain third person is always
            // safe to enter.
            RE::NiPointer<RE::Actor> mount;
            const bool haveMount = a_ply && a_ply->IsOnMount() &&
                                   a_ply->GetMount(mount) && mount;
            if (haveMount) {
                bool dragon = false;
                {
                    // NEVER TESObjectREFR::IsDragon(). It routes to
                    // HasKeywordWithType, which does:
                    //     auto keyword = *dobj->GetObject<BGSKeyword>(type);
                    // — an UNCHECKED dereference of a pointer that is null when
                    // the default-object entry is unfilled. That is a bug in
                    // CommonLibSSE, and it crashed this exact line twice on
                    // 2026-08-23 (`mov rdx,[rax]`, rax = 0, inside
                    // HasKeywordWithType, called from ActivateForMenu).
                    //
                    // The race keyword is the test the rest of DDC uses —
                    // FindNearestDragonShakeSource has run it on every nearby
                    // actor every frame for months — and it touches nothing but
                    // the form's own keyword list.
                    if (auto* base = mount->GetActorBase(); base && base->race) {
                        dragon = base->race->HasKeywordString("ActorTypeDragon");
                    }
                }
                id = dragon ? RE::CameraState::kDragon : RE::CameraState::kMount;
            }
            if (a_outId) *a_outId = id;
            auto* st = skyrim_cast<RE::ThirdPersonState*>(a_cam->cameraStates[id].get());
            // Fall back to plain third person if the mount state is somehow
            // unavailable — better a wrong-but-live state than a null deref.
            if (!st) {
                if (a_outId) *a_outId = RE::CameraState::kThirdPerson;
                st = skyrim_cast<RE::ThirdPersonState*>(
                    a_cam->cameraStates[RE::CameraState::kThirdPerson].get());
            }
            return st;
        }
    }

    void ShowPlayerInMenusController::ActivateForMenu(const std::string& menuName)
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!cam || !ply) return;

        // MOUNTS ARE HANDLED, NOT SKIPPED (2026-08-23, third attempt).
        //
        // Round 1 hijacked kThirdPerson and stranded the mount camera. Round 2
        // stood down and left the camera parked in kTween. Round 2b tried
        // SetState(kDragon) *and returned*, which crashed — the camera ended
        // up in the dragon state with active_ false, i.e. a half-configured
        // combination nothing was written for. Round 3 is the actual fix: run
        // the SAME path third person uses, against the mount's own
        // ThirdPersonState-derived instance (see PlayerThirdPersonLikeState).
        // active_ is set, the save/restore pair is symmetric, and the
        // per-frame driver keeps the camera on the player exactly as it does
        // on foot.
        //
        // The four things that are genuinely third-person-specific are gated
        // on `mounted` below: UpdateThirdPerson, ForceThirdPerson,
        // ForceFirstPerson, and the player body-yaw pin (the mount owns yaw
        // while riding).
        const bool mounted = ply->IsOnMount();

        // Acquire the state directly from the camera's state
        // array — NOT through currentState. By the time the
        // MenuOpenCloseEvent fires, the engine has already transitioned
        // the camera into the kTween state for menu rendering. The
        // state instances in cameraStates[] persist regardless of which
        // one is currently selected, so the one we want is always
        // reachable for us to mutate.
        auto* tps = PlayerThirdPersonLikeState(cam, ply, &activeStateId_);
        if (!tps) {
            spdlog::warn("[ShowPlayerInMenus] camera state unavailable — abort");
            return;
        }

        // Track pre-menu POV so the close path restores it. By the time
        // MenuOpenCloseEvent fires the engine has already switched
        // currentState to kTween, so we can't read currentState->id —
        // it's always kTween here. Use HookManager's per-frame POV
        // tracker, which is set from the actual FirstPerson/ThirdPerson
        // Update hooks (kTween doesn't tick those).
        bool wasFirstPerson = HookManager::WasLastFrameFirstPerson();
        // Honor a one-shot 1p hint left by a previous DoRestoreToVanilla
        // that forced 1p in the same tick. Without this, swap-drops-
        // force3p sees the stale 3p tracker and re-applies 3p framing.
        if (s_pendingForce1pHint) {
            wasFirstPerson = true;
            s_pendingForce1pHint = false;
        }

        // 1p carve-out: when the user is in first person AND the
        // per-menu "force 3p from 1p" toggle is off, skip framing
        // entirely. The engine has already transitioned the camera
        // to kTween for the menu render — without intervention the
        // camera locks at that tweened pose while the character walks
        // around behind a frozen view. Push state back to
        // kFirstPerson so vanilla 1p tracking resumes (camera follows
        // the character, no framing). active_ stays false so the
        // close path is a no-op; the engine handles the transition
        // back to the prior state.
        const auto* entryFor1pCarve = EntryFor(menuName);
        if (wasFirstPerson && entryFor1pCarve &&
            !entryFor1pCarve->force3pFromFirstPerson)
        {
            // ONLY IF THE CAMERA IS NOT ALREADY THERE, and never without
            // arming the follow-spring carry first.
            //
            // active_ stays false on this path, so DrivePerFrameFallback's
            // "user just enabled customization" branch calls ActivateForMenu
            // again on EVERY frame the menu is open — and this SetState ran
            // every one of them. SetState onto the state you are already in
            // still runs End+Begin, and FirstPersonState::Begin drops the
            // camera's follow anchor onto the player. Measured with the Tween
            // entry configured, 2026-09-01 19:06:51, player running:
            //
            //   [TWEENCAM] distRoot 21.02 -> 0.00 on the open frame, then
            //              10.22, 0.00, 10.00, 0.00, 10.11, 0.00 ...
            //
            // The whole 21u trail collapsed at the open and then the camera
            // alternated between 10 units behind the player and exactly on
            // them for the life of the menu. That is the "skips when unpaused
            // is enabled" report: it is not the unpause path at all, it is
            // this carve-out, and it only fires once the menu entry is
            // CONFIGURED (ShouldHandleMenu) — which is what enabling unpause
            // on the entry does.
            if (auto* fps = cam->cameraStates[RE::CameraState::kFirstPerson].get();
                fps && cam->currentState.get() != fps) {
                HookManager::ArmFirstPersonSpringCarry();
                cam->SetState(fps);
            }
            return;
        }
        // Mounted, the player is already in a third-person-like state by
        // definition — there is nothing to force, and ForceThirdPerson on a
        // mount would be the engine changing the mount camera out from under
        // us. Latch it false so every `forcedThirdPerson_` branch below (and
        // the ForceFirstPerson on close) is skipped.
        forcedThirdPerson_ = wasFirstPerson && !mounted;

        savedFreeRotationEnabled_ = tps->freeRotationEnabled;
        savedToggleAnimCam_       = tps->toggleAnimCam;
        savedFreeRotationX_       = tps->freeRotation.x;
        savedFreeRotationY_       = tps->freeRotation.y;
        savedPosOffsetExpectedX_  = tps->posOffsetExpected.x;
        savedPosOffsetExpectedY_  = tps->posOffsetExpected.y;
        savedPosOffsetExpectedZ_  = tps->posOffsetExpected.z;
        savedPosOffsetActualX_    = tps->posOffsetActual.x;
        savedPosOffsetActualY_    = tps->posOffsetActual.y;
        savedPosOffsetActualZ_    = tps->posOffsetActual.z;
        savedPlayerAngleZ_        = ply->data.angle.z;
        savedPlayerAngleX_        = ply->data.angle.x;
        savedWorldFOV_            = cam->worldFOV;
        // Camera position, player-relative, captured BEFORE any SetState /
        // Begin / framing write moves it. See the header for why this exists.
        savedCamOffsetValid_ = false;
        if (cam->cameraRoot) {
            const RE::NiPoint3 cw = cam->cameraRoot->world.translate;
            const RE::NiPoint3 pp = ply->GetPosition();
            savedCamOffsetFromPlayer_ = RE::NiPoint3{ cw.x - pp.x, cw.y - pp.y, cw.z - pp.z };
            savedCamOffsetValid_      = true;
        }
        savedCurrentZoomOffset_   = tps->currentZoomOffset;
        savedTargetZoomOffset_    = tps->targetZoomOffset;
        // THE REST OF THE ENGINE'S PERSISTENT 3P STATE (2026-09-04). Every
        // field above was pinned across the unframed-menu synchronous update
        // below and the Tween-open snap survived every pin: the 19:45:16-18
        // log (three opens in three seconds, standing still, weapon drawn)
        // shows the activation frame rendering FOV 90 against the profile's
        // 75, the camera 4.85u to the right ([SNAP] dump 3/12: dCam=4.87 with
        // side, zoom, orbit and preX all unchanged) and yaw +0.7 deg, then
        // FOV back next tick and the position walking back at a CONSTANT
        // 0.14u/frame - a rate-limited return, not a lerp, which is the shape
        // of the engine easing out of a collision state. These are the
        // ThirdPersonState fields Begin / UpdateThirdPerson can reset that
        // nothing restored: the collision result, the yaw-smoothing pair and
        // the saved/pitch zoom, plus PlayerCamera::worldFOV. Restored around
        // the synchronous update exactly like the fields above; identical
        // write-backs when the engine left them alone.
        const RE::NiPoint3 preCollisionPos   = tps->collisionPos;
        const float        preCollisionValid = tps->collisionPosValid;
        const float        preTargetYaw      = tps->targetYaw;
        const float        preCurrentYaw     = tps->currentYaw;
        const float        preSavedZoom      = tps->savedZoomOffset;
        const float        prePitchZoom      = tps->pitchZoomOffset;
        // The camera's own rotation quaternion (2026-09-05). The 14:10 [SNAP]
        // capture decomposed the open-frame jump: ThirdPersonState::translation
        // itself moved 25.92 -> 30.78 on the open frame while every pinned
        // field held (Cam3 pivot 0.00, collision unset, posOffset 30.0 both
        // sides), then walked back 0.14u/frame. translation is composed from
        // THIS field, which Begin resets and nothing here pinned - a 1.75 deg
        // yaw difference at 133u of zoom is exactly that lateral gap.
        const RE::NiQuaternion preRotation = tps->rotation;
        const auto quatYawDeg = [](const RE::NiQuaternion& q) {
            const float fx = 2.0f * (q.x * q.y - q.w * q.z);
            const float fy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            return std::atan2(fx, fy) * 57.2957795f;
        };

        // Fresh menu, fresh orbit — the user's look offset from the last
        // framed menu must not carry over.
        userOrbitYaw_ = 0.0f;
        frWriteValid_ = false;

        active_         = true;
        activeMenuName_ = menuName;

        const auto* entry = EntryFor(menuName);
        const bool framingMode = entry && FramingOn(entry);

        // lockAngleZ_ MUST be set before ApplyFraming — ApplyFraming
        // writes a_player->data.angle.z = lockAngleZ_, so a stale
        // lockAngleZ_ (e.g. 0 from default-init) clobbers the player's
        // body yaw. Original code only ran this path for framingMode
        // (enabled=true) where ApplyFraming early-returned and the
        // lockAngleZ_ assignment was further down; the force3p &&
        // !enabled path I added bypasses that early-return so
        // lockAngleZ_ has to be valid by the time ApplyFraming runs.
        framingApplied_ = framingMode;
        lockAngleZ_     = savedPlayerAngleZ_;
        // Track that the vanilla-pose write path was used so
        // DoRestoreToVanilla knows to undo the TPS / body / FOV
        // writes even though framingApplied_ is false. Without this,
        // body yaw stays at our overwrite and the player ends up
        // facing whatever direction our write set (= 180° from
        // original if the synthesized pose was wrong).
        wroteVanillaPose_ = forcedThirdPerson_ && !framingMode;

        // ORDER MATTERS: enter kThirdPerson FIRST, then write the framing.
        //
        // SetState(tps) re-enters ThirdPersonState::Begin, which resets
        // freeRotation — so anything written before it is wiped for the menu's
        // first rendered frames. ApplyFraming used to run here, ahead of the
        // SetState below, which is why opening a menu could flash the camera
        // round to the player's forward axis (reads as a sideways snap) before
        // the per-frame fallback put the framing back a frame or two later.
        // Framing has to land ON TOP of Begin, not under it.
        //
        // Skip the SetState when we're already in 3p AND not going to
        // overwrite freeRotation in ApplyFraming. Calling SetState(tps)
        // while ALREADY in tps causes the engine to re-enter
        // ThirdPersonState::Begin, which resets freeRotation.x → camera
        // snaps to face the player's forward axis. Framed menus mask
        // this because ApplyFraming overwrites freeRotation immediately
        // after; Favorites-with-only-force3p doesn't overwrite, so the
        // snap is visible. Preserve original behavior for the framing
        // and 1p→3p paths.
        // Skip SetState only when the camera is GENUINELY already in the
        // kThirdPerson state instance -- calling SetState(tps) while already in
        // tps re-enters ThirdPersonState::Begin and resets freeRotation (a snap).
        // The old check used !forcedThirdPerson_ ("player wasn't in 1p"), but the
        // engine puts the camera in kTween for the TweenMenu even when the player
        // was in 3p -- so SetState got skipped and the camera was stranded in
        // kTween, which the engine's 3p follow doesn't tick (the "Tween menu
        // doesn't follow the player" bug; confirmed via [TWEENDIAG] camState=7
        // for the whole TweenMenu window). Test the actual current state so
        // kTween (and kFirstPerson) trigger the switch to 3p -- same condition
        // the menu-swap path already uses.
        const bool alreadyIn3p = (cam->currentState.get() == tps);
        const bool willOverwriteFreeRotation = framingMode;
        const bool changedState = (!alreadyIn3p || willOverwriteFreeRotation);
        if (changedState && CanEnterState(cam, activeStateId_, "activate")) {
            TweenCameraTrace::Sample("activate-state-before", true);
            cam->SetState(tps);
            if (!framingMode && !forcedThirdPerson_) {
                HookManager::RestoreMenuCameraAnimationSource(tps);
            }
            TweenCameraTrace::Sample("activate-state-after", true);
        }
        if (forcedThirdPerson_) {
            cam->ForceThirdPerson();
            // When force3p && !enabled, our ApplyFraming and
            // ApplyRenderMatrix early-return so nothing positions the
            // NiCamera. The engine's natural ThirdPersonState::Update
            // tick doesn't run while paused menus (Sleep/Wait) hold the
            // simulation — so the rendered NiCamera matrix stays at
            // the pre-menu 1p location even though state.id is now
            // kThirdPerson. Result visually reads as 1p. Calling
            // UpdateThirdPerson here forces the engine to refresh the
            // 3p camera position immediately, before the menu's first
            // frame renders.
            if (!framingMode) {
                bool weaponDrawn = false;
                if (auto* asState = ply->AsActorState()) {
                    weaponDrawn = asState->IsWeaponDrawn();
                }
                cam->UpdateThirdPerson(weaponDrawn);
            }
        } else if (changedState && !framingMode) {
            // Already-in-3p player entering an unframed, camera-control-only
            // menu (the TweenMenu case: the engine has parked the camera in
            // kTween, so `changedState` is true even though the player never
            // left third person). SetState alone leaves the 3p camera holding
            // whatever pose Begin produced until the next natural tick, and
            // that pose has none of our composed offsets on it — one or two
            // rendered frames of "camera jumped sideways", then it eases back
            // as the normal 3p update resumes. Drive one synchronous 3p update
            // so the first menu frame already shows the settled camera.
            //
            // Begin (re-entered by the SetState above) also RESETS
            // freeRotation and the shoulder-offset channels to engine
            // defaults, and ApplyFraming below early-returns for an unframed
            // menu — nothing masks that reset, so without the restore the
            // synchronous update renders a vanilla right-shoulder pose on the
            // body axis (the occasional "snaps right, then eases back" Tween
            // open). The pre-menu values were saved a few lines up; put them
            // back first so frame 1 shows the exact pre-menu orbit.
            tps->freeRotationEnabled = savedFreeRotationEnabled_;
            tps->freeRotation.x      = savedFreeRotationX_;
            tps->freeRotation.y      = savedFreeRotationY_;
            tps->posOffsetExpected.x = savedPosOffsetExpectedX_;
            tps->posOffsetExpected.y = savedPosOffsetExpectedY_;
            tps->posOffsetExpected.z = savedPosOffsetExpectedZ_;
            tps->posOffsetActual.x   = savedPosOffsetActualX_;
            tps->posOffsetActual.y   = savedPosOffsetActualY_;
            tps->posOffsetActual.z   = savedPosOffsetActualZ_;
            // ZOOM, pinned defensively rather than as a known fix. It is the
            // one channel the engine EASES rather than sets (UpdateThirdPerson
            // walks current toward target from inside the tick), so if Begin
            // ever reset it the first menu frame would render at the default
            // distance and ease back — the shape of the 1p->3p dialogue-exit
            // zoom snap. MEASURED: on 1.6.1170 Begin leaves both alone; the
            // [TWEEN] log reads "zoom cur=-0.200 tgt=-0.200 (was cur=-0.200
            // tgt=-0.200)", so this restore is a no-op here and was NOT the
            // Tween snap. That turned out to be the noise offset being dropped
            // in one frame when the camera enters kTween — fixed by the
            // suppression tail in CameraNoiseController. Kept because it costs
            // nothing and the log line is the standing proof either way.
            const float zoomCurAtOpen = tps->currentZoomOffset;  // what Begin left
            const float zoomTgtAtOpen = tps->targetZoomOffset;
            tps->currentZoomOffset   = savedCurrentZoomOffset_;
            tps->targetZoomOffset    = savedTargetZoomOffset_;
            // What Begin (re-entered by the SetState above) left in the
            // fields the earlier pins never covered - captured for the
            // [TWEEN] line, then put back (see the save block for why).
            const float beginCollisionValid = tps->collisionPosValid;
            const float beginTargetYaw      = tps->targetYaw;
            const float beginCurrentYaw     = tps->currentYaw;
            const float beginFov            = cam->worldFOV;
            tps->collisionPos      = preCollisionPos;
            tps->collisionPosValid = preCollisionValid;
            tps->targetYaw         = preTargetYaw;
            tps->currentYaw        = preCurrentYaw;
            tps->savedZoomOffset   = preSavedZoom;
            tps->pitchZoomOffset   = prePitchZoom;
            cam->worldFOV          = savedWorldFOV_;
            const float beginRotYaw = quatYawDeg(tps->rotation);
            tps->rotation          = preRotation;
            bool weaponDrawn = false;
            if (auto* asState = ply->AsActorState()) {
                weaponDrawn = asState->IsWeaponDrawn();
            }
            // PIN THE RENDERED CAMERA across the synchronous update.
            //
            // Re-asserting the DATA channels (below) is not enough: with every
            // channel provably identical across the open edge, the 2026-08-14
            // 17:12 [MENUCAM] capture still showed the camera displaced
            // sideways by the same +4.8u on EVERY open (camRel r 25.0→29.8,
            // and 12.1→16.9 at a different orbit — a constant lateral bias,
            // lock or no lock), then spring back over ~0.3s while the menu
            // sat open. That is this UpdateThirdPerson call moving the
            // rendered camera with the engine's own instant composition,
            // which does not agree with the steady-state per-frame path the
            // camera was resting in. The displaced pose then reads as "the
            // Tween menu snaps the camera" and the ease-back confirms it.
            //
            // This branch only runs for an already-in-3p player entering an
            // unframed menu, so the pre-call rendered pose is the correct,
            // settled 3p view — capture the camera root transform and put it
            // back after the call. The call is still made for its STATE
            // side effects (leaving the parked kTween pose armed correctly);
            // only its world-position write is unwanted.
            RE::NiTransform savedCamRootLocal{};
            RE::NiTransform savedCamRootWorld{};
            const bool haveCamRoot = cam->cameraRoot != nullptr;
            if (haveCamRoot) {
                savedCamRootLocal = cam->cameraRoot->local;
                savedCamRootWorld = cam->cameraRoot->world;
            }
            // ALSO pin ThirdPersonState::translation — the engine's PERSISTENT
            // camera-position state, which is what UpdateThirdPerson actually
            // writes (the cameraRoot pin above measured d=(0,0,0) while the
            // jump persisted — [SNAPXRAY] 18:59 then showed the activation
            // frame's 3p update LANDING on the identical raw pose at every
            // open: tpsOut r50.74 exactly, vs the converged steady pose 4.8u
            // away, walked back at slew pace). The next 3p update renders
            // FROM this field, so restoring the offsets but not translation
            // still yields one raw-pose frame that the per-frame path then
            // eases back out — the residual Tween snap.
            const RE::NiPoint3 savedTranslation = tps->translation;
            // NOT while mounted. UpdateThirdPerson is the engine's THIRD-PERSON
            // setup pass — it composes from fOverShoulder* and knows nothing
            // about a mount's framing, so on a horse or a dragon it would
            // compose the wrong pose entirely. The SetState above is what makes
            // the mount camera live again; the mount's own Update then follows
            // the player from the next tick, which is all this call was for.
            if (!mounted) {
                TweenCameraTrace::Sample("activate-setup-before", true);
                cam->UpdateThirdPerson(weaponDrawn);
                TweenCameraTrace::Sample("activate-setup-after", true);
            }
            RE::NiPoint3 updWrote{};
            const RE::NiPoint3 updTransWrote = tps->translation;
            tps->translation = savedTranslation;
            if (haveCamRoot) {
                updWrote = cam->cameraRoot->world.translate;
                cam->cameraRoot->local = savedCamRootLocal;
                cam->cameraRoot->world = savedCamRootWorld;
                RE::NiUpdateData camUd;
                cam->cameraRoot->Update(camUd);
            }

            // RE-ASSERT after the synchronous update, not just before it.
            //
            // UpdateThirdPerson is the engine's own third-person setup pass,
            // and it composes the shoulder offset from the game's INI values
            // (fOverShoulderPosX/Z, or the combat variants selected by the
            // weaponDrawn argument above) — NOT from the values DDC's profile
            // had a moment ago. So the restore above can be undone by the very
            // next call: the camera lands on the vanilla shoulder for a frame
            // and then eases back to the profile offset as CameraController
            // re-asserts it on the following tick. An ease from a wrong pose
            // back to the right one IS "the camera moves when the menu opens",
            // and it is invisible to any angle-only measurement because the
            // FACING never changes — only the offset does. Writing the saved
            // values again here costs nothing when the engine left them alone
            // (identical values), and closes that window when it did not.
            const float postUpdExpX = tps->posOffsetExpected.x;   // for [TWEEN] below
            const float postUpdExpZ = tps->posOffsetExpected.z;
            const float postUpdActX = tps->posOffsetActual.x;
            tps->freeRotationEnabled = savedFreeRotationEnabled_;
            tps->freeRotation.x      = savedFreeRotationX_;
            tps->freeRotation.y      = savedFreeRotationY_;
            tps->posOffsetExpected.x = savedPosOffsetExpectedX_;
            tps->posOffsetExpected.y = savedPosOffsetExpectedY_;
            tps->posOffsetExpected.z = savedPosOffsetExpectedZ_;
            tps->posOffsetActual.x   = savedPosOffsetActualX_;
            tps->posOffsetActual.y   = savedPosOffsetActualY_;
            tps->posOffsetActual.z   = savedPosOffsetActualZ_;
            tps->currentZoomOffset   = savedCurrentZoomOffset_;
            tps->targetZoomOffset    = savedTargetZoomOffset_;
            // ...and the fields the synchronous pass itself may have written
            // (what it wrote is captured first, for the [TWEEN] line).
            const float updCollisionValid = tps->collisionPosValid;
            const float updTargetYaw      = tps->targetYaw;
            const float updCurrentYaw     = tps->currentYaw;
            const float updFov            = cam->worldFOV;
            tps->collisionPos      = preCollisionPos;
            tps->collisionPosValid = preCollisionValid;
            tps->targetYaw         = preTargetYaw;
            tps->currentYaw        = preCurrentYaw;
            tps->savedZoomOffset   = preSavedZoom;
            tps->pitchZoomOffset   = prePitchZoom;
            cam->worldFOV          = savedWorldFOV_;
            const float updRotYaw = quatYawDeg(tps->rotation);
            tps->rotation          = preRotation;
            {
                static int sRotPinLines = 0;
                if (sRotPinLines < 8) {
                    ++sRotPinLines;
                    spdlog::info("[TWEEN] rotation pin ({}/8): yaw pre={:.2f} begin={:.2f} upd={:.2f} -> restored pre "
                                 "| yaw tgt/cur pre={:.3f}/{:.3f} freeRotX={:.3f} bodyYaw={:.3f}",
                                 sRotPinLines, quatYawDeg(preRotation), beginRotYaw, updRotYaw,
                                 preTargetYaw, preCurrentYaw, tps->freeRotation.x, ply->data.angle.z);
                }
            }

            // INFO for the first six opens of a session: this line carries the
            // one measurement that names the mover - camPinned / transPinned
            // are what the synchronous pass moved the root and the state's
            // translation by - and it was debug-level through two sessions
            // that ran with Verbose off (2026-09-04).
            static int sTweenOpenInfoLines = 0;
            const auto tweenOpenLvl = (sTweenOpenInfoLines < 6) ? spdlog::level::info
                                                                : spdlog::level::debug;
            if (sTweenOpenInfoLines < 6) ++sTweenOpenInfoLines;
            spdlog::log(tweenOpenLvl, "[TWEEN] unframed 3p open: restored orbit freeRotX={:.3f} "
                         "posOffsetActual=({:.1f},{:.1f},{:.1f}) zoom cur={:.3f} tgt={:.3f} "
                         "(was cur={:.3f} tgt={:.3f}) weaponDrawn={} | "
                         "engineUpdateWrote posExp=({:.1f},_,{:.1f}) posAct.x={:.1f} "
                         "(saved exp=({:.1f},_,{:.1f}) act.x={:.1f}) -> reasserted | "
                         "camPinned d=({:.1f},{:.1f},{:.1f}) transPinned d=({:.1f},{:.1f},{:.1f})",
                         savedFreeRotationX_, savedPosOffsetActualX_,
                         savedPosOffsetActualY_, savedPosOffsetActualZ_,
                         savedCurrentZoomOffset_, savedTargetZoomOffset_,
                         zoomCurAtOpen, zoomTgtAtOpen, weaponDrawn,
                         postUpdExpX, postUpdExpZ, postUpdActX,
                         savedPosOffsetExpectedX_, savedPosOffsetExpectedZ_,
                         savedPosOffsetActualX_,
                         updWrote.x - savedCamRootWorld.translate.x,
                         updWrote.y - savedCamRootWorld.translate.y,
                         updWrote.z - savedCamRootWorld.translate.z,
                         updTransWrote.x - savedTranslation.x,
                         updTransWrote.y - savedTranslation.y,
                         updTransWrote.z - savedTranslation.z);
            // The verdict line for the extra pins, at INFO for the first few
            // opens of a session (the 2026-09-04 19:25 session ran with
            // Verbose off and every debug-level Tween line was lost). "pre"
            // is the pre-open value, "begin" what SetState's Begin left,
            // "upd" what the synchronous UpdateThirdPerson wrote. A field
            // that reads pre != begin or pre != upd is the one the open was
            // snapping through; all-equal means the snap is elsewhere still.
            {
                static int sTweenPinLines = 0;
                if (sTweenPinLines < 6) {
                    ++sTweenPinLines;
                    spdlog::info("[TWEEN] pins ({}/6): collisionValid pre={} begin={} upd={} | "
                                 "yaw tgt/cur pre={:.3f}/{:.3f} begin={:.3f}/{:.3f} upd={:.3f}/{:.3f} | "
                                 "fov pre={:.2f} begin={:.2f} upd={:.2f} | savedZoom pre={:.3f} "
                                 "pitchZoom pre={:.3f} -> all restored",
                                 sTweenPinLines,
                                 preCollisionValid, beginCollisionValid, updCollisionValid,
                                 preTargetYaw, preCurrentYaw, beginTargetYaw, beginCurrentYaw,
                                 updTargetYaw, updCurrentYaw,
                                 savedWorldFOV_, beginFov, updFov,
                                 preSavedZoom, prePitchZoom);
                }
            }
        }

        // Framing is written AFTER the state switch (see the ORDER MATTERS
        // note above) so ThirdPersonState::Begin can't wipe it.
        ApplyFraming(cam, tps, ply);

        if (framingMode) {
            auto& tdm = TDMIntegration::GetSingleton();
            if (tdm.IsAvailable()) {
                tdm.RequestDisableDirectionalMovement();
                tdm.RequestYawControl(0.0f);
                tdm.SetPlayerYaw(lockAngleZ_);
            }
            ply->data.angle.z = lockAngleZ_;
            ply->data.angle.x = 0.0f;
            ply->Update3DPosition(true);
        }

        if (cam->cameraRoot) {
            auto* asNode = cam->cameraRoot->AsNode();
            if (asNode && !asNode->GetChildren().empty()) {
                if (auto* niCam = skyrim_cast<RE::NiCamera*>(asNode->GetChildren()[0].get())) {
                    if (ApplyRenderMatrix(niCam, ply)) {
                        using fn_t = void(*)(RE::NiCamera*);
                        static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                        updateW2S(niCam);
                    }
                }
            }
        }

        spdlog::info("[ShowPlayerInMenus] activated for {} (was1p={}, forced3p={})",
                     menuName, wasFirstPerson, forcedThirdPerson_);
        TweenCameraTrace::Sample("activate-after", true);
    }

    void ShowPlayerInMenusController::DrivePerFrameFallback()
    {
        // Mid-menu settings sync: react to Enable Customization being
        // toggled while a relevant menu is already open. Without this,
        // changes wouldn't take effect until the user closed and
        // reopened the menu.
        const std::string topMenu = FindTopmostRelevantMenu();
        const bool topShouldHandle = !topMenu.empty() && ShouldHandleMenu(topMenu);

        if (!active_ && topShouldHandle) {
            // User just enabled customization (or allowCameraControl)
            // while the menu was already open. Activate now using the
            // current camera state as the "saved" baseline so disabling
            // later restores cleanly.
            ActivateForMenu(topMenu);
        } else if (active_ && !topShouldHandle && !topMenu.empty() && topMenu == activeMenuName_) {
            // User disabled customization while the menu is still open
            // (and Favorites' camera-control-only carve-out also pushes
            // us into this branch when Enable goes off). Restore the
            // saved baseline so the camera returns to where it was
            // pre-activation. The Plugin.cpp close event will arrive
            // later and become a no-op (active_ is already false).
            DoRestoreToVanilla();
            return;
        } else if (active_ && topMenu.empty()) {
            // The menu we activated for has closed but our pendingRestore_
            // path didn't fire (shouldn't normally happen — close event
            // sets pendingRestore_, Tick() drains it). Defensive
            // safety net: if nothing relevant is on the stack and we're
            // still active, restore now.
            DoRestoreToVanilla();
            return;
        }

        // Expire the one-shot 1p hint if nothing consumed it by now. It
        // only exists to bridge DoRestoreToVanilla's ForceFirstPerson to
        // a re-activation in the same tick (menu swap) or in the branch
        // above, where the POV tracker is still stale. Left alone, it
        // would persist until the NEXT menu open — possibly minutes
        // later, with the player genuinely in 3p — and mark them as 1p,
        // making the close path wrongly force 1p on a 3p player.
        s_pendingForce1pHint = false;

        if (!active_) return;
        // Camera-control-only mode: user input owns freeRotation /
        // posOffset; skip our framing writes the same way the
        // ThirdPersonState::Update site does.
        if (IsActiveCameraControlEnabled()) return;

        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!cam || !ply) return;

        // The state this activation took over — kMount / kDragon when the run
        // started mounted. Read the LATCHED id rather than re-deriving it, so
        // a dismount mid-menu cannot make the per-frame driver write to a
        // different instance than the save read from.
        auto* tps = skyrim_cast<RE::ThirdPersonState*>(
            cam->cameraStates[activeStateId_].get());
        if (!tps) return;

        // Same write set the third-person hook applies. ApplyFraming
        // gates internally on FramingOn(entry), so the camera-control-
        // only flow stays correct.
        ApplyFraming(cam, tps, ply);

        // (No per-frame fallback for force3p && !enabled — the
        // one-shot ApplyRenderMatrix call at activation now writes
        // the synthesized vanilla 3p pose, and DrivePerFrameFallback
        // doesn't tick during paused Sleep/Wait anyway.)

        // Matrix override on the live NiCamera. Without this, the
        // engine's kCustomRendering pipeline (FavoritesMenu) keeps
        // whatever world.translate / rotate it computed for its own
        // pose, and our posOffset writes never reach the renderer.
        if (cam->cameraRoot) {
            auto* asNode = cam->cameraRoot->AsNode();
            if (asNode && !asNode->GetChildren().empty()) {
                if (auto* niCam = skyrim_cast<RE::NiCamera*>(asNode->GetChildren()[0].get())) {
                    if (ApplyRenderMatrix(niCam, ply)) {
                        using fn_t = void(*)(RE::NiCamera*);
                        static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                        updateW2S(niCam);
                    }
                }
            }
        }
    }

    void ShowPlayerInMenusController::ForceRestoreToVanilla()
    {
        if (!active_) return;
        DoRestoreToVanilla();
    }

    void ShowPlayerInMenusController::DoRestoreToVanilla()
    {
        TweenCameraTrace::Sample("restore-enter", true);
        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!cam || !ply) {
            // Still release the TDM claims — bailing with yaw control or
            // the directional-movement disable held leaks them with
            // active_ false, and a leaked yaw claim stops TDM moving the
            // player entirely (ProcessInput early-returns while a plugin
            // owns yaw).
            auto& tdmBail = TDMIntegration::GetSingleton();
            if (tdmBail.HasYawControl()) tdmBail.ReleaseYawControl();
            if (tdmBail.HasDirectionalMovementDisabled())
                tdmBail.ReleaseDisableDirectionalMovement();
            active_ = false;
            activeMenuName_.clear();
            return;
        }

        // Restore TPS fields and worldFOV only if framing was actually
        // applied — in camera-control-only mode none of these were
        // touched, and the user may have orbited the camera (changing
        // freeRotation) or walked (changing player angle.z). Snapping
        // them back would undo the user's orientation.
        if (framingApplied_ || wroteVanillaPose_) {
            // Restore into the SAME instance the save read from (latched at
            // activation) — restoring a mount's saved values into
            // kThirdPerson is precisely how the mount camera got stranded.
            auto* tps = skyrim_cast<RE::ThirdPersonState*>(
                cam->cameraStates[activeStateId_].get());
            if (tps) {
                tps->freeRotationEnabled = savedFreeRotationEnabled_;
                tps->toggleAnimCam       = savedToggleAnimCam_;
                tps->freeRotation.x      = savedFreeRotationX_;
                tps->freeRotation.y      = savedFreeRotationY_;
                tps->posOffsetExpected.x = savedPosOffsetExpectedX_;
                tps->posOffsetExpected.y = savedPosOffsetExpectedY_;
                tps->posOffsetExpected.z = savedPosOffsetExpectedZ_;
                tps->posOffsetActual.x   = savedPosOffsetActualX_;
                tps->posOffsetActual.y   = savedPosOffsetActualY_;
                tps->posOffsetActual.z   = savedPosOffsetActualZ_;
            }
            ply->data.angle.z = savedPlayerAngleZ_;
            ply->data.angle.x = savedPlayerAngleX_;
            cam->worldFOV     = savedWorldFOV_;
        }

        if ((framingApplied_ || wroteVanillaPose_) && savedCamOffsetValid_ && cam->cameraRoot) {
            TweenCameraTrace::Sample("restore-position-before", true);
            const RE::NiPoint3 pp = ply->GetPosition();
            const RE::NiPoint3 restored{
                pp.x + savedCamOffsetFromPlayer_.x,
                pp.y + savedCamOffsetFromPlayer_.y,
                pp.z + savedCamOffsetFromPlayer_.z };
            const RE::NiPoint3 before = cam->cameraRoot->world.translate;
            cam->cameraRoot->world.translate = restored;
            cam->cameraRoot->local.translate = restored;
            if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                    ni->world.translate = restored;
                }
            }
            // Into the SAME instance the save read from, for the reason the
            // field restores above give: a mount's state is not kThirdPerson.
            if (auto* tpsPos = skyrim_cast<RE::ThirdPersonState*>(
                    cam->cameraStates[activeStateId_].get())) {
                tpsPos->translation = restored;
            }
            TweenCameraTrace::Sample("restore-position-after", true);
            const float dx = restored.x - before.x, dy = restored.y - before.y;
            spdlog::info("[ShowPlayerInMenus] camera position restored on close: "
                         "moved ({:.2f},{:.2f}) = {:.2f}u | framingApplied={} vanillaPose={}",
                         dx, dy, std::sqrt(dx * dx + dy * dy),
                         framingApplied_ ? 1 : 0, wroteVanillaPose_ ? 1 : 0);
        }

        // Release TDM control so target lock and directional
        // movement resume naturally after the menu closes. Order
        // matters: release directional movement LAST so TDM
        // reactivates with our final yaw still in place rather
        // than the engine's mid-transition value.
        auto& tdm = TDMIntegration::GetSingleton();
        if (tdm.HasYawControl()) {
            tdm.ReleaseYawControl();
        }
        if (tdm.HasDirectionalMovementDisabled()) {
            tdm.ReleaseDisableDirectionalMovement();
        }

        // If we forced into 3p on open, flip back to 1p on close
        // so the player's POV preference is respected. Set a one-
        // shot hint so any same-tick re-activation (Magic→Inventory
        // swap dropping force3p) treats the player as 1p — the
        // engine's per-frame POV tracker hasn't seen the
        // ForceFirstPerson yet by the time the next ActivateForMenu
        // runs.
        if (forcedThirdPerson_) {
            cam->ForceFirstPerson();
            s_pendingForce1pHint = true;
        }

        spdlog::info("[ShowPlayerInMenus] deactivated for {}", activeMenuName_);
        active_            = false;
        activeMenuName_.clear();
        forcedThirdPerson_ = false;
        framingApplied_    = false;
        wroteVanillaPose_  = false;
        savedCamOffsetValid_ = false;
        lockAngleZ_        = 0.0f;
        TweenCameraTrace::Sample("restore-exit", true);
    }

    void ShowPlayerInMenusController::ApplyFraming(RE::PlayerCamera*     a_camera,
                                                   RE::ThirdPersonState* a_tps,
                                                   RE::PlayerCharacter*  a_player)
    {
        if (!active_ || !a_camera || !a_tps || !a_player) return;

        // An item is zoomed in for inspection — hands off entirely.
        //
        // While the 3D preview is up the player is turning the ITEM, with the
        // same stick this framing is driven by. Every write below (freeRotation,
        // posOffset, worldFOV, the body-yaw pin) fought that: the view swung
        // around behind the menu and the item refused to rotate. The preview
        // owns the camera for as long as it is zoomed; we resume the moment it
        // closes, and because none of these writes are eased there is nothing
        // to hand back — the next frame simply re-asserts the framing.
        if (UnpauseManager::IsInventoryItemZoomed()) return;

        const auto* entry = EntryFor(activeMenuName_);
        if (!entry) return;

        // DIAG-favorites: rate-limited log to verify per-frame ticking
        // during FavoritesMenu (kCustomRendering may bypass the
        // ThirdPersonState::Update hook).
        if (activeMenuName_ == "FavoritesMenu") {
            static int sFavTickCount = 0;
            if ((sFavTickCount++ % 60) == 0) {
                spdlog::debug("[DIAG-favorites] ApplyFraming tick #{} y={} yaw={} fov={}",
                             sFavTickCount, entry->offsetY, entry->yaw, entry->fov);
            }
        }
        // Camera-control-only mode (allowCameraControl on, enabled off):
        // controller activates to force kThirdPerson so look input orbits
        // the camera, but we don't lock the framing. Skip every write
        // below so the camera stays at its natural over-shoulder position
        // and freeRotation responds to user input.
        //
        // EXCEPT: when we forced 3p from 1p and the user didn't enable
        // customization, we still need to write TPS posOffset / freeRot
        // values so the engine's post-construction recompose lands at
        // a vanilla over-the-shoulder pose. Without these writes, the
        // recompose sees pre-menu 1p state and the matrix override
        // from ApplyRenderMatrix gets stomped.
        if (!FramingOn(entry) && !forcedThirdPerson_) return;

        // useVanillaPose: ONLY write fixed vanilla-3p values when the
        // user doesn't want camera control. If allowCameraControl is
        // on, this menu wants gamepad/mouse to orbit the camera —
        // hammering freeRotation/data.angle/posOffset every frame
        // would override the engine's stick→camera mapping and
        // produce the inverted/zoom-instead-of-tilt behavior the
        // user reported. Return early in that case; the initial
        // POV switch at activation already put the camera in 3p
        // and that's all force3p needed to do.
        const bool useVanillaPose = !FramingOn(entry) && forcedThirdPerson_;
        if (useVanillaPose && entry->allowCameraControl) return;

        const float effYaw     = useVanillaPose ? 0.0f    : entry->yaw;
        // Stored offsets are CAMERA-relative (positive Y = camera further
        // away), matching CameraProfile's Zoom. The math below is
        // subject-relative, so negate on the way in — see the note on
        // ShowPlayerInMenuEntry::offsetY.
        const float effOffsetY = -(useVanillaPose ? 150.0f : entry->offsetY);
        const float effFov     = useVanillaPose ? a_camera->worldFOV : entry->fov;

        // FOV override — the engine and our profile system both write
        // worldFOV based on weapon-state, which causes the menu camera
        // to zoom in/out as the player swaps gear. Pin worldFOV to the
        // user-chosen menu FOV every frame so it stays put. Save value
        // is restored on close. (Vanilla pose leaves FOV alone.)
        a_camera->worldFOV = effFov;

        // Pin player body yaw so the framing rotation is deterministic
        // and TDM target-lock can't drift the body. The TDM yaw control
        // call in OnMenuOpenChange yields ownership to us, but TDM has
        // multiple write paths and other code (engine POV transitions,
        // dialogue camera) may also touch angle.z — re-asserting each
        // frame is the only way to guarantee the body stays where we
        // pinned it on activation.
        //
        // NOT WHILE MOUNTED. The rider's yaw belongs to the mount — the
        // player turns because the horse or dragon turns — so pinning
        // angle.z would fight the animation every frame, and angle.x = 0
        // would flatten a rider whose pitch the flight sets.
        if (!a_player->IsOnMount()) {
            a_player->data.angle.z = lockAngleZ_;
            a_player->data.angle.x = 0.0f;
            if (TDMIntegration::GetSingleton().HasYawControl()) {
                TDMIntegration::GetSingleton().SetPlayerYaw(lockAngleZ_);
            }
        }

        // freeRotation.x rotates the camera around the player's vertical
        // axis (yaw offset from the player's facing direction). Setting
        // it to π puts the camera directly in front of the player
        // (looking at their face). The framing offsets are then applied
        // on top of that orientation.
        //
        // CAMERA CONTROL while framed (2026-08-15): the engine's look input
        // writes freeRotation.x between our frames; overwriting it flat
        // discarded that input ("isn't letting me move the camera"). Fold
        // the delta since our LAST write into a persistent user orbit
        // offset instead, so the stick/mouse orbits the framing.
        if (entry->allowCameraControl && FramingOn(entry) && frWriteValid_) {
            float d = a_tps->freeRotation.x - lastWrittenFrX_;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            if (std::abs(d) < 1.0f) userOrbitYaw_ += d;   // ignore snaps
        }
        a_tps->freeRotationEnabled = true;
        a_tps->freeRotation.x      = effYaw + userOrbitYaw_;
        lastWrittenFrX_ = a_tps->freeRotation.x;
        frWriteValid_   = true;
        // freeRotation.y is mouse-pitch offset; zero it so the camera
        // stays level on the player. Pitch tilt isn't user-configurable
        // for this feature.
        a_tps->freeRotation.y      = 0.0f;

        // toggleAnimCam unshackles the over-shoulder cam when a weapon
        // is drawn — without it, posOffsetExpected/Actual writes are
        // partially ignored in the combat-ready pose.
        a_tps->toggleAnimCam = true;

        // Write BOTH posOffsetExpected and posOffsetActual to skip the
        // engine's interpolation. Paused-menu camera ticks may not run
        // long enough for the lerp to converge; matching them up-front
        // makes the framing apply instantly.
        //
        // Only the distance (offsetY) is written through the TPS path.
        // Sideways (offsetX) and Height (offsetZ) are applied as a
        // camera-space translation in ApplyRenderMatrix so they shift
        // the player on screen instead of orbiting. Writing them to
        // posOffset.x/z here would cause the engine to look-at-player
        // from the shifted position (orbit behavior) on any frame
        // where ApplyRenderMatrix doesn't reach the NiCamera.
        a_tps->posOffsetExpected.x = 0.0f;
        a_tps->posOffsetExpected.y = effOffsetY;
        a_tps->posOffsetExpected.z = 0.0f;
        a_tps->posOffsetActual.x   = 0.0f;
        a_tps->posOffsetActual.y   = effOffsetY;
        a_tps->posOffsetActual.z   = 0.0f;

    }

    bool ShowPlayerInMenusController::ApplyRenderMatrix(RE::NiCamera*        a_niCamera,
                                                        RE::PlayerCharacter* a_player)
    {
        if (!active_ || !a_niCamera || !a_player) return false;

        const auto* entry = EntryFor(activeMenuName_);
        if (!entry) return false;
        // Same as ApplyFraming: in camera-control-only mode the user
        // owns the camera. Don't override the NiCamera matrix —
        // UNLESS we forced 3p from 1p and the user didn't enable
        // customization. In that case we DO need to position the
        // camera (otherwise the rendered NiCamera stays at the
        // pre-menu 1p location and the user sees "no change" even
        // though state.id is kThirdPerson). Synthesize vanilla
        // over-the-shoulder values for that path.
        if (!FramingOn(entry) && !forcedThirdPerson_) return false;
        // Same allowCameraControl carve-out as ApplyFraming: if the
        // user wants to orbit, don't slam the NiCamera matrix every
        // frame — engine's stick→camera mapping owns it.
        if (!FramingOn(entry) && forcedThirdPerson_ && entry->allowCameraControl) {
            return false;
        }

        // Compose camera world transform.
        //
        // Two-step build so Sideways / Height TRANSLATE the player on
        // screen instead of orbiting around them:
        //   1. Base camera position uses ONLY the distance (offsetY) and
        //      yaw. The forward direction is locked to "look at the
        //      anchor from this base position" — this is the framing
        //      the player would see if Sideways/Height were both zero.
        //   2. Then translate the camera perpendicular to that fixed
        //      forward direction: offsetX shifts along camera-right,
        //      offsetZ shifts along camera-up. Forward stays fixed, so
        //      the player drifts off-center on screen rather than the
        //      camera pivoting around them.
        // Sign convention: positive Sideways moves the player RIGHT on
        // screen (camera shifts LEFT in world). Positive Height moves the
        // player UP on screen (camera shifts DOWN in world). Hence the
        // SUBTRACTION when applying ox/oz to camPos.
        //
        // Effective values: entry's when enabled; synthesized vanilla
        // over-the-shoulder when not (forced-3p-only mode).
        const bool useVanillaPose = !FramingOn(entry) && forcedThirdPerson_;
        // CAMERA-relative in, subject-relative out. Positive stored X moves
        // the CAMERA right, positive Z moves the CAMERA up, positive Y moves
        // the CAMERA further away — the same reading as Side Offset / Height /
        // Zoom on every other page. The step-2 maths below subtracts ox and oz,
        // so a single negation here turns each of them the right way round.
        const float effYaw     = useVanillaPose ? 0.0f    : entry->yaw;
        const float effOffsetX = -(useVanillaPose ? 30.0f  : entry->offsetX);
        const float effOffsetY = -(useVanillaPose ? 150.0f : entry->offsetY);
        const float effOffsetZ = -(useVanillaPose ? 0.0f   : entry->offsetZ);

        // userOrbitYaw_ = the accumulated look input while framed (see
        // ApplyFraming), so the rendered matrix follows the same orbit.
        const float yaw = lockAngleZ_ + effYaw + (FramingOn(entry) ? userOrbitYaw_ : 0.0f);
        const float c   = std::cos(yaw);
        const float sn  = std::sin(yaw);

        const RE::NiPoint3 anchor{
            a_player->GetPosition().x,
            a_player->GetPosition().y,
            a_player->GetPosition().z + 100.0f
        };
        const float ox = effOffsetX;
        const float oy = effOffsetY;
        const float oz = effOffsetZ;

        // Step 1: base position from yaw + distance only.
        const RE::NiPoint3 basePos{
            anchor.x + sn * oy,
            anchor.y + c  * oy,
            anchor.z
        };

        // Forward = base camera looking at anchor. Stays fixed for
        // step 2 so sideways/height don't change orientation.
        RE::NiPoint3 fwd{ anchor.x - basePos.x, anchor.y - basePos.y, anchor.z - basePos.z };
        const float fwdLen = std::sqrt(fwd.x*fwd.x + fwd.y*fwd.y + fwd.z*fwd.z);
        if (fwdLen < 0.001f) return false;
        fwd.x /= fwdLen; fwd.y /= fwdLen; fwd.z /= fwdLen;

        const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
        RE::NiPoint3 right{
            fwd.y * worldUp.z - fwd.z * worldUp.y,
            fwd.z * worldUp.x - fwd.x * worldUp.z,
            fwd.x * worldUp.y - fwd.y * worldUp.x
        };
        const float rLen = std::sqrt(right.x*right.x + right.y*right.y + right.z*right.z);
        if (rLen < 0.001f) return false;
        right.x /= rLen; right.y /= rLen; right.z /= rLen;
        const RE::NiPoint3 up{
            right.y * fwd.z - right.z * fwd.y,
            right.z * fwd.x - right.x * fwd.z,
            right.x * fwd.y - right.y * fwd.x
        };

        // Step 2: translate camera perpendicular to forward. Subtract
        // ox*right so positive Sideways → player RIGHT on screen.
        // Subtract oz*up so positive Height → player UP on screen.
        const RE::NiPoint3 camPos{
            basePos.x - ox * right.x - oz * up.x,
            basePos.y - ox * right.y - oz * up.y,
            basePos.z - ox * right.z - oz * up.z
        };

        // TEMPORARY [SPIMSKY] — "moving with a custom angle enabled for
        // menus causes the sky to move" (user report 2026-08-15). Two
        // suspects, one probe:
        //  (a) some rendered frames carry the ENGINE's rotation instead of
        //      ours — the pre-write yaw deviates from the constant framing
        //      yaw (alternating-source rendering = the sky swings),
        //  (b) the sky/imagespace/audio systems key off cameraRoot, which
        //      this CHILD-NiCamera override leaves at the engine's pose —
        //      the root-vs-written position delta grows while walking and
        //      the camera-centred sky dome drifts against the view.
        // Rate-limited + capped; strip once the next log answers it.
        {
            static int  sSkyLogs = 0;
            static auto sSkyLast = std::chrono::steady_clock::now();
            const auto  nowSky   = std::chrono::steady_clock::now();
            if (sSkyLogs < 60 &&
                std::chrono::duration<float>(nowSky - sSkyLast).count() > 0.25f) {
                sSkyLast = nowSky;
                const float preYaw = std::atan2(a_niCamera->world.rotate.entry[0][0],
                                                a_niCamera->world.rotate.entry[1][0]);
                const float expYaw = std::atan2(fwd.x, fwd.y);
                RE::NiPoint3 rootPos{};
                if (auto* pcSky = RE::PlayerCamera::GetSingleton(); pcSky && pcSky->cameraRoot)
                    rootPos = pcSky->cameraRoot->world.translate;
                float speed = 0.0f;
                {
                    static RE::NiPoint3 sPrevPly{};
                    const RE::NiPoint3 pp = a_player->GetPosition();
                    speed = std::sqrt((pp.x - sPrevPly.x) * (pp.x - sPrevPly.x) +
                                      (pp.y - sPrevPly.y) * (pp.y - sPrevPly.y));
                    sPrevPly = pp;
                }
                // Only interesting while the framing is being fought or the
                // player is actually moving.
                if (speed > 0.5f || std::abs(preYaw - expYaw) > 0.005f) {
                    ++sSkyLogs;
                    spdlog::debug("[SPIMSKY] preYaw={:.4f} expYaw={:.4f} dYaw={:.4f} "
                                 "rootDelta=({:.1f},{:.1f},{:.1f}) plyMoved={:.1f}",
                                 preYaw, expYaw, preYaw - expYaw,
                                 rootPos.x - camPos.x, rootPos.y - camPos.y,
                                 rootPos.z - camPos.z, speed);
                }
            }
        }

        // Skyrim NiCamera convention: world.rotate columns are
        // (forward, up, right). Write directly to the NiCamera's
        // world transform. Don't call UpdateDownwardPass on the
        // cameraRoot from inside this hook — UpdateDownwardPass
        // traverses children invoking UpdateWorldData, which would
        // re-enter THIS hook on the NiCamera we're currently inside
        // an UpdateWorldData of. Recursive re-entry crashed the game
        // when opening Container.
        a_niCamera->world.rotate.entry[0][0] = fwd.x;
        a_niCamera->world.rotate.entry[1][0] = fwd.y;
        a_niCamera->world.rotate.entry[2][0] = fwd.z;
        a_niCamera->world.rotate.entry[0][1] = up.x;
        a_niCamera->world.rotate.entry[1][1] = up.y;
        a_niCamera->world.rotate.entry[2][1] = up.z;
        a_niCamera->world.rotate.entry[0][2] = right.x;
        a_niCamera->world.rotate.entry[1][2] = right.y;
        a_niCamera->world.rotate.entry[2][2] = right.z;
        a_niCamera->world.translate          = camPos;

        // Pin the cameraRoot to the SAME position. [SPIMSKY]
        // (2026-08-15 17:53-18:00 log) measured the root sitting ~295
        // units from the rendered camera — the engine's own pose — and
        // the sky dome / audio listener / imagespace all key off the
        // root, so while the player MOVED the sky slid by the difference
        // ("moving with a custom angle makes the sky move"). Same
        // renderer/derivation-lockstep rule the stair-smoothing and
        // Looseness writes follow. Rotation is left alone — the sky
        // centres on position, and the child NiCamera above carries the
        // rendered orientation.
        if (auto* pcRoot = RE::PlayerCamera::GetSingleton();
            pcRoot && pcRoot->cameraRoot) {
            pcRoot->cameraRoot->world.translate = camPos;
            pcRoot->cameraRoot->local.translate = camPos;
        }
        return true;
    }
}
