#pragma once

#include <cstddef>

namespace DietDrCamera
{
    class MenuUI
    {
    public:
        [[nodiscard]] static MenuUI& GetSingleton();

        void Init();
        static void InvalidateSettingsReferences();

        // Apply resolution-aware UI scaling. Called per-frame from a
        // non-menu hook so the scale lands BEFORE the user first opens
        // the menu, eliminating the launch-time snap to scaled style.
        // Internally short-circuits on every call after the first
        // successful apply (and re-applies if DisplaySize.y changes).
        static void TickUiScale();

        // Quick Tune overlay — small top-left panel that surfaces the
        // currently-active camera profile's sliders without opening the
        // full DDC menu.
        //
        // The hotkey is wired so OpenQuickTune() runs from the input
        // sink (keydown edge) and CLOSE happens from the render-callback
        // poll (because SKSE Menu Framework's BlockUserInput=true gates
        // the InputEventSink while the overlay is open). Splitting the
        // sources prevents same-frame open/close races that surfaced
        // as "hotkey spam" when the sink toggled.
        static bool OpenQuickTune();
        static bool ToggleQuickTune();  // legacy — kept for callers that need toggle semantics.

        // Read by CameraController::Update so its menuBlocking gate
        // doesn't suppress profile writes while Quick Tune is open.
        // Without this, the user's TL/Categories slider edits mutate
        // the profile correctly but the engine writes are skipped.
        [[nodiscard]] static bool IsQuickTuneOpen();

        // True while the Quick Tune overlay WINDOW is actually open. Distinct
        // from IsQuickTuneOpen() above, which reads a section-active flag that
        // (currently) is never set — so this is the reliable "is QT on screen"
        // signal. The noise controller uses it to route 1p noise through the
        // deferred late-hook apply while QT is open, so the shake survives QT's
        // per-frame camera recompose the same way 3p noise does natively.
        [[nodiscard]] static bool IsQuickTuneWindowOpen();

        // Framework main window, including its sidebar and override popups.
        // Noise uses the same held-duration preview here as in Quick Tune.
        [[nodiscard]] static bool IsMainMenuOpen();

        // True while Quick Tune holds the TDM directional-movement disable (it
        // claims one while open with a target locked, so QT's R3 POV toggle
        // can't drop the lock). PUBLISHED FOR THE TDM ORPHAN WATCHDOG: that
        // watchdog releases any claim with "no live holder", and this holder
        // was added without being registered — so the claim was reaped the
        // frame it was taken (21:59 log: disabled → "orphaned … releasing" →
        // re-enabled, same millisecond), and QT's own close-path release then
        // DOUBLE-released, desyncing the claim state. Every place that judges
        // claims must consult every holder.
        [[nodiscard]] static bool HasQuickTuneDMClaim();

        // True while a DDC menu framework window is capturing input (the
        // [SMF-BLOCK] state): the GAME legitimately receives no input, so the
        // player standing still with the stick held is EXPECTED. Published for
        // the [FREEZE] watchdog, which used to count these frames as "input
        // but not moving" and burned its entire 20-line log budget on menu
        // sessions — leaving the one real freeze of the session unlogged.
        [[nodiscard]] static bool IsGameInputBlocked();

    private:
        MenuUI() = default;

        static void __stdcall RenderTargetLock();
        static void __stdcall RenderFirstPerson();
        static void __stdcall RenderDialogue();
        static void __stdcall RenderCameraNoise();
        static void __stdcall RenderExtras();
        static void __stdcall RenderInformation();
        static void __stdcall RenderShowPlayerInMenus();
        static void __stdcall RenderPresets();
        static void __stdcall RenderCinematics();
        static void __stdcall RenderDebug();
        static void __stdcall RenderCategories();
        static void __stdcall RenderQuickTune();
    };
}
