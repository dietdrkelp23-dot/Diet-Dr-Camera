#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace DietDrCamera
{
    inline float MenuHintBarHeight(ImGuiMCP::ImGuiWindow* panel)
    {
        const auto* style = ImGuiMCP::ImGui::GetStyle();
        return ImGuiMCP::ImGui::ImGuiWindowManager::CalcFontSize(panel) +
            style->FramePadding.y * 2 + style->ItemSpacing.y + style->WindowPadding.y;
    }

    // SMF sizes its two panes before calling a section renderer. Keep the
    // native pane windows at the controller layout size while DDC is selected,
    // including on keyboard frames. ImGui still performs their normal Begin()
    // layout, clipping, scrollbar and hit-test setup with that size.
    class MenuHintBarLayout
    {
        using Window = ImGuiMCP::ImGuiWindow;

    public:
        void BeginPage()
        {
            namespace Gui = ImGuiMCP::ImGui;
            RestoreSizeConditions();
            if (std::strcmp(Gui::GetVersion(), "1.90.8") != 0) return;
            auto* page = Gui::GetCurrentWindow();
            auto* panel = page ? page->ParentWindow : nullptr;
            if (!panel || std::strcmp(panel->Name, "#MCPMainWindow") != 0) return;
            Window* tree = nullptr;
            for (int i = 0; i < panel->DC.ChildWindows.Size; ++i) {
                auto* child = panel->DC.ChildWindows.Data[i];
                if (std::strstr(child->Name, "/SKSEModControlPanelTreeView_") != nullptr)
                    tree = child;
            }
            if (!tree) return;
            page_ = page;
            panel_ = panel;
            tree_ = tree;
            pageFrame_ = Gui::GetFrameCount();
            bodyOffset_ = tree->Pos.y - (panel->Pos.y + panel->DecoOuterSizeY1 +
                panel->WindowPadding.y - panel->Scroll.y);
        }

        void BeforePanel(bool open)
        {
            using namespace ImGuiMCP;
            namespace Gui = ImGuiMCP::ImGui;
            RestoreSizeConditions();
            if (!open || !panel_ || pageFrame_ != Gui::GetFrameCount() - 1) return;
            treeSizeConditions_ = tree_->SetWindowSizeAllowFlags;
            pageSizeConditions_ = page_->SetWindowSizeAllowFlags;
            sizesHeld_ = true;
            SizePanes(panel_->SizeFull);
            // BeginChild supplies an unconditional size. Preserve our sizes
            // for these two Begin calls only; restore before DDC renders.
            tree_->SetWindowSizeAllowFlags &= ~ImGuiCond_Always;
            page_->SetWindowSizeAllowFlags &= ~ImGuiCond_Always;

            // The main window normally renders next. Its size callback also
            // runs on resize, after font/padding setup, so both panes follow
            // the current frame's geometry rather than lagging a frame.
            // If another window renders first, this callback is a no-op.
            Gui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, FLT_MAX), &OnPanelSize, this);
        }

        void EndFrame()
        {
            RestoreSizeConditions();
            if (pageFrame_ != ImGuiMCP::ImGui::GetFrameCount()) {
                panel_ = page_ = tree_ = nullptr;
                pageFrame_ = -1000;
            }
        }

    private:
        static void OnPanelSize(ImGuiMCP::ImGuiSizeCallbackData* data)
        {
            auto* self = static_cast<MenuHintBarLayout*>(data->UserData);
            if (self->sizesHeld_ && ImGuiMCP::ImGui::GetCurrentWindow() == self->panel_)
                self->SizePanes(data->DesiredSize);
        }

        void SizePanes(ImGuiMCP::ImVec2 size)
        {
            namespace Gui = ImGuiMCP::ImGui;
            const auto* style = Gui::GetStyle();
            const float width = std::trunc(size.x) - panel_->WindowPadding.x * 2 -
                panel_->DecoOuterSizeX1 - panel_->DecoOuterSizeX2;
            const float height = std::max(4.0f, std::trunc(size.y) - panel_->WindowPadding.y * 2 -
                panel_->DecoOuterSizeY1 - panel_->DecoOuterSizeY2 - bodyOffset_ -
                MenuHintBarHeight(panel_) - style->ItemSpacing.y);
            // Match SMF's 30% section tree and remaining-width settings pane.
            const float treeWidth = std::trunc(width * 0.3f);
            Gui::SetWindowSize(tree_, ImGuiMCP::ImVec2(std::max(4.0f, treeWidth), height), 0);
            Gui::SetWindowSize(page_, ImGuiMCP::ImVec2(std::max(4.0f, width - treeWidth - style->ItemSpacing.x), height), 0);
        }

        void RestoreSizeConditions()
        {
            if (!sizesHeld_) return;
            tree_->SetWindowSizeAllowFlags = treeSizeConditions_;
            page_->SetWindowSizeAllowFlags = pageSizeConditions_;
            sizesHeld_ = false;
        }

        Window* panel_ = nullptr;
        Window* page_ = nullptr;
        Window* tree_ = nullptr;
        int pageFrame_ = -1000;
        int treeSizeConditions_ = 0, pageSizeConditions_ = 0;
        float bodyOffset_ = 0;
        bool sizesHeld_ = false;
    };
}
