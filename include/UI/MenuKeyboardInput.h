#pragma once

// Use the framework's named keys, independently of its optional legacy IO
// arrays, controller connection, or ownership of stock ImGui navigation.
namespace DietDrCamera
{
    struct MenuKeyboardInput
    {
        bool left = false, right = false, up = false, down = false;
        bool confirm = false;
        bool Any() const { return left || right || up || down || confirm; }

        static MenuKeyboardInput Read(bool enabled)
        {
            using namespace ImGuiMCP;
            namespace Gui = ImGuiMCP::ImGui;
            if (!enabled || Gui::GetIO()->WantTextInput ||
                Gui::IsMouseDown(0) || Gui::IsMouseDown(1)) return {};
            const auto pressed = [](ImGuiKey key) {
                const auto* data = Gui::GetKeyData(key);
                return data->Down && data->DownDuration == 0.0f;
            };
            return {Gui::GetKeyData(ImGuiKey_LeftArrow)->Down,
                Gui::GetKeyData(ImGuiKey_RightArrow)->Down,
                Gui::GetKeyData(ImGuiKey_UpArrow)->Down,
                Gui::GetKeyData(ImGuiKey_DownArrow)->Down,
                pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)};
        }
    };
}
