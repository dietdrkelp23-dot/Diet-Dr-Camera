#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include "Unpause/UnpauseManager.h"

#include "Camera/StateResolver.h"
#include "Hooks/HookManager.h"
#include "Menus/ShowPlayerInMenusController.h"
#include "Settings/SettingsManager.h"

#include <RE/A/ActivateHandler.h>
#include <RE/A/AIProcess.h>
#include <RE/A/AttackBlockHandler.h>
#include <RE/A/AutoMoveHandler.h>
#include <RE/B/BarterMenu.h>
#include <RE/C/ContainerMenu.h>
#include <RE/D/DialogueMenu.h>
#include <RE/F/FavoritesHandler.h>
#include <RE/F/FavoritesMenu.h>
#include <RE/I/Inventory3DManager.h>
#include <RE/I/InventoryMenu.h>
#include <RE/M/MagicMenu.h>
#include <RE/T/TweenMenu.h>
#include <RE/H/HighProcessData.h>
#include <RE/J/JumpHandler.h>
#include <RE/L/LookHandler.h>
#include <RE/M/MenuCursor.h>
#include <RE/M/MenuOpenHandler.h>
#include <RE/M/MovementHandler.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/R/ReadyWeaponHandler.h>
#include <RE/R/RunHandler.h>
#include <RE/S/ScrapHeap.h>
#include <RE/S/ShoutHandler.h>
#include <RE/S/SneakHandler.h>
#include <RE/S/SprintHandler.h>
#include <RE/T/TogglePOVHandler.h>
#include <RE/T/ToggleRunHandler.h>
#include <RE/U/UI.h>
#include <RE/U/UserEvents.h>

#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <chrono>
#include <map>
#include <string>

namespace DietDrCamera::UnpauseManager
{
    namespace
    {
        // Saved engine Creator function pointer per hijacked menu.
        std::map<std::string, RE::UI::Create_t*> sOriginalCreators;
        bool sInstalled = false;

        // --------- DIAG: stutter timing ---------
        // Wall-clock instrumentation to attribute the SkyUI select-button
        // swap hitch. Logs entry/exit deltas for CreateMenuShared and
        // logs frame durations >5ms from the main-thread tick. Remove
        // once the source is identified.
        using diag_clock = std::chrono::steady_clock;
        std::chrono::time_point<diag_clock> sLastFrameTick{};

        inline double DiagMicros(std::chrono::time_point<diag_clock> a_t0)
        {
            return std::chrono::duration<double, std::micro>(diag_clock::now() - a_t0).count();
        }

        // Per-menu Creator hijack: call the engine's stock creator,
        // then mutate the resulting IMenu's flags to clear pause +
        // mark our custom kUnpaused bit so we can count later. This
        // runs INSIDE the engine's UI::ProcessMessage(kShow) path,
        // BEFORE the engine reads PausesGame() to increment the
        // counter — so neither increment nor decrement ever happens.
        // Pattern from SkyrimSoulsRE/src/SkyrimSoulsRE.cpp::CreateMenu.
        RE::IMenu* CreateMenuShared(std::string_view a_menuName)
        {
            const auto diagT0 = diag_clock::now();
            auto it = sOriginalCreators.find(std::string{ a_menuName });
            if (it == sOriginalCreators.end()) {
                spdlog::error("[Unpause] Creator hijack invoked for {} but no original saved",
                              std::string{ a_menuName }.c_str());
                return nullptr;
            }
            const auto diagBeforeEngine = diag_clock::now();
            RE::IMenu* menu = it->second();
            const double engineMicros = DiagMicros(diagBeforeEngine);
            if (!menu) return nullptr;

            // Resolve per-menu unpause toggle from settings.
            const auto& s = SettingsManager::GetSingleton();
            bool isUnpaused = false;
            if      (a_menuName == "BarterMenu")      isUnpaused = s.showPlayerInBarter.unpauseGame;
            else if (a_menuName == "InventoryMenu")   isUnpaused = s.showPlayerInInventory.unpauseGame;
            else if (a_menuName == "ContainerMenu")   isUnpaused = s.showPlayerInContainer.unpauseGame;
            else if (a_menuName == "MagicMenu")       isUnpaused = s.showPlayerInMagic.unpauseGame;
            else if (a_menuName == "TweenMenu")       isUnpaused = s.showPlayerInTween.unpauseGame;
            else if (a_menuName == "FavoritesMenu")   isUnpaused = s.showPlayerInFavorites.unpauseGame;
            // (A "keep the pause while mounted" gate lived here for one build
            // on 2026-08-23, as a way to hide a camera parked in kTween. The
            // user's verdict was that the frozen state read worse than the
            // camera being left behind, and ShowPlayerInMenus now handles
            // mounts properly anyway, so mounted menus unpause like any other.)

            // Sleep/Wait Menu intentionally NOT hijacked — the engine's
            // wait mechanism advances time only while paused; running it
            // against a live simulation causes time to fast-forward and
            // the render thread to crash on animation desync. Porting
            // SkyrimSouls's SleepWaitMenuEx (OK-button callback wrap +
            // CanSleep hook + re-pause before StartSleepWait) is the
            // proper path; deferred.

            // EVERY mutation below is gated on isUnpaused. With the toggle
            // off this creator must be TRANSPARENT — same vanilla-identical
            // contract as every other unconfigured surface. It wasn't
            // (2026-08-31, "stutter forward if walking while opening it in
            // first person"): InterruptCast(true) fired on the player on
            // every PAUSED open too — a graph interrupt on the exact open
            // frame — and kRequiresUpdate was forced onto paused menus,
            // ticking menu-mod code (Tween Menu Overhaul's zoom) through a
            // pause where vanilla wouldn't.
            if (menu->PausesGame() && isUnpaused) {
                menu->menuFlags.reset(RE::IMenu::Flag::kPausesGame);
                menu->menuFlags.set(static_cast<RE::IMenu::Flag>(kUnpausedMenuFlagBit));
                // The engine relied on the pause itself to interrupt the
                // player's spell cast on menu open. Restore that behavior
                // for the unpaused menu; a PAUSED menu still gets it from
                // the pause, the vanilla way.
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    pc->InterruptCast(true);
                }
            }
            if (isUnpaused) {
                // Force per-frame updates. Vanilla pause-everything menus
                // sometimes skip kUpdate; with the world running we need
                // them to keep ticking.
                if (!menu->RequiresUpdate()) {
                    menu->menuFlags.set(RE::IMenu::Flag::kRequiresUpdate);
                }
                // Don't freeze-blur the world background while unpaused.
                if (menu->FreezeFrameBackground()) {
                    menu->menuFlags.reset(RE::IMenu::Flag::kFreezeFrameBackground);
                }
                // Edge-clamp the cursor away from screen borders so the
                // menu opening doesn't kick off a screen-edge camera pan
                // (only possible while the world keeps running).
                if (auto* mc = RE::MenuCursor::GetSingleton()) {
                    mc->cursorPosX = std::clamp(mc->cursorPosX, 10.0f, mc->screenWidthX - 10.0f);
                    mc->cursorPosY = std::clamp(mc->cursorPosY, 10.0f, mc->screenWidthY - 10.0f);
                }
            }

            // Break the depthPriority tie with DialogueMenu. Vanilla
            // BarterMenu's depthPriority is 0 (per CommonLibSSE comment)
            // and our DialogueMenu AdvanceMovie hook drops dialogue to 0.
            // Both at 0 leaves the engine routing input to dialogue.
            // Bumping inventory-item menus to 1 matches SkyrimSouls's
            // pattern (CreateMenu in their SkyrimSoulsRE.cpp).
            if (isUnpaused && menu->InventoryItemMenu()) {
                menu->depthPriority = 1;
            }
            const double totalMicros = DiagMicros(diagT0);
            spdlog::debug("[DIAG-stutter] CreateMenuShared({}) total={:.0f}us engine={:.0f}us",
                         std::string{ a_menuName }.c_str(), totalMicros, engineMicros);
            return menu;
        }

        // Per-menu thunk so Skyrim's stored Create_t function pointer
        // matches the expected signature. One thunk per menu we hijack.
        RE::IMenu* CreateBarterMenu()    { return CreateMenuShared(RE::BarterMenu::MENU_NAME); }
        RE::IMenu* CreateInventoryMenu() { return CreateMenuShared(RE::InventoryMenu::MENU_NAME); }
        RE::IMenu* CreateContainerMenu() { return CreateMenuShared(RE::ContainerMenu::MENU_NAME); }
        RE::IMenu* CreateMagicMenu()     { return CreateMenuShared(RE::MagicMenu::MENU_NAME); }
        RE::IMenu* CreateTweenMenu()     { return CreateMenuShared(RE::TweenMenu::MENU_NAME); }
        RE::IMenu* CreateFavoritesMenu() { return CreateMenuShared(RE::FavoritesMenu::MENU_NAME); }

        // ----- Main-thread UI tick driver -----
        // Vanilla Skyrim runs UI::ProcessMessages + UI::AdvanceMovies
        // from a job that only fires while the game is paused. With our
        // unpaused menus, the world keeps ticking but the menu stops
        // receiving message/movie updates → frozen UI. We hook the
        // displaced ScrapHeap function called from Main::Update + 0xADF,
        // and after the original runs, manually drive both UI methods
        // when GameIsPaused() is false. Plus an explicit
        // ExecuteConsoleCommands tick because that path is normally
        // gated on the same UI job.
        // Use free-function pointer types with explicit `this` as the
        // first argument. CommonLibSSE-NG's REL::Relocation handling of
        // member function pointer types is fragile (MSVC member fn ptrs
        // aren't simple addresses), and our previous build crashed
        // inside UI::ProcessMessages because the `this` pointer wasn't
        // bound correctly. Free-fn style passes `ui` in RCX directly,
        // matching x64 thiscall.
        using ProcessMessages_t = void (*)(RE::UI*);
        using AdvanceMovies_t   = void (*)(RE::UI*);
        using GetExecConsole_t  = void* (*)();
        using ExecConsole_t     = void (*)(void*);

        REL::Relocation<ProcessMessages_t> sProcessMessages;
        REL::Relocation<AdvanceMovies_t>   sAdvanceMovies;
        REL::Relocation<GetExecConsole_t>  sGetExecConsoleSingleton;
        REL::Relocation<ExecConsole_t>     sExecConsole;

        REL::Relocation<void (*)(RE::ScrapHeap*)> sMainThreadOriginal;

        void MainThreadHook(RE::ScrapHeap* a_this)
        {
            sMainThreadOriginal(a_this);

            // DO NOT REMOVE THIS DRIVE. It was deleted for one build on
            // 2026-09-01 on the deduction that the Job::UI+0xB patch left
            // the vanilla UI job running unconditionally — WRONG: without
            // this drive the screen stays BLACK in game (the fader never
            // pumps; user hit it immediately). This is the ONLY UI driver
            // while the game is unpaused. The unpaused-menu double-pump
            // measured by [TWEENSTUT] must be solved some other way.
            auto* ui = RE::UI::GetSingleton();
            if (ui && !ui->GameIsPaused()) {
                // Flagged for the camera hooks. With a menu open this drive
                // produces a SECOND camera pass in the same rendered frame,
                // and a camera that integrates twice per step of movement
                // oscillates ([TWEENCAM] distRoot 17.18 / 9.22 alternating).
                // The flag is the causal test for "this pass is the extra
                // one" — see HookManager::SetUiDriveActive.
                HookManager::SetUiDriveActive(true);
                sProcessMessages(ui);
                sAdvanceMovies(ui);
                if (void* cs = sGetExecConsoleSingleton()) {
                    sExecConsole(cs);
                }
                HookManager::SetUiDriveActive(false);
            }

            // Drive the Show-Player-In-Menus framing every frame,
            // paused or not. Required for FavoritesMenu (kCustomRendering
            // bypasses ThirdPersonState::Update) and any other menu
            // where the engine suspends camera-state ticks. Idempotent
            // with HookedThirdPersonUpdate's per-frame spim block.
            ShowPlayerInMenusController::GetSingleton().DrivePerFrameFallback();

            // DIAG-stutter: log inter-tick gaps that indicate a real
            // hitch, not a normal frame. Threshold is 50ms so it stays
            // silent at every common framerate (30/60/144 FPS) and only
            // fires on a meaningful frame skip. The previous 20ms
            // threshold fired on every frame at 30 FPS, flooding the
            // log and adding synchronous disk I/O to the hot path.
            const auto now = diag_clock::now();
            if (sLastFrameTick.time_since_epoch().count() != 0) {
                const double deltaMs =
                    std::chrono::duration<double, std::milli>(now - sLastFrameTick).count();
                if (deltaMs > 50.0) {
                    spdlog::debug("[DIAG-stutter] long tick {:.1f}ms (unpaused={})",
                                 deltaMs, GetUnpausedMenuCount());
                }
            }
            sLastFrameTick = now;
        }

        void InstallMainThreadHook()
        {
            const auto& sites = RuntimeHooks::Get();
            sProcessMessages = sites.processMessages;
            sAdvanceMovies = sites.advanceMovies;
            sGetExecConsoleSingleton = sites.getConsole;
            sExecConsole = sites.executeConsole;
            RuntimeHooks::RequireCall(sites.mainUpdate);
            auto& trampoline = SKSE::GetTrampoline();
            sMainThreadOriginal = trampoline.write_call<5>(sites.mainUpdate, &MainThreadHook);
            RuntimeHooks::DisableUIJob();
            spdlog::info("[Unpause] Main-thread UI drive installed; displaced function=0x{:X}", sMainThreadOriginal.address());
        }

        // Phase 2: input handler vtable patching. SkyrimSouls's
        // canonical pattern. For each gameplay PlayerInputHandler we
        // care about, patch its CanProcess virtual (slot 1) so that
        // when GetUnpausedMenuCount() > 0 the handler returns false
        // and never gets to fire its action. Movement and gamepad-look
        // get carve-outs so the player can still walk and look.
        //
        // Free-function pointer style (T*, RE::InputEvent*) matches
        // x64 thiscall — `this` in RCX, event in RDX. Same convention
        // as the engine's vtable entry.
        // (IsItemPreviewMenuOpen / IsInventoryItemZoomed are declared in the
        // header and defined further down at namespace scope — HookManager
        // needs them too.)
        template <class T>
        struct InputHandlerHook
        {
            using CanProcess_t = bool (*)(T*, RE::InputEvent*);
            static inline REL::Relocation<CanProcess_t> _orig;

            static bool CanProcess_Hook(T* a_this, RE::InputEvent* a_event)
            {
                // NOTHING form-specific belongs in this function.
                //
                // Werewolf's "no first person" rule was briefly enforced here by
                // returning false for TogglePOVHandler. That broke target lock
                // in beast form, because True Directional Movement implements
                // lock-on by hooking THIS handler — refusing the handler refuses
                // the lock along with the POV change. The rule lives in
                // HookManager::HookedTogglePOVProcessButton instead, which can
                // still pass the button down the chain to TDM while skipping
                // DDC's own POV forcing. (Vanilla already refuses 1p in beast
                // form on its own; DDC's held-view toggle was the only thing
                // overriding that.)
                // An item-preview menu is up: the POV button belongs to the
                // menu there, full stop. Blocked whether or not that menu is
                // UNPAUSED — the gate below only fires for unpaused menus, so a
                // player who left the inventory paused still had R3 reach the
                // POV machinery and recentre the camera behind the character
                // while they were only trying to inspect a sword.
                if constexpr (std::is_same_v<T, RE::TogglePOVHandler>) {
                    if (IsItemPreviewMenuOpen()) return false;
                }
                // Edge-logged (2026-09-05): this is the one place DDC refuses
                // the player's movement input outright. If the refusal ever
                // outlives its menu, the log has to say so in so many words.
                if constexpr (std::is_same_v<T, RE::MovementHandler>) {
                    const bool blockNow = GetUnpausedMenuCount() > 0 && ShouldBlockPlayerMovement();
                    static bool sMoveWasBlocked = false;
                    if (blockNow != sMoveWasBlocked) {
                        sMoveWasBlocked = blockNow;
                        spdlog::info("[Unpause] player movement {} (unpaused menus on the stack: {})",
                                     blockNow ? "REFUSED by an unpaused menu with allowMovement=false"
                                              : "released",
                                     GetUnpausedMenuCount());
                    }
                }
                if (GetUnpausedMenuCount() > 0) {
                    // Movement: normally passes so the player can walk
                    // around with the menu open, EXCEPT for unpaused
                    // Barter where the dialogue-camera framing requires
                    // the player to stay put.
                    if constexpr (std::is_same_v<T, RE::MovementHandler>) {
                        if (ShouldBlockPlayerMovement()) return false;
                        return _orig(a_this, a_event);
                    }
                    // LookHandler: gamepad thumbstick always passes.
                    // Mouse events pass only when the active unpaused
                    // menu has allowCameraControl on (otherwise mouse
                    // would drive both the cursor AND the camera, which
                    // is jarring).
                    if constexpr (std::is_same_v<T, RE::LookHandler>) {
                        // ...unless an item is being INSPECTED. The zoomed 3D
                        // preview is rotated with the same stick, so letting the
                        // camera have it too meant the view swung around behind
                        // the menu while the item refused to turn. The camera
                        // gives up the stick for as long as the item has it.
                        if (IsInventoryItemZoomed()) return false;
                        if (a_event && a_event->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
                            return _orig(a_this, a_event);
                        }
                        if (IsAnyUnpausedMenuAllowingCameraControl()) {
                            return _orig(a_this, a_event);
                        }
                    }
                    // TogglePOVHandler stays BLOCKED here, deliberately.
                    //
                    // It was briefly let through while an item-preview menu was
                    // open, on the theory that the engine routes the 3D item
                    // zoom through it. Whether or not it does, True Directional
                    // Movement hooks this same handler for target lock — so
                    // letting the button through meant clicking R3 in the
                    // inventory ACQUIRED A LOCK on whatever was outside the
                    // menu. That is strictly worse than the thing it was trying
                    // to fix, and there is no way to tell the two consumers
                    // apart from here.
                    return false;
                }
                return _orig(a_this, a_event);
            }

            static void Install(REL::Relocation<std::uintptr_t>& vtbl, std::uint16_t slot = 1)
            {
                _orig = vtbl.write_vfunc(slot, &CanProcess_Hook);
            }
        };

        void InstallInputHandlerHooks()
        {
            // Each handler's vtable is at VTABLE_<Type>[0]. CanProcess
            // is virtual slot 1 for all PlayerInputHandler derivatives.
            #define DDC_PATCH(Type) do { \
                REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_##Type[0] }; \
                InputHandlerHook<RE::Type>::Install(vt, 1); \
            } while (0)

            DDC_PATCH(ActivateHandler);
            DDC_PATCH(AttackBlockHandler);
            DDC_PATCH(AutoMoveHandler);
            DDC_PATCH(JumpHandler);
            DDC_PATCH(LookHandler);
            DDC_PATCH(MovementHandler);
            DDC_PATCH(ReadyWeaponHandler);
            DDC_PATCH(RunHandler);
            DDC_PATCH(ShoutHandler);
            DDC_PATCH(SneakHandler);
            DDC_PATCH(SprintHandler);
            DDC_PATCH(TogglePOVHandler);
            DDC_PATCH(ToggleRunHandler);
            DDC_PATCH(FavoritesHandler);
            DDC_PATCH(MenuOpenHandler);

            #undef DDC_PATCH
            spdlog::info("[Unpause] 15 PlayerInputHandler vtables patched (CanProcess slot 1)");
        }

        // Phase 3: DialogueMenu coexistence. When BarterMenu opens on top
        // of an active DialogueMenu and we've unpaused, the dialogue movie
        // stays visible at depth 3 (same as barter) and steals input —
        // user sees a phantom dialogue box and can't click barter rows.
        // SkyrimSouls's fix is two hooks:
        //  1. AdvanceMovie (vtable slot 5): while another unpaused menu
        //     is open and game isn't paused, hide the dialogue movie and
        //     drop its depthPriority to 0. Restore on close.
        //  2. UpdateAutoCloseTimer call site (REL::ID 37541 + 0x6E8):
        //     pin the awarePlayerTimer at 120.0 so the goodbye-on-walk-
        //     away path doesn't fire while the player is buying/selling.
        //     Without this the dialogue would auto-close mid-trade once
        //     the player walked too far from the speaker.
        // Free-function pointer style (same RCX-this convention as the
        // InputHandlerHook). Avoids MSVC member-function-pointer fragility.
        using DialogueAdvanceMovie_t = void (*)(RE::DialogueMenu*, float, std::uint32_t);
        REL::Relocation<DialogueAdvanceMovie_t> sDialogueAdvanceMovieOrig;

        void DialogueMenu_AdvanceMovie_Hook(RE::DialogueMenu* a_this, float a_interval, std::uint32_t a_currentTime)
        {
            const std::uint32_t unpausedMenuCount = GetUnpausedMenuCount();
            auto* ui = RE::UI::GetSingleton();
            const bool gamePaused = ui ? ui->GameIsPaused() : true;
            const bool hasMovie = a_this->uiMovie != nullptr;

            if (!gamePaused && hasMovie) {
                const bool isVisible = a_this->uiMovie->GetVisible();
                if (isVisible && unpausedMenuCount > 0) {
                    a_this->uiMovie->SetVisible(false);
                    a_this->depthPriority = 0;
                } else if (!isVisible && unpausedMenuCount == 0) {
                    a_this->uiMovie->SetVisible(true);
                    a_this->depthPriority = 3;
                }
            }

            sDialogueAdvanceMovieOrig(a_this, a_interval, a_currentTime);
        }

        void DialogueMenu_UpdateAutoCloseTimer_Hook(RE::AIProcess* a_process, float a_delta)
        {
            if (!a_process) return;
            auto* highData = a_process->high;
            if (!highData) return;
            if (GetUnpausedMenuCount() > 0) {
                highData->awarePlayerTimer = 120.0f;
            } else {
                highData->awarePlayerTimer += a_delta;  // a_delta is negative
            }
        }

        void InstallDialogueMenuHooks()
        {
            REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_DialogueMenu[0] };
            sDialogueAdvanceMovieOrig = vt.write_vfunc(0x5, &DialogueMenu_AdvanceMovie_Hook);
            spdlog::info("[Unpause] DialogueMenu AdvanceMovie vtable hook installed (slot 5)");

            const auto callSite = RuntimeHooks::Get().dialogueTimer;
            RuntimeHooks::RequireCall(callSite);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
            spdlog::info("[Unpause] UpdateAutoCloseTimer pre-patch bytes at 0x{:x}: {:02x} {:02x} {:02x} {:02x} {:02x}",
                         callSite, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

            auto& trampoline = SKSE::GetTrampoline();
            trampoline.write_call<5>(callSite,
                                     reinterpret_cast<std::uintptr_t>(&DialogueMenu_UpdateAutoCloseTimer_Hook));
            spdlog::info("[Unpause] DialogueMenu UpdateAutoCloseTimer call site patched at 0x{:x}", callSite);
        }

        void HijackCreator(std::string_view menuName, RE::UI::Create_t* replacement)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                spdlog::error("[Unpause] UI singleton null at hijack time for {}",
                              std::string{ menuName }.c_str());
                return;
            }
            auto it = ui->menuMap.find(menuName);
            if (it == ui->menuMap.end()) {
                spdlog::warn("[Unpause] menuMap has no entry for {} (skipping hijack)",
                             std::string{ menuName }.c_str());
                return;
            }
            sOriginalCreators[std::string{ menuName }] = it->second.create;
            it->second.create                          = replacement;
            spdlog::info("[Unpause] Creator hijacked for {}",
                         std::string{ menuName }.c_str());
        }
    }

    std::uint32_t GetUnpausedMenuCount()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return 0;
        std::uint32_t count = 0;
        for (auto& slot : ui->menuStack) {
            auto* menu = slot.get();
            if (menu && menu->menuFlags.all(static_cast<RE::IMenu::Flag>(kUnpausedMenuFlagBit))) {
                ++count;
            }
        }
        return count;
    }

    namespace {
        // Map an IMenu* on the stack to its ShowPlayerInMenuEntry. Returns
        // nullptr if the menu isn't one of our hijacked types. Used by both
        // ShouldBlockPlayerMovement and IsAnyUnpausedMenuAllowingMovement.
        const SettingsManager::ShowPlayerInMenuEntry* EntryForMenu(RE::IMenu* a_menu)
        {
            const auto& s = SettingsManager::GetSingleton();
            if (skyrim_cast<RE::BarterMenu*>(a_menu))    return &s.showPlayerInBarter;
            if (skyrim_cast<RE::InventoryMenu*>(a_menu)) return &s.showPlayerInInventory;
            if (skyrim_cast<RE::ContainerMenu*>(a_menu)) return &s.showPlayerInContainer;
            if (skyrim_cast<RE::MagicMenu*>(a_menu))     return &s.showPlayerInMagic;
            if (skyrim_cast<RE::TweenMenu*>(a_menu))     return &s.showPlayerInTween;
            if (skyrim_cast<RE::FavoritesMenu*>(a_menu)) return &s.showPlayerInFavorites;
            return nullptr;
        }
    }

    bool ShouldBlockPlayerMovement()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        for (auto& slot : ui->menuStack) {
            auto* menu = slot.get();
            if (!menu) continue;
            if (!menu->menuFlags.all(static_cast<RE::IMenu::Flag>(kUnpausedMenuFlagBit))) continue;
            const auto* entry = EntryForMenu(menu);
            if (entry && !entry->allowMovement) return true;
        }
        return false;
    }

    bool IsAnyUnpausedMenuAllowingMovement()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        for (auto& slot : ui->menuStack) {
            auto* menu = slot.get();
            if (!menu) continue;
            if (!menu->menuFlags.all(static_cast<RE::IMenu::Flag>(kUnpausedMenuFlagBit))) continue;
            const auto* entry = EntryForMenu(menu);
            if (entry && entry->allowMovement) return true;
        }
        return false;
    }

    bool IsAnyUnpausedMenuAllowingCameraControl()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        for (auto& slot : ui->menuStack) {
            auto* menu = slot.get();
            if (!menu) continue;
            if (!menu->menuFlags.all(static_cast<RE::IMenu::Flag>(kUnpausedMenuFlagBit))) continue;
            const auto* entry = EntryForMenu(menu);
            if (entry && entry->allowCameraControl) return true;
        }
        return false;
    }

    // Menus that own a 3D item preview, i.e. the ones where the POV button
    // means "inspect" rather than "change view".
    bool IsItemPreviewMenuOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        return ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::BarterMenu::MENU_NAME)    ||
               ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::MagicMenu::MENU_NAME)     ||
               ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME);
    }

    // Is an item actually ZOOMED IN for inspection right now? zoomProgress
    // is 1 while zoomed, 0 while not and in between during the transition,
    // so anything above ~0 means the preview owns the input.
    bool IsInventoryItemZoomed()
    {
        auto* inv3d = RE::Inventory3DManager::GetSingleton();
        if (!inv3d) return false;
        return inv3d->GetRuntimeData().zoomProgress > 0.01f;
    }


    void Install()
    {
        if (sInstalled) return;
        sInstalled = true;
        InstallMainThreadHook();
        InstallInputHandlerHooks();
        InstallDialogueMenuHooks();
        HijackCreator(RE::BarterMenu::MENU_NAME,    CreateBarterMenu);
        HijackCreator(RE::InventoryMenu::MENU_NAME, CreateInventoryMenu);
        HijackCreator(RE::ContainerMenu::MENU_NAME, CreateContainerMenu);
        HijackCreator(RE::MagicMenu::MENU_NAME,     CreateMagicMenu);
        HijackCreator(RE::TweenMenu::MENU_NAME,     CreateTweenMenu);
        HijackCreator(RE::FavoritesMenu::MENU_NAME, CreateFavoritesMenu);
        // Sleep/Wait Menu intentionally not hijacked (see CreateMenuShared
        // comment for the reason — needs SleepWaitMenuEx port).
        // Future menus add another HijackCreator call + matching thunk.
    }
}
