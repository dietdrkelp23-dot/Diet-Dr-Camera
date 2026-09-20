#pragma once

#include <cstddef>
#include <cstring>
#include "UI/MenuKeyboardInput.h"

// Include the framework bindings before this header. The headless compatibility
// checks provide the same API using the framework's actual ImGui implementation.
namespace DietDrCamera
{
    namespace MenuCaptureGui = ImGuiMCP::ImGui;
    class MenuControllerCapture
    {
        using Window = ImGuiMCP::ImGuiWindow;
        using ID = ImGuiMCP::ImGuiID;

    public:
        bool BeginPage(const void* renderer, bool connected, bool keyboard = false)
        {
            using namespace ImGuiMCP;
            RestoreSidebarConfirm();
            keyboard_ = keyboard;
            const int frame = MenuCaptureGui::GetFrameCount();
            const bool changed = renderer != renderer_ || pageFrame_ != frame - 1;
            renderer_ = renderer;
            pageFrame_ = frame;
            page_ = MenuCaptureGui::GetCurrentWindow();
            owner_ = MenuCaptureGui::GetID("##DDCControllerCapture");
            supported_ = std::strcmp(MenuCaptureGui::GetVersion(), "1.90.8") == 0;
            if (supported_) FindSidebar();
            else tree_ = nullptr;

            pageFocused_ = PageHasFocus();
            // Selecting a section displays it without entering its controls.
            // A click back into the sidebar also keeps the controller there,
            // even when it selects the section that is already displayed.
            if (changed || (pageFocused_ && MenuCaptureGui::IsMouseClicked(0) &&
                    !MenuCaptureGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)))
                sidebarMode_ = true;
            usable_ = (connected || keyboard) && pageFocused_;
            if (usable_) {
                if (!sidebarMode_ && !MenuCaptureGui::IsPopupOpen(nullptr,
                        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                    (MenuCaptureGui::GetActiveID() == 0 || OwnsCapture())) {
                    MenuCaptureGui::FocusWindow(page_, ImGuiFocusRequestFlags_UnlessBelowModal);
                }
                Acquire();
            } else {
                ReleaseCapture();
            }
            return changed;
        }

        void EndPage()
        {
            // Mouse controls and popups may have changed focus during rendering.
            pageFocused_ = PageHasFocus();
            usable_ = usable_ && pageFocused_;
            if (usable_) Acquire();
            else ReleaseCapture();
        }

        bool IsSidebar() const { return sidebarMode_; }
        bool IsSupported() const { return supported_; }
        bool IsPageFocused() const { return pageFocused_; }
        bool IsUsable() const { return usable_; }
        bool RenderedThisFrame() const { return pageFrame_ == MenuCaptureGui::GetFrameCount(); }
        bool OwnsCapture() const { return owner_ != 0 && MenuCaptureGui::GetActiveID() == owner_; }

        Window* FocusedPanelWindow() const
        {
            using namespace ImGuiMCP;
            if (!supported_ || !usable_ || !RenderedThisFrame() || !page_ || !page_->ParentWindow) return nullptr;
            // Recheck focus after all framework windows have rendered. The
            // Options menu or settings may have taken it after EndPage.
            // IsWindowFocused reads CurrentWindow without changing input or
            // drawing state. Temporarily supply the page for that query, then
            // restore it immediately. This avoids reading NavWindow through
            // the SDK's incompatible optional legacy-input layout.
            constexpr auto currentFromStyle = offsetof(ImGuiContext, CurrentWindow) - offsetof(ImGuiContext, Style);
            auto* style = reinterpret_cast<unsigned char*>(MenuCaptureGui::GetStyle());
            auto& current = *reinterpret_cast<Window**>(style + currentFromStyle);
            auto* saved = current;
            current = page_;
            const bool focused = PageHasFocus();
            current = saved;
            if (!focused) return nullptr;
            return page_->ParentWindow;
        }

        Window* HintBarWindow() const
        {
            auto* panel = FocusedPanelWindow();
            if (!panel) return nullptr;
            const auto& children = panel->DC.ChildWindows;
            for (int i = 0; i < children.Size; ++i) {
                Window* child = children.Data[i];
                if (child->Active && !child->Hidden && child->LastFrameActive == MenuCaptureGui::GetFrameCount() &&
                    std::strstr(child->Name, "/##MCPGamepadHints_") != nullptr) return child;
            }
            return nullptr;
        }

        void SetSidebar(bool sidebar)
        {
            using namespace ImGuiMCP;
            sidebarMode_ = sidebar;
            if (!usable_ || !supported_) return;
            Window* target = sidebar ? tree_ : page_;
            if (target) {
                ReleaseCapture();
                // FocusWindow restores the remembered tree item. Forcing a new
                // nav initialization here would discard the selected section.
                MenuCaptureGui::FocusWindow(target, ImGuiFocusRequestFlags_UnlessBelowModal);
                if (sidebar) MenuCaptureGui::NavRestoreHighlightAfterMove();
                Acquire();
            }
        }

        void BeforeNewFrame(bool connected, bool panelOpen, bool returningToPage, bool keyboard = false)
        {
            using namespace ImGuiMCP;
            RestoreSidebarConfirm();
            keyboard_ = keyboard;
            resume_ = (connected || keyboard) && panelOpen && usable_ && OwnsCapture() &&
                pageFrame_ == MenuCaptureGui::GetFrameCount();
            if (!resume_) {
                ReleaseCapture();
                return;
            }
            // Raw framework Back handling has already seen this capture. Let
            // stock ImGui process sidebar arrows/A, then reacquire before the
            // framework's separate 3.15 navigation runs during rendering.
            if (sidebarMode_ && !returningToPage) {
                ReleaseCapture();
                if (keyboard) {
                    auto* io = MenuCaptureGui::GetIO();
                    savedNavFlags_ = io->ConfigFlags & kNavFlags;
                    navFlagsSaved_ = true;
                    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                }
            }
            else {
                auto* io = MenuCaptureGui::GetIO();
                savedNavFlags_ = io->ConfigFlags & kNavFlags;
                navFlagsSaved_ = true;
                io->ConfigFlags &= ~kNavFlags;
            }
        }

        void AfterNewFrame()
        {
            using namespace ImGuiMCP;
            RestoreFlags();
            if (!resume_) return;
            // A click can select another mod or open framework settings. Leave
            // its real widget in charge; never replace another item's ActiveId.
            if (!MouseDown()) Acquire();
            if (OwnsCapture() && sidebarMode_ && keyboard_ &&
                !MenuCaptureGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                MenuKeyboardInput::Read(true).confirm) {
                // SMF's RenderNode selects a focused section on GamepadFaceDown
                // only. Bridge Enter for the tree's render, then restore the
                // real controller state before DDC renders (or at EndFrame if
                // the selected section belongs to another mod).
                auto* confirm = MenuCaptureGui::GetKeyData(ImGuiKey_GamepadFaceDown);
                savedConfirm_ = *confirm;
                confirmSaved_ = true;
                confirm->Down = true;
                confirm->DownDuration = 0.0f;
                confirm->DownDurationPrev = -1.0f;
                confirm->AnalogValue = 1.0f;
                MenuCaptureGui::NavRestoreHighlightAfterMove();
            }
            if (OwnsCapture() && !sidebarMode_) {
                // The framework also polls keys directly (not through ImGui
                // navigation flags), notably its right-stick scrolling path.
                for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
                    if (MenuCaptureGui::GetKeyOwner(static_cast<ImGuiKey>(key)) == kNoKeyOwner) {
                        MenuCaptureGui::SetKeyOwner(static_cast<ImGuiKey>(key), owner_, ImGuiInputFlags_LockThisFrame);
                    }
                }
            }
        }

        void EndFrame()
        {
            RestoreSidebarConfirm();
            RestoreFlags();
            ReleaseKeys();
            if (!RenderedThisFrame()) Reset();
            // Focusing another window automatically clears our ActiveId. Do not
            // acquire it again here: settings and other overlays own that focus.
            else if (OwnsCapture()) {
                // SMF requests content focus after a section's callback. Restore
                // the tree before the next input frame; only Right enters DDC.
                // A real widget or another window owning focus prevents this.
                if (sidebarMode_ && tree_)
                    MenuCaptureGui::FocusWindow(tree_, ImGuiMCP::ImGuiFocusRequestFlags_UnlessBelowModal);
                MenuCaptureGui::KeepAliveID(owner_);
            }
        }

        void Reset()
        {
            RestoreSidebarConfirm();
            RestoreFlags();
            ReleaseKeys();
            ReleaseCapture();
            pageFrame_ = -1000;
            renderer_ = nullptr;
            page_ = tree_ = nullptr;
            usable_ = pageFocused_ = resume_ = sidebarMode_ = false;
            keyboard_ = false;
        }

    private:
        static constexpr int kNavFlags = ImGuiMCP::ImGuiConfigFlags_NavEnableGamepad |
            ImGuiMCP::ImGuiConfigFlags_NavEnableKeyboard;
        static constexpr ID kNoKeyOwner = static_cast<ID>(-1);

        static bool MouseDown()
        {
            return MenuCaptureGui::IsMouseDown(0) || MenuCaptureGui::IsMouseDown(1);
        }

        bool PageHasFocus() const
        {
            using namespace ImGuiMCP;
            // Children includes our own popups. The root check deliberately
            // excludes popup hierarchy, so framework Options does not count.
            return MenuCaptureGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) ||
                MenuCaptureGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows |
                    ImGuiFocusedFlags_NoPopupHierarchy);
        }

        void FindSidebar()
        {
            tree_ = nullptr;
            if (!page_ || !page_->ParentWindow) return;
            const auto& children = page_->ParentWindow->DC.ChildWindows;
            for (int i = 0; i < children.Size; ++i) {
                Window* child = children.Data[i];
                if (child != page_ && std::strstr(child->Name, "/SKSEModControlPanelTreeView_") != nullptr) {
                    tree_ = child;
                    break;
                }
            }
        }

        void Acquire()
        {
            using namespace ImGuiMCP;
            if (!supported_ || !usable_ || !page_ || MouseDown()) return;
            const auto active = MenuCaptureGui::GetActiveID();
            if (active != 0 && active != owner_) return;
            if (active != owner_) MenuCaptureGui::SetActiveID(owner_, page_);
            MenuCaptureGui::KeepAliveID(owner_);

            // This is a custom navigation widget, not a mouse drag. Allow other
            // controls to receive hover/clicks while it captures the controller.
            // Anchor at Style instead of casting the whole context: cimgui's
            // generated IO omits optional legacy key arrays in the shipped DLL.
            // Both native build layouts are checked in MenuControllerLayoutChecks.
            constexpr auto overlapFromStyle = offsetof(ImGuiContext, ActiveIdAllowOverlap) -
                offsetof(ImGuiContext, Style);
            auto* style = reinterpret_cast<unsigned char*>(MenuCaptureGui::GetStyle());
            *reinterpret_cast<bool*>(style + overlapFromStyle) = true;

            // A mouse click can set NavId even on a NoNav tab. SMF's separate
            // foreground highlight ignores both NoNav and our active capture,
            // so discard that stale native focus when our page cursor takes
            // over. Keep the sidebar's remembered item and real text edits.
            if (!sidebarMode_)
                MenuCaptureGui::SetNavID(0, ImGuiNavLayer_Main, 0, {});
        }

        void ReleaseCapture()
        {
            if (OwnsCapture()) MenuCaptureGui::ClearActiveID();
        }

        void RestoreSidebarConfirm()
        {
            if (!confirmSaved_) return;
            *MenuCaptureGui::GetKeyData(ImGuiMCP::ImGuiKey_GamepadFaceDown) = savedConfirm_;
            confirmSaved_ = false;
        }

        void RestoreFlags()
        {
            if (navFlagsSaved_) {
                auto* io = MenuCaptureGui::GetIO();
                io->ConfigFlags = (io->ConfigFlags & ~kNavFlags) | savedNavFlags_;
                savedNavFlags_ = 0;
                navFlagsSaved_ = false;
            }
        }

        void ReleaseKeys()
        {
            using namespace ImGuiMCP;
            if (!owner_) return;
            for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
                if (MenuCaptureGui::GetKeyOwner(static_cast<ImGuiKey>(key)) == owner_)
                    MenuCaptureGui::SetKeyOwner(static_cast<ImGuiKey>(key), kNoKeyOwner, 0);
            }
        }

        const void* renderer_ = nullptr;
        Window* page_ = nullptr;
        Window* tree_ = nullptr;
        ID owner_ = 0;
        int pageFrame_ = -1000;
        int savedNavFlags_ = 0;
        bool navFlagsSaved_ = false;
        bool sidebarMode_ = false;
        bool usable_ = false;
        bool pageFocused_ = false;
        bool resume_ = false;
        bool supported_ = false;
        bool keyboard_ = false;
        bool confirmSaved_ = false;
        ImGuiMCP::ImGuiKeyData savedConfirm_{};
    };
}
