# SKSE Menu Framework controller compatibility checks

Run from the repository root with Visual Studio 2026's C++ build tools and CMake:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/check-menu-controller.ps1
```

The script downloads the unmodified ImGui and `GamepadNavigation.cpp` sources
from SKSE Menu Framework commit
[`d448520c07dec8430e29adad4337fcf4319847a7`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/d448520c07dec8430e29adad4337fcf4319847a7)
into the ignored `build/menu-controller` directory. This is the 3.15 navigation
implementation. Engine, texture and translation services are stubbed; the
navigation implementation and ImGui are real.

The interaction checks run DDC's production `MenuControllerCapture` class with
a small fixture matching the framework's window layout and section-selection
contract. They cover section selection and reselection staying in the sidebar
(by controller and mouse), selection-button consumption, sidebar movement and
folder toggles, Right entering the page, popup Back,
settings focus, suppression of duplicate right-stick scrolling, switching to
another mod, native controls on that mod, mouse interaction, controller disconnect
and panel close. Real tab widgets reproduce the stale blue focus box after mouse
selection and a return to controller input. Draw-list checks require that box
to disappear on DDC while sidebar/other-mod highlights remain; a text editor
checks that native editing focus and typing still work, including while controller
focus remains in the sidebar. A per-frame check verifies that framework section
selection cannot leave sidebar mode focused in the right pane for the next input.
The framework's raw Back
gate is reproduced in the fixture:
the game input hook itself is not executed.

The fixture also runs the framework's real hint bar and DDC's production footer
drawing code. It checks contextual sidebar/page/slider/popup hints, live binding
changes, clearing bindings, keyboard/controller distinctions and PlayStation
button names. Draw-list checks cover a narrow footer, unchanged layout and input
state, late focus transfer to a foreign window, settings, other mods, mouse input,
disconnect and close. Other pages retain their original footer vertices and
indices. The small fixture supplies navigation context; it does not run the full
DDC page renderers or the settings binding buttons.

Keyboard support is limited to arrow navigation and the existing Copy, Paste and
Quick Tune hotkeys. The keyboard legend uses the same centered groups and tight
spacing. When the framework omits its controller footer, a DDC strip attaches
below the full panel (above it when the panel is at the bottom of the screen),
leaving the settings and sidebar unobstructed. It disappears on another mod's
page, in text editing, while binding a key, or when the panel closes. Controller
bindings remain labeled as controller buttons in a keyboard legend.

With no controller or gamepad backend present, the fixture checks Right entering
the page, all four arrows feeding the shared directional navigation, no extra
Enter/Tab/Backspace navigation, and arrows yielding to text editing and binding
capture. It checks the added strip's centering, panel alignment, focus, style and
other-mod isolation. Production page rendering and Win32/XInput polling still
require an in-game check.

The readability checks require each binding and its explanation to remain inside
the same outlined group, with no text overlap and a compact, uniform gap of at
least eight pixels between neighboring groups. Responsive layouts reuse the
original footer dimensions.
Labels distinguish entry/tab/override copying, slider cancel/coarse steps, and
stick clicks from stick movement used for scrolling.

Position checks compare centering, row assignments and font sizes across every
clipboard subject and menu mode, with and without Paste or tabs. The complete
set of labels determines the fitting budget, so hovering a longer name or
entering slider mode cannot wrap or shrink the row. Visible boxes fit
their current contents and pack together with equal gaps; hidden actions consume
no horizontal space. Each row centers its visible boxes within the footer,
including after captions change or actions disappear. Checks cover four footer
widths, keyboard/Xbox/PlayStation names and long custom
bindings. They require compact, equal gaps even after hiding Paste or tabs, and
normal padding after the caption and between an inline binding and caption.

The production clipboard-hint resolver is checked with controller navigation
items for entries, transition overrides, Reset and ungrabbed sliders. Clipboard
hints require a current-frame target matching the controller item and active
layer. Checks cover stale targets, idle mouse hover, removed/disabled items,
popup-layer changes, and groups that inherit a slider/Reset item's ID. A native
ImGui fixture scrolls a school list in both directions across offscreen rows:
current-frame widget registration validates a target even while scrolling is
pending or its rectangle differs from the previous frame's cursor rectangle.
The rendering fixture also verifies that the resolved subject changes the copy
and paste labels and that clearing the subject removes both hints immediately.
Copy remains available with an empty clipboard, while Paste is hidden until a
payload exists and disappears again if it is cleared. The grouped footer checks
use the same capitalization and wording as controls such as Transition Override.

Both interaction and SDK layout checks run with legacy key arrays enabled and
disabled. The SDK omits those optional arrays, so the production capture code
uses the exported Style pointer for its version-gated `ActiveIdAllowOverlap`
access. The late footer focus query temporarily supplies the DDC page as the
current window, calls the exported focus query, then immediately restores it.
This avoids the additional optional legacy field before native navigation state.
Static assertions verify both relative offsets and the window/font/draw-list
fields used by capture and the footer. Capture is enabled only for ImGui 1.90.8; other versions
retain the existing navigation-flag fallback without accessing those internals.
Footer replacement also stays disabled for other ImGui versions.

These checks do not render DDC's full menu or exercise XInput, Skyrim, or the
installed framework DLL. Confirm that choosing a section keeps controller focus
in the list and Right shows the cursor in its controls. Check D-pad movement,
slider A/B, bumper tabs, trigger/right-stick scrolling, sidebar B/Left and Right
return in game. Also open another mod's page and framework settings,
and leave Quick Tune open while switching between main-menu pages. Move from
an entry to Reset and an ungrabbed slider: neither should show copy/paste.
Transition, location, weapon and enemy override buttons should identify their
own clipboard contents; tab and environment buttons should identify theirs.
Target Lock's General tab must have no clipboard hints or right-click clipboard
menu, and Copy/Paste must not act on the previous controller target after moving
onto General.
