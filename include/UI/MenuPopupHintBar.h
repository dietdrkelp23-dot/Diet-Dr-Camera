#pragma once

#include "UI/MenuHintBarLayout.h"

namespace DietDrCamera
{
    // The popup owns this child so hints remain inside its fixed bounds and
    // participate in normal modal ordering/dimming. Drawing is deferred until
    // the navigation cursor and clipboard target are finalized for the frame.
    class MenuPopupHintBar
    {
        using Window = ImGuiMCP::ImGuiWindow;

    public:
        static float ReservedHeight()
        {
            namespace Gui = ImGuiMCP::ImGui;
            if (!Supported()) return 0;
            return Height(Gui::GetCurrentWindow()) + Gui::GetStyle()->ItemSpacing.y;
        }

        void Create(int layer)
        {
            using namespace ImGuiMCP;
            namespace Gui = ImGuiMCP::ImGui;
            if (!Supported()) return;
            auto* popup = Gui::GetCurrentWindow();
            const float height = Height(popup);
            Gui::BeginChild("##DDCLocationHints", ImVec2(0, height), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            // Match the main footer's base font, independently of a popup's
            // enlarged headings and of the section that opened it.
            Gui::SetWindowFontScale(1.0f / popup->FontWindowScale);
            auto* footer = Gui::GetCurrentWindow();
            Gui::EndChild();
            const int frame = Gui::GetFrameCount();
            if (frame != frame_ || layer >= layer_) {
                footer_ = footer;
                frame_ = frame;
                layer_ = layer;
            }
        }

        Window* WindowForFrame() const
        {
            const int frame = ImGuiMCP::ImGui::GetFrameCount();
            return frame_ == frame && footer_ && footer_->Active && !footer_->Hidden &&
                footer_->LastFrameActive == frame ? footer_ : nullptr;
        }

    private:
        static bool Supported()
        {
            return std::strcmp(ImGuiMCP::ImGui::GetVersion(), "1.90.8") == 0;
        }

        static float Height(Window* popup)
        {
            namespace Gui = ImGuiMCP::ImGui;
            float fontSize = Gui::ImGuiWindowManager::CalcFontSize(popup) / popup->FontWindowScale;
            if (popup->ParentWindow) fontSize /= popup->ParentWindow->FontWindowScale;
            const auto* style = Gui::GetStyle();
            return fontSize + style->FramePadding.y * 2 + style->ItemSpacing.y + style->WindowPadding.y;
        }

        Window* footer_ = nullptr;
        int frame_ = -1;
        int layer_ = 0;
    };
}
