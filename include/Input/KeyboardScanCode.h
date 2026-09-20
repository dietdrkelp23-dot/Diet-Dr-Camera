#pragma once

#include <Windows.h>
#include <cstdint>

namespace DietDrCamera
{
    // Match Skyrim's DirectInput codes, including the extended-key bit. The
    // non-EX Windows map aliases Down with Numpad 2 (and every navigation key
    // with its numpad counterpart), making unrelated hotkeys fire in menus.
    inline std::uint32_t KeyboardScanCodeFromVirtualKey(UINT key)
    {
        // Poll the left/right modifier keys themselves, not their aggregate.
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) return 0;
        if (key == VK_PAUSE) return 0xC5;
        if (key == VK_NUMLOCK) return 0x45;
        if (key == VK_SNAPSHOT) return 0xB7;
        const UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC_EX);
        if (scan == 0) return 0;
        bool extended = (scan & 0xFF00u) == 0xE000u;
        // Some Windows layouts omit E0 even with the EX map. These virtual
        // keys unambiguously identify the extended cluster or right modifier.
        switch (key) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE:
            extended = true;
        }
        return (scan & 0xFFu) | (extended ? 0x80u : 0u);
    }
}
