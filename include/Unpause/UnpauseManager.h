#pragma once

#include <RE/I/IMenu.h>

namespace DietDrCamera::UnpauseManager
{
    // Custom bit reused from the high end of IMenu::menuFlags to mark
    // menus we have unpaused. Pattern lifted from SkyrimSoulsRE so we
    // can count "how many unpaused menus are open" by scanning menuStack.
    // 1 << 28 is unused by vanilla flags (highest vanilla is 1 << 27 =
    // kUsesMovementToDirection).
    static constexpr std::uint32_t kUnpausedMenuFlagBit = 1u << 28;

    // Walk RE::UI::menuStack and count menus carrying the kUnpausedMenuFlagBit.
    [[nodiscard]] std::uint32_t GetUnpausedMenuCount();

    // True when an unpaused Barter is on the stack. Barter is the one
    // unpaused menu where we want the player rooted in place (matches
    // the dialogue-camera framing — you don't walk away mid-trade).
    [[nodiscard]] bool ShouldBlockPlayerMovement();

    // True when at least one unpaused menu is on the stack AND that
    // menu's per-menu allowMovement is on. Used by the MenuControls
    // ProcessEvent hook to know whether to relabel WASD / left-stick
    // events as gameplay-movement events while the menu is open.
    [[nodiscard]] bool IsAnyUnpausedMenuAllowingMovement();

    // True when at least one unpaused menu on the stack has
    // allowCameraControl on. Used by the LookHandler input gate to
    // let mouse and right-stick events through to the camera while
    // the menu is open.
    [[nodiscard]] bool IsAnyUnpausedMenuAllowingCameraControl();

    // True when a menu that owns a 3D item preview is open (Inventory,
    // Barter, Container, Magic, Favorites). In those the POV button means
    // INSPECT, not "change view", so every DDC path that would otherwise
    // swing the camera on that button has to stand down.
    [[nodiscard]] bool IsItemPreviewMenuOpen();

    // True while an item is actually zoomed in for inspection. The zoomed
    // preview is rotated with the same stick the camera uses, so the camera
    // gives the stick up for as long as this is true.
    [[nodiscard]] bool IsInventoryItemZoomed();

    // Install hooks. Must be called after kDataLoaded — menuMap is
    // populated by then. Safe to call multiple times; second call is a
    // no-op.
    void Install();
}
