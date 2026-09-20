#pragma once
// Engine services unused by the navigation tests. The navigation and ImGui
// implementations themselves are built unmodified from the pinned framework.
namespace RE {
    enum class INPUT_DEVICE { kKeyboard, kMouse, kGamepad };
    enum class PC_GAMEPAD_TYPE { kOrbis, kXbox };
    struct ControlMap {
        static ControlMap* GetSingleton() { return nullptr; }
        PC_GAMEPAD_TYPE GetGamePadType() const { return PC_GAMEPAD_TYPE::kXbox; }
    };
}
namespace logger { template<class... Args> void error(const char*, Args&&...) {} }
