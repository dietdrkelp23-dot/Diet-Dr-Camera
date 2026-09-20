#include "Input/KeyboardScanCode.h"
#include <array>
#include <iostream>

int main()
{
    struct Pair { UINT navigation, numpad; std::uint32_t expected; };
    constexpr std::array pairs{
        Pair{VK_DOWN, VK_NUMPAD2, 0x50}, Pair{VK_UP, VK_NUMPAD8, 0x48},
        Pair{VK_LEFT, VK_NUMPAD4, 0x4B}, Pair{VK_RIGHT, VK_NUMPAD6, 0x4D},
        Pair{VK_HOME, VK_NUMPAD7, 0x47}, Pair{VK_END, VK_NUMPAD1, 0x4F},
        Pair{VK_PRIOR, VK_NUMPAD9, 0x49}, Pair{VK_NEXT, VK_NUMPAD3, 0x51},
        Pair{VK_INSERT, VK_NUMPAD0, 0x52}, Pair{VK_DELETE, VK_DECIMAL, 0x53}};
    for (const auto& pair : pairs) {
        if (DietDrCamera::KeyboardScanCodeFromVirtualKey(pair.navigation) != (pair.expected | 0x80) ||
            DietDrCamera::KeyboardScanCodeFromVirtualKey(pair.numpad) != pair.expected) {
            std::cerr << "Navigation key aliases its numpad hotkey: " << pair.navigation << " values " << std::hex
                << DietDrCamera::KeyboardScanCodeFromVirtualKey(pair.navigation) << ' '
                << DietDrCamera::KeyboardScanCodeFromVirtualKey(pair.numpad) << " native "
                << MapVirtualKeyA(pair.navigation, MAPVK_VK_TO_VSC_EX) << ' '
                << MapVirtualKeyA(pair.numpad, MAPVK_VK_TO_VSC_EX) << '\n';
            return 1;
        }
    }
    struct Key { UINT vk; std::uint32_t expected; };
    constexpr std::array keys{Key{VK_RETURN, 0x1C}, Key{VK_LCONTROL, 0x1D}, Key{VK_RCONTROL, 0x9D},
        Key{VK_LMENU, 0x38}, Key{VK_RMENU, 0xB8}, Key{VK_PAUSE, 0xC5}, Key{VK_NUMLOCK, 0x45},
        Key{VK_SNAPSHOT, 0xB7}, Key{VK_DIVIDE, 0xB5}, Key{VK_CONTROL, 0}, Key{VK_SHIFT, 0}, Key{VK_MENU, 0}};
    for (const auto& key : keys) {
        if (DietDrCamera::KeyboardScanCodeFromVirtualKey(key.vk) != key.expected) {
            std::cerr << "Incorrect DirectInput identity for key: " << key.vk << '\n';
            return 1;
        }
    }
    std::cout << "Navigation/numpad hotkeys and extended key identities passed\n";
}
