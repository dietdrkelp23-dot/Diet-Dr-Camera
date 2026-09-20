#include "imgui.h"
#include "imgui_internal.h"
#include "GamepadNavigation.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Use the production capture class against the real ImGui API. GetIO/GetStyle
// return pointers in cimgui's generated wrapper and references in native ImGui.
namespace ImGuiMCP {
    using ::ImGuiWindow;
    using ::ImGuiID;
    using ::ImGuiContext;
    using ::ImGuiKey;
    using ::ImGuiKeyData;
    using ::ImGuiSizeCallbackData;
    using ::ImFont;
    using ::ImVec2;
    using ::ImVec4;
    using enum ::ImGuiCol_;
    using enum ::ImGuiKey;
    using enum ::ImGuiConfigFlags_;
    using enum ::ImGuiFocusedFlags_;
    using enum ::ImGuiFocusRequestFlags_;
    using enum ::ImGuiPopupFlags_;
    using enum ::ImGuiInputFlags_;
    using enum ::ImGuiNavLayer;
    using enum ::ImGuiWindowFlags_;
    using enum ::ImGuiChildFlags_;
    using enum ::ImGuiCond_;
    using enum ::ImGuiStyleVar_;
    namespace ImGui {
        using namespace ::ImGui;
        inline ::ImGuiIO* GetIO() { return &::ImGui::GetIO(); }
        inline ::ImGuiStyle* GetStyle() { return &::ImGui::GetStyle(); }
        namespace ImFontManger {
            inline void CalcTextSizeA(ImVec2* out, ImFont* font, float size, float maxWidth,
                float wrapWidth, const char* begin, const char* end, const char** remaining) {
                *out = font->CalcTextSizeA(size, maxWidth, wrapWidth, begin, end, remaining);
            }
        }
        namespace ImGuiWindowManager {
            inline float CalcFontSize(ImGuiWindow* window) { return window->CalcFontSize(); }
        }
        namespace ImDrawListManager {
            inline std::vector<std::string> labels;
            struct TextDraw { ImRect bounds; float size; ImFont* font; };
            inline std::vector<TextDraw> textDraws;
            inline std::vector<ImRect> groups;
            inline void _ResetForNewFrame(ImDrawList* draw) { draw->_ResetForNewFrame(); }
            inline void PushTextureID(ImDrawList* draw, ImTextureID id) { draw->PushTextureID(id); }
            inline void PopTextureID(ImDrawList* draw) { draw->PopTextureID(); }
            inline void PushClipRect(ImDrawList* draw, ImVec2 min, ImVec2 max, bool intersect) {
                draw->PushClipRect(min, max, intersect);
            }
            inline void PopClipRect(ImDrawList* draw) { draw->PopClipRect(); }
            inline void AddRectFilled(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
                float rounding, ImDrawFlags flags) {
                if (rounding == 3.0f) groups.emplace_back(min, max);
                draw->AddRectFilled(min, max, color, rounding, flags);
            }
            inline void AddText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 color,
                const char* begin, const char* end, float wrap, const ImVec4* clip) {
                labels.emplace_back(begin);
                textDraws.push_back({ImRect(pos, pos + font->CalcTextSizeA(size, FLT_MAX, 0, begin, end)), size, font});
                draw->AddText(font, size, pos, color, begin, end, wrap, clip);
            }
        }
    }
}
namespace FontAwesome {
    inline ImFont* solid = nullptr;
    inline void PushSolid() { ::ImGui::PushFont(solid); }
    inline void Pop() { ::ImGui::PopFont(); }
}
#include "UI/MenuControllerCapture.h"
#include "UI/MenuControllerHintBar.h"
#include "UI/MenuPopupHintBar.h"
#include "UI/MenuKeyboardInput.h"
#include "UI/ClipboardHintHover.h"

namespace GN = UI::GamepadNavigation;
using DietDrCamera::MenuControllerCapture;
static MenuControllerCapture capture;
static DietDrCamera::MenuHintBarLayout hintLayout;
static DietDrCamera::MenuPopupHintBar popupHintBar;
static int pageTokens[3]{};
static int selected = 0, entries = 0, customActivations = 0, otherActivations = 0;
static bool connected = true, panelOpen = true, settingsOpen = false, focusSettings = false;
static bool keyboardEnabled = false;
static int keyboardCursor = 0;
static DietDrCamera::DirectionalNavigation keyboardNavigation;
static const std::array keyboardItems{
    DietDrCamera::NavigationItem{0, 0, 80, 20, 0, 1},
    DietDrCamera::NavigationItem{90, 0, 170, 20, 0, 2},
    DietDrCamera::NavigationItem{0, 30, 80, 50, 0, 3},
    DietDrCamera::NavigationItem{90, 30, 170, 50, 0, 4}};
static bool lateForeignWindow = false;
static bool popup = false, openPopup = false, rawA = false, rawB = false, rawRight = false;
static int frameworkBacks = 0;
static int frameworkHighlightVertices = 0;
static ImGuiWindow* tree = nullptr;
static ImGuiWindow* content = nullptr;
static ImGuiWindow* footer = nullptr;
static ImGuiID nativeFooterHash = 0;
static bool footerReplaced = false, binding = false, slider = false;
static float panelWidth = 800, panelHeight = 600;
static DietDrCamera::MenuControllerHints::Context hintContext;
static ImGuiID treeIds[3]{}, otherIds[2]{}, folderId = 0;
static ImRect treeRects[3]{}, otherRects[2]{}, folderRect{};
static ImRect ddcFirstTabRect{};
static ImRect ddcTextRect{};
static ImGuiID ddcTextId = 0;
static char ddcText[64] = "initial";
static bool longSidebar = false;
static ImRect bottomPageRect{}, bottomTreeRect{};
static int bottomPageClicks = 0, bottomTreeClicks = 0;
static bool openLocationPopup = false, closeLocationPopup = false;
static ImGuiWindow* locationWindow = nullptr;
static ImGuiWindow* locationList = nullptr;
static ImGuiWindow* locationSettings = nullptr;
static ImRect locationResetRect{}, locationDoneRect{}, locationBottomRect{};
static int locationResetClicks = 0, locationBottomClicks = 0;

static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

static void LoadSymbolFont(float size = 13.0f, const char* textFont = nullptr)
{
    static const ImWchar ranges[]{0xF060, 0xF063, 0};
    // Match FontManager::LoadFonts: an independent primary text face with
    // the solid icons merged in. In particular, spaces must stay spaces.
    auto* atlas = ImGui::GetIO().Fonts;
    ImFontConfig config;
    config.SizePixels = size;
    FontAwesome::solid = textFont ? atlas->AddFontFromFileTTF(textFont, size) : atlas->AddFontDefault(&config);
    Require(FontAwesome::solid != nullptr, "Could not load the symbol font's text face");
    config.MergeMode = true;
    FontAwesome::solid = atlas->AddFontFromFileTTF(SMF_SYMBOL_FONT, size, &config, ranges);
    Require(FontAwesome::solid != nullptr, "Could not load the framework's arrow symbol font");
}

static ImGuiID DrawHash(const ImDrawList* draw)
{
    return ImHashData(draw->IdxBuffer.Data, draw->IdxBuffer.size_in_bytes(),
        ImHashData(draw->VtxBuffer.Data, draw->VtxBuffer.size_in_bytes()));
}

static void DrawFooter()
{
    namespace Hints = DietDrCamera::MenuControllerHints;
    const auto* currentWindow = GImGui->CurrentWindow;
    const auto* focusedWindow = GImGui->NavWindow;
    auto* target = capture.HintBarWindow();
    Require(currentWindow == GImGui->CurrentWindow && focusedWindow == GImGui->NavWindow,
        "Footer lookup did not preserve current and focused windows");
    auto& labels = ImGuiMCP::ImGui::ImDrawListManager::labels;
    labels.clear();
    auto& groups = ImGuiMCP::ImGui::ImDrawListManager::groups;
    auto& textDraws = ImGuiMCP::ImGui::ImDrawListManager::textDraws;
    groups.clear(); textDraws.clear();
    hintContext.mode = binding ? Hints::Mode::Binding :
        ImGui::GetIO().WantTextInput ? Hints::Mode::TextInput :
        slider ? Hints::Mode::Slider : popup ? Hints::Mode::Popup :
        capture.IsSidebar() ? Hints::Mode::Sidebar : Hints::Mode::Page;
    hintContext.tabs = !popup;
    if (auto* popupFooter = popupHintBar.WindowForFrame(); popupFooter && capture.FocusedPanelWindow()) {
        const auto active = ImGui::GetActiveID();
        const auto styleHash = ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle));
        footer = popupFooter;
        footerReplaced = DietDrCamera::DrawMenuControllerHintBar(footer, hintContext);
        Require(footerReplaced && footer->ParentWindow == locationWindow,
            "Location hints were drawn outside their popup");
        Require(currentWindow == GImGui->CurrentWindow && focusedWindow == GImGui->NavWindow &&
            active == ImGui::GetActiveID(), "Location hints changed input focus");
        Require(styleHash == ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle)),
            "Location hints changed shared style");
        Require(footer->Pos.y >= locationResetRect.Max.y && footer->Pos.y >= locationDoneRect.Max.y &&
            footer->Pos.y + footer->Size.y <= locationWindow->Pos.y + locationWindow->Size.y - locationWindow->WindowPadding.y,
            "Location hints cover the action buttons or extend the popup");
        Require(footer->CalcFontSize() == content->ParentWindow->CalcFontSize(),
            "Location hints inherited a heading or host-pane font scale");
        Require(locationList->Pos.y + locationList->Size.y <= locationResetRect.Min.y &&
            locationSettings->Pos.y + locationSettings->Size.y <= locationDoneRect.Min.y,
            "Location panes overlap their action buttons");
        return;
    }
    if (!target && keyboardEnabled) {
        const auto styleHash = ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle));
        const auto active = ImGui::GetActiveID();
        auto* panel = capture.FocusedPanelWindow();
        footer = DietDrCamera::DrawMenuKeyboardHintBar(panel, hintContext);
        footerReplaced = footer != nullptr;
        Require(currentWindow == GImGui->CurrentWindow && focusedWindow == GImGui->NavWindow &&
            active == ImGui::GetActiveID(), "Keyboard hints changed input focus");
        Require(styleHash == ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle)),
            "Keyboard hints changed shared style");
        if (footer) {
            Require(footer->ParentWindow == panel && footer->Pos.x == tree->Pos.x &&
                footer->Pos.x + footer->Size.x == content->Pos.x + content->Size.x,
                "Keyboard hints do not span both panes inside the panel");
            Require(footer->Pos.y >= tree->Pos.y + tree->Size.y &&
                footer->Pos.y >= content->Pos.y + content->Size.y &&
                footer->Pos.y + footer->Size.y <= panel->Pos.y + panel->Size.y - panel->WindowPadding.y,
                "Keyboard hints extend the menu or overlap a pane");
            // Keep the background flush with its child-window rectangle.
            const auto& background = footer->DrawList->CmdBuffer.front();
            Require(background.ElemCount >= 6 &&
                background.ClipRect.x <= footer->Pos.x && background.ClipRect.y <= footer->Pos.y &&
                background.ClipRect.z >= footer->Pos.x + footer->Size.x &&
                background.ClipRect.w >= footer->Pos.y + footer->Size.y,
                "Keyboard footer clipping leaves a transparent gap at the panel edge");
            Require(!groups.empty(), "Keyboard hints have no visible controls");
            for (std::size_t begin = 0; begin < groups.size();) {
                auto end = begin + 1;
                while (end < groups.size() && std::abs(groups[end].Min.y - groups[begin].Min.y) < 0.01f) ++end;
                Require(std::abs((groups[begin].Min.x + groups[end - 1].Max.x) * 0.5f -
                    (panel->Pos.x + panel->Size.x * 0.5f)) < 0.01f, "A keyboard hint row is not centered");
                begin = end;
            }
        }
        return;
    }
    if (!target) return;
    Require(target == footer, "DDC chose a window other than the framework footer");
    const auto active = ImGui::GetActiveID(), navId = ImGui::GetFocusID();
    auto* navWindow = GImGui->NavWindow;
    const auto config = ImGui::GetIO().ConfigFlags;
    const auto styleHash = ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle));
    std::vector<ImGuiID> owners;
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        owners.push_back(ImGui::GetKeyOwner(static_cast<ImGuiKey>(key)));
    const ImVec2 size = target->Size;
    const auto flags = target->DrawList->Flags;
    footerReplaced = DietDrCamera::DrawMenuControllerHintBar(target, hintContext);
    Require(footerReplaced, "DDC footer drawing failed");
    Require(active == ImGui::GetActiveID() && navId == ImGui::GetFocusID() && navWindow == GImGui->NavWindow,
        "Footer changed controller focus or the active widget");
    Require(config == ImGui::GetIO().ConfigFlags && styleHash == ImHashData(&ImGui::GetStyle(), sizeof(ImGuiStyle)),
        "Footer changed shared navigation settings or style");
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        Require(owners[key - ImGuiKey_GamepadStart] == ImGui::GetKeyOwner(static_cast<ImGuiKey>(key)),
            "Footer changed controller button ownership");
    Require(target->Size.x == size.x && target->Size.y == size.y && target->ScrollMax.y == 0 &&
        target->DrawList->Flags == flags, "Footer changed layout, scrolling or drawing flags");
    Require(DrawHash(target->DrawList) != nativeFooterHash, "Native hints were not replaced");
    Require(groups.size() * 2 == textDraws.size(), "Each binding/action pair must have its own group");
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        for (std::size_t label = i * 2; label < i * 2 + 2; ++label) {
            const auto& bounds = textDraws[label].bounds;
            Require(bounds.Min.x >= group.Min.x && bounds.Max.x <= group.Max.x &&
                bounds.Min.y >= group.Min.y && bounds.Max.y <= group.Max.y,
                "Binding or action text escaped its shared group");
        }
        Require(!textDraws[i * 2].bounds.Overlaps(textDraws[i * 2 + 1].bounds),
            "A binding overlaps its explanation");
        if (i > 0 && std::abs(groups[i - 1].Min.y - group.Min.y) < 0.1f) {
            const float gap = group.Min.x - groups[i - 1].Max.x;
            Require(gap >= 7.9f && gap <= std::max(8.0f, target->CalcFontSize() * 0.35f) + 0.1f,
                "Visible hints have excessive or insufficient separation");
        }
    }
    // All glyphs and keycaps must fit the reserved row, including long custom
    // keyboard bindings and a small panel. Allow one pixel for edge antialiasing.
    for (const auto& vertex : target->DrawList->VtxBuffer) {
        if (vertex.pos.x < target->Pos.x - 1 || vertex.pos.x > target->Pos.x + size.x + 1 ||
            vertex.pos.y < target->Pos.y - 1 || vertex.pos.y > target->Pos.y + size.y + 1) {
            std::cerr << "Footer vertex " << vertex.pos.x << ',' << vertex.pos.y << " outside " <<
                target->Pos.x << ',' << target->Pos.y << " size " << size.x << ',' << size.y << '\n';
            Require(false, "A footer glyph escaped the existing footer space");
        }
    }
}

static bool HasHint(const char* key, const char* action)
{
    const auto& labels = ImGuiMCP::ImGui::ImDrawListManager::labels;
    for (std::size_t i = 0; i + 1 < labels.size(); i += 2)
        if (labels[i] == key && labels[i + 1] == action) return true;
    return false;
}

static void CheckCenteredHintLayout()
{
    namespace Hints = DietDrCamera::MenuControllerHints;
    namespace Draw = ImGuiMCP::ImGui::ImDrawListManager;
    struct Placement {
        bool visible = false;
        ImRect group;
        float keySize = 0, actionSize = 0;
    };
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    const auto size = footer->Size;
    const auto clip = footer->ClipRect;
    auto* originalDraw = footer->DrawList;
    auto* originalWindow = GImGui->CurrentWindow;
    GImGui->CurrentWindow = footer;
    footer->DrawList = &scratch;
    const auto captureLayout = [&](const Hints::Context& context) {
        Draw::groups.clear(); Draw::textDraws.clear(); Draw::labels.clear();
        Require(DietDrCamera::DrawMenuControllerHintBar(footer, context), "Centered hint layout failed to render");
        auto hints = Hints::Build(context);
        std::ranges::sort(hints, {}, &Hints::Hint::slot);
        Require(Draw::groups.size() == hints.size() && Draw::textDraws.size() == hints.size() * 2,
            "Hidden slots drew a hint or visible slots disappeared");
        std::array<Placement, Hints::SlotCount> result{};
        for (std::size_t i = 0; i < hints.size(); ++i) {
            const auto& group = Draw::groups[i];
            const auto& key = Draw::textDraws[i * 2];
            const auto& action = Draw::textDraws[i * 2 + 1];
            Require(group.Min.x >= footer->Pos.x && group.Max.x <= footer->Pos.x + footer->Size.x &&
                group.Min.y >= footer->Pos.y && group.Max.y <= footer->Pos.y + footer->Size.y,
                "A hint escaped the existing footer");
            Require(group.Contains(key.bounds) && group.Contains(action.bounds) && !key.bounds.Overlaps(action.bounds),
                "A hint clipped or overlapped its labels");
            const float padding = key.bounds.Min.x - group.Min.x;
            const bool inlinePair = std::abs(key.bounds.Min.y - action.bounds.Min.y) < 0.01f;
            const float contentRight = std::max(key.bounds.Max.x, action.bounds.Max.x);
            Require(std::abs(group.Max.x - contentRight - padding) < 0.01f,
                "The visible hint box stretched into unused reserved space");
            if (inlinePair) Require(std::abs(action.bounds.Min.x - key.bounds.Max.x -
                std::max(8.0f, footer->CalcFontSize() * 0.4f)) < 0.01f,
                "A short binding retained padding for a longer key name");
            Require(Draw::labels[i * 2] == hints[i].keys && Draw::labels[i * 2 + 1] == hints[i].action,
                "A hint was drawn in another action's slot");
            if (i > 0 && std::abs(Draw::groups[i - 1].Min.y - group.Min.y) < 0.01f) {
                const float gap = group.Min.x - Draw::groups[i - 1].Max.x;
                Require(gap >= 7.9f && gap <= std::max(8.0f, footer->CalcFontSize() * 0.35f) + 0.1f,
                    "A short or hidden hint left unused space before the next box");
                if (i > 1 && std::abs(Draw::groups[i - 2].Min.y - group.Min.y) < 0.01f)
                    Require(std::abs(gap - (Draw::groups[i - 1].Min.x - Draw::groups[i - 2].Max.x)) < 0.01f,
                        "Visible hint boxes have uneven gaps");
            }
            result[static_cast<std::size_t>(hints[i].slot)] =
                {true, group, key.size, action.size};
        }
        for (std::size_t begin = 0; begin < Draw::groups.size();) {
            auto end = begin + 1;
            while (end < Draw::groups.size() &&
                std::abs(Draw::groups[end].Min.y - Draw::groups[begin].Min.y) < 0.01f) ++end;
            const float leftMargin = Draw::groups[begin].Min.x - footer->Pos.x;
            const float rightMargin = footer->Pos.x + footer->Size.x - Draw::groups[end - 1].Max.x;
            Require(std::abs(leftMargin - rightMargin) < 0.01f,
                "The visible controller hints are not centered in the footer");
            begin = end;
        }
        return result;
    };
    for (const float width : {480.0f, 800.0f, 1680.0f, 3072.0f}) {
        footer->Size.x = width;
        footer->ClipRect.Max.x = footer->Pos.x + width;
        for (const int device : {0, 1, 2}) {
            Hints::Context full;
            full.playStation = device == 1;
            full.keyboard = device == 2;
            full.tabs = full.clipboardHasContents = true;
            full.copy = "Key Backspace"; full.paste = "Key 0xA2";
            full.quickTune = Hints::BindingName("R Stick", true, full.playStation);
            full.clipboardSubject = Hints::ClipboardLabels::Entry;
            const auto baseline = captureLayout(full);
            const auto check = [&](const Hints::Context& context) {
                const auto current = captureLayout(context);
                for (std::size_t i = 0; i < current.size(); ++i) {
                    const auto& now = current[i];
                    const auto& before = baseline[i];
                    if (!now.visible) continue;
                    Require(before.visible && std::abs(now.group.Min.y - before.group.Min.y) < 0.01f &&
                        now.keySize == before.keySize && now.actionSize == before.actionSize,
                        "Hover/context changes wrapped or resized a controller hint");
                }
            };
            for (const auto* subject : Hints::ClipboardLabels::All) {
                auto context = full;
                context.clipboardSubject = subject;
                check(context);
                context.clipboardHasContents = false;
                check(context);
            }
            for (const auto mode : {Hints::Mode::Sidebar, Hints::Mode::Page, Hints::Mode::Popup,
                     Hints::Mode::Slider, Hints::Mode::Binding, Hints::Mode::TextInput}) {
                auto context = full;
                context.mode = mode;
                context.tabs = false;
                context.clipboardSubject = {};
                check(context);
            }
        }
    }
    footer->Size = size;
    footer->ClipRect = clip;
    footer->DrawList = originalDraw;
    GImGui->CurrentWindow = originalWindow;
}

static void CheckClipboardHintTargets()
{
    using DietDrCamera::NavigationItem;
    using DietDrCamera::ClipboardHintHover;
    namespace Hints = DietDrCamera::MenuControllerHints;
    // Reset and slider are valid navigation items, but publish no clipboard
    // target. A held/idle mouse over an entry must not advertise it on either.
    const NavigationItem entry{10, 10, 110, 30, 0, 101};
    const NavigationItem reset{120, 10, 190, 30, 0, 102};
    const NavigationItem sliderItem{120, 40, 390, 60, 0, 103};
    const NavigationItem transition{10, 70, 180, 90, 0, 104};
    const std::array live{entry, reset, sliderItem, transition};
    ClipboardHintHover hover;
    bool clipboardHasContents = false;
    const auto actions = [&](int frame, const NavigationItem& cursor) {
        Hints::Context context;
        context.copy = "X"; context.paste = "Y";
        context.clipboardSubject = hover.Resolve(frame, cursor, live, 0);
        context.clipboardHasContents = clipboardHasContents;
        return Hints::Build(context);
    };
    const auto has = [](const auto& hints, const char* action) {
        return std::any_of(hints.begin(), hints.end(), [&](const auto& hint) { return hint.action == action; });
    };
    const auto noClipboard = [](const auto& hints) {
        return std::none_of(hints.begin(), hints.end(), [](const auto& hint) {
            return hint.action.starts_with("Copy ") || hint.action.starts_with("Paste ");
        });
    };
    Require(noClipboard(actions(10, reset)), "Bound keys alone advertised clipboard actions on Reset");
    hover.Publish(10, entry, entry, true, "Entry", live);
    Require(has(actions(10, entry), "Copy Entry") && !has(actions(10, entry), "Paste Entry"),
        "An empty clipboard advertised Paste or hid Copy");
    clipboardHasContents = true;
    Require(has(actions(10, entry), "Copy Entry") && has(actions(10, entry), "Paste Entry"),
        "A current copyable entry lost its shortcuts");
    Require(noClipboard(actions(10, reset)) && noClipboard(actions(10, sliderItem)),
        "Moving from an entry to Reset/slider retained clipboard hints");
    Require(noClipboard(actions(11, entry)), "Clipboard hints reused a previous frame's target");
    hover.Publish(11, entry, sliderItem, false, "Entry", live);
    Require(noClipboard(actions(11, sliderItem)), "Idle mouse hover published hints on a controller slider");
    auto group = reset; group.x0 = 0; group.y1 = 100; group.x1 = 400;
    hover.Publish(12, group, reset, true, "Entry", live);
    Require(noClipboard(actions(12, reset)), "A group inheriting Reset's active ID advertised a clipboard target");
    hover.Publish(13, transition, transition, true, "Transition Override", live);
    hover.Publish(13, entry, transition, false, "Entry", live);
    Require(has(actions(13, transition), "Copy Transition Override") &&
        has(actions(13, transition), "Paste Transition Override") && !has(actions(13, transition), "Copy Entry"),
        "Override hints used the entry label or were overwritten by idle mouse hover");
    clipboardHasContents = false;
    Require(has(actions(13, transition), "Copy Transition Override") &&
        !has(actions(13, transition), "Paste Transition Override"),
        "Clearing the clipboard retained an override Paste hint");
    Require(hover.Resolve(13, transition, live, 2).empty(), "Underlying page hints leaked into a new popup");
    Require(hover.Resolve(13, transition, {}, 0).empty(), "Removed/disabled targets retained clipboard hints");
    auto clipped = transition; clipped.wy0 = 500; clipped.wy1 = 600;
    const std::array clippedLive{clipped};
    Require(hover.Resolve(13, transition, clippedLive, 0) == "Transition Override",
        "A selected row lost its hints while scrolling into view");
    auto anonymous = entry; anonymous.id = 0;
    hover.Publish(14, anonymous, anonymous, true, "Entry", live);
    Require(noClipboard(actions(14, anonymous)), "An ID-less group advertised clipboard actions");
    auto scrolled = entry; scrolled.y0 -= 60; scrolled.y1 -= 60;
    const std::array scrolledLive{scrolled};
    hover.Publish(15, scrolled, entry, true, "Entry", scrolledLive);
    Require(hover.Resolve(15, entry, scrolledLive, 0) == "Entry",
        "A current target was rejected because the cursor still had its pre-scroll rectangle");
}

static void CheckScrollingClipboardHints()
{
    using DietDrCamera::NavigationItem;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {600, 400}; io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    DietDrCamera::ClipboardHintHover hover;
    std::array<NavigationItem, 15> previous{};
    bool sawClippedTarget = false, sawScrolledGeometry = false;
    for (const int selectedRow : {0, 0, 3, 3, 6, 6, 9, 9, 12, 12, 3, 3, 1, 1}) {
        const auto cursor = previous[selectedRow];
        std::array<NavigationItem, 15> live{};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({300, 250}, ImGuiCond_Always);
        ImGui::Begin("Scrolling clipboard hints");
        ImGui::BeginChild("School entries", {200, 100}, true);
        ImGui::SetWindowFontScale(1.4f);
        for (int row = 0; row < static_cast<int>(live.size()); ++row) {
            if (row % 3 == 0) ImGui::TextUnformatted("School");
            ImGui::PushID(row);
            ImGui::Selectable(row % 3 == 0 ? "Concentration" : row % 3 == 1 ? "Fire & Forget" : "Ritual");
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            live[row] = {min.x, min.y, max.x, max.y, 0, ImGui::GetItemID(),
                pos.y, pos.y + size.y, pos.x, pos.x + size.x};
            if (row == selectedRow && cursor.id != 0) {
                sawClippedTarget |= !live[row].Visible();
                sawScrolledGeometry |= std::abs(live[row].y0 - cursor.y0) >= 2;
                if (min.y < pos.y) ImGui::SetScrollHereY(0);
                else if (max.y > pos.y + size.y) ImGui::SetScrollHereY(1);
                hover.Publish(ImGui::GetFrameCount(), live[row], cursor, true, "Entry",
                    std::span<const NavigationItem>(live.data(), row + 1));
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        if (cursor.id != 0) Require(hover.Resolve(ImGui::GetFrameCount(), cursor, live, 0) == "Entry",
            "School-list clipboard hints flickered during native ImGui scroll/reveal");
        previous = live;
    }
    Require(sawClippedTarget && sawScrolledGeometry, "The scrolling fixture did not exercise offscreen rows and reflow");
    ImGui::DestroyContext();
}

static void Hook(ImGuiContext*, ImGuiContextHook* hook)
{
    if (hook->Type == ImGuiContextHookType_NewFramePre)
        capture.BeforeNewFrame(connected, panelOpen, rawRight, keyboardEnabled);
    else if (hook->Type == ImGuiContextHookType_NewFramePost) {
        capture.AfterNewFrame();
        hintLayout.BeforePanel(panelOpen);
    }
    else if (hook->Type == ImGuiContextHookType_EndFramePre) {
        DrawFooter(); hintLayout.EndFrame(); capture.EndFrame();
    }
}

static void Leaf(const char* name, int index)
{
    ImGui::TreeNodeEx(name, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
    treeIds[index] = ImGui::GetItemID();
    treeRects[index] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    const bool pad = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) && ImGui::IsItemFocused();
    // This is the framework's RenderNode selection contract.
    if ((ImGui::IsItemClicked() || pad) && !ImGui::IsItemToggledOpen()) {
        selected = index + 1;
        if (pad) GN::RequestFocus(GN::Area::PageContent);
    }
}

static void RenderLocationPopupFixture()
{
    if (openLocationPopup) { ImGui::OpenPopup("Location override checks"); openLocationPopup = false; }
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(display * 0.5f, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({display.x * 0.8f, display.y * 0.93f});
    const float hostScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(1.4f);
    if (ImGui::BeginPopupModal("Location override checks", nullptr, ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        popup = true;
        locationWindow = ImGui::GetCurrentWindow();
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted("Location Override");
        ImGui::TextUnformatted("Bind current location");
        const auto avail = ImGui::GetContentRegionAvail();
        const float footerHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y +
            DietDrCamera::MenuPopupHintBar::ReservedHeight();
        const float bodyHeight = std::max(4.0f, avail.y - footerHeight);
        ImGui::BeginChild("Location list", {avail.x * 0.26f, bodyHeight}, ImGuiChildFlags_Border);
        locationList = ImGui::GetCurrentWindow();
        ImGui::Selectable("Whiterun"); ImGui::Selectable("Riverwood");
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("Location settings", {0, bodyHeight}, ImGuiChildFlags_Border);
        locationSettings = ImGui::GetCurrentWindow();
        ImGui::TextUnformatted("Location settings");
        ImGui::Dummy({1, 2200});
        if (ImGui::Button("Last location control")) ++locationBottomClicks;
        locationBottomRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::EndChild();
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Reset All", {width, 0})) ++locationResetClicks;
        locationResetRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::SameLine();
        const bool done = ImGui::Button("Done", {width, 0});
        locationDoneRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        if (done || closeLocationPopup) {
            ImGui::CloseCurrentPopup(); popup = false; closeLocationPopup = false;
        }
        popupHintBar.Create(2);
        ImGui::EndPopup();
    }
    ImGui::SetWindowFontScale(hostScale);
}

static void Frame()
{
    footer = nullptr;
    footerReplaced = false;
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    ImGui::NewFrame();
    if (panelOpen) {
        ImGui::SetNextWindowSize({panelWidth, panelHeight});
        ImGui::Begin("#MCPMainWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse);
        GN::BeginFrame(selected != 0, settingsOpen);
        GN::PushFocusStyle();
        if (ImGui::BeginMenuBar()) { ImGui::TextUnformatted("Mod Control Panel"); ImGui::EndMenuBar(); }
        ImGui::BeginChild("TreeView2", {ImGui::GetContentRegionAvail().x * 0.3f, 50});
        ImGui::Dummy({1, 1}); ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("SKSEModControlPanelModMenuHeader", {0, 41});
        ImGui::TextUnformatted("Selected section"); ImGui::EndChild();
        const float hintHeight = GN::GetHintBarHeight();
        const float bodyHeight = hintHeight > 0 ? -(hintHeight + ImGui::GetStyle().ItemSpacing.y) : -FLT_MIN;
        ImGui::BeginChild("SKSEModControlPanelTreeView", {ImGui::GetContentRegionAvail().x * 0.3f, bodyHeight}, ImGuiChildFlags_Border);
        tree = ImGui::GetCurrentWindow();
        GN::BeginArea(GN::Area::PageTree);
        const bool folder = ImGui::TreeNodeEx("Diet Dr Camera", ImGuiTreeNodeFlags_DefaultOpen);
        folderId = ImGui::GetItemID();
        folderRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        if (folder) { Leaf("DDC1", 0); Leaf("DDC2", 1); ImGui::TreePop(); }
        Leaf("Other mod", 2);
        if (longSidebar) {
            ImGui::Dummy({1, 1000});
            if (ImGui::Button("Bottom sidebar control")) ++bottomTreeClicks;
            bottomTreeRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        }
        GN::EndArea();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("SKSEModControlPanelMenuNode", {0, bodyHeight}, ImGuiChildFlags_Border);
        content = ImGui::GetCurrentWindow();
        GN::BeginArea(GN::Area::PageContent);
        if (selected == 1 || selected == 2) {
            const bool entering = capture.BeginPage(&pageTokens[selected - 1], connected, keyboardEnabled);
            hintLayout.BeginPage();
            if (entering) ++entries;
            const auto keyboard = DietDrCamera::MenuKeyboardInput::Read(keyboardEnabled && !binding &&
                (ImGui::GetActiveID() == 0 || capture.OwnsCapture()));
            if (rawRight && capture.IsSidebar()) capture.SetSidebar(false);
            else if (capture.IsUsable() && !capture.IsSidebar() && keyboard.Any()) {
                const int dx = static_cast<int>(keyboard.right) - static_cast<int>(keyboard.left);
                const int dy = static_cast<int>(keyboard.down) - static_cast<int>(keyboard.up);
                const int next = keyboardNavigation.Move(keyboardItems, keyboardCursor, dx, dy, 0);
                if (next >= 0) keyboardCursor = next;
                else if (dx < 0 && !popup) capture.SetSidebar(true);
            }
            if ((rawA || keyboard.confirm) && !entering && capture.IsUsable() && !capture.IsSidebar()) ++customActivations;
            if (rawB && capture.IsUsable() && !popup) capture.SetSidebar(true);
            if (ImGui::BeginTabBar("DDC tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
                for (const char* tab : {"Sheathed", "Melee", "Archery", "Magic", "Staves", "Blocking", "Transformations"}) {
                    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                    const bool open = ImGui::BeginTabItem(tab);
                    ImGui::PopItemFlag();
                    if (std::string_view(tab) == "Sheathed")
                        ddcFirstTabRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                    if (open) ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            ImGui::Button("DDC custom control");
            ImGui::InputText("DDC text edit", ddcText, sizeof(ddcText), ImGuiInputTextFlags_AutoSelectAll);
            ddcTextId = ImGui::GetItemID();
            ddcTextRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::PopItemFlag();
            ImGui::Dummy({1, 1200});
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button("Bottom page control")) ++bottomPageClicks;
            bottomPageRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::PopItemFlag();
            if (openPopup) { ImGui::OpenPopup("DDC popup"); openPopup = false; }
            if (ImGui::BeginPopupModal("DDC popup")) {
                popup = true;
                ImGui::TextUnformatted("DDC confirmation");
                if (rawB) { ImGui::CloseCurrentPopup(); popup = false; }
                ImGui::EndPopup();
            }
            RenderLocationPopupFixture();
            capture.EndPage();
        } else if (selected == 3) {
            for (int i = 0; i < 2; ++i) {
                if (ImGui::Button(i == 0 ? "Other 1" : "Other 2")) ++otherActivations;
                otherIds[i] = ImGui::GetItemID();
                otherRects[i] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            }
        }
        GN::EndArea();
        ImGui::EndChild();
        GN::RenderHintBar(selected != 0);
        for (auto* child : ImGui::GetCurrentWindow()->DC.ChildWindows) {
            if (child->LastFrameActive == ImGui::GetFrameCount() &&
                std::strstr(child->Name, "/##MCPGamepadHints_")) {
                footer = child;
                nativeFooterHash = DrawHash(footer->DrawList);
            }
        }
        const int highlightStart = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
        GN::RenderFocusedItemHighlight();
        frameworkHighlightVertices = ImGui::GetForegroundDrawList()->VtxBuffer.Size - highlightStart;
        GN::PopFocusStyle();
        GN::EndFrame();
        ImGui::End();
    }
    if (settingsOpen) {
        if (focusSettings) { ImGui::SetNextWindowFocus(); focusSettings = false; }
        ImGui::Begin("Framework settings");
        ImGui::Button("Settings control");
        ImGui::End();
    }
    if (lateForeignWindow) {
        ImGui::SetNextWindowFocus();
        ImGui::Begin("Late framework window");
        ImGui::Button("Foreign control");
        ImGui::End();
    }
    ImGui::Render();
    if (footer && !footerReplaced)
        Require(DrawHash(footer->DrawList) == nativeFooterHash, "DDC changed another page's native hints");
    if (capture.OwnsCapture() && capture.IsSidebar() && !popup && !settingsOpen)
        Require(GImGui->NavWindow == tree, "DDC left sidebar mode focused in the right pane for an input frame");
    Require((io.ConfigFlags & 3) == 3, "DDC left shared navigation flags disabled");
    Require(((io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0) == connected, "DDC changed controller availability");
    if (panelOpen) {
        Require((tree->SetWindowSizeAllowFlags & ImGuiCond_Always) &&
            (content->SetWindowSizeAllowFlags & ImGuiCond_Always), "DDC left pane sizing disabled");
        Require(content->ParentWindow->Size.x == panelWidth && content->ParentWindow->Size.y == panelHeight,
            "Footer layout changed the outer window size");
    }
    rawA = rawB = rawRight = false;
}

static void Key(ImGuiKey key)
{
    GN::NotifyInputDevice(key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown ?
        RE::INPUT_DEVICE::kGamepad : RE::INPUT_DEVICE::kKeyboard);
    rawA = key == ImGuiKey_GamepadFaceDown;
    rawB = key == ImGuiKey_GamepadFaceRight;
    rawRight = key == ImGuiKey_GamepadDpadRight || (keyboardEnabled && key == ImGuiKey_RightArrow);
    // Match the framework's raw Back gate, which runs BEFORE NewFrame and
    // BEFORE plugin input callbacks. This must not discard a captured DDC page.
    if (rawB && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ++frameworkBacks;
        selected = 0;
        GN::NotifyPageClosed();
    }
    ImGui::GetIO().AddKeyEvent(key, true);
    Frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    Frame(); Frame();
}

static void FocusTree(ImGuiID id, ImRect rect)
{
    capture.SetSidebar(true);
    ImGui::FocusWindow(tree);
    ImGui::SetNavID(id, ImGuiNavLayer_Main, tree->NavRootFocusScopeId, ImGui::WindowRectAbsToRel(tree, rect));
    Frame(); Frame();
}

static void Click(ImRect rect, bool hold = false)
{
    GN::NotifyInputDevice(RE::INPUT_DEVICE::kMouse);
    ImGui::GetIO().AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y);
    Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    Frame();
    if (!hold) { ImGui::GetIO().AddMouseButtonEvent(0, false); Frame(); Frame(); }
}

static void CheckFixedFooterLayout()
{
    // Compare complete pane/footer geometry on the first frame of every
    // input-device switch, and after both shrinking and growing the panel.
    const auto geometry = [] {
        Require(footer && footerReplaced, "A device switch or resize lost the footer");
        Require(!content->ParentWindow->ScrollbarX && !content->ParentWindow->ScrollbarY,
            "Footer layout introduced a scrollbar on the main window");
        return std::array{tree->Pos.x, tree->Pos.y, tree->Size.x, tree->Size.y,
            content->Pos.x, content->Pos.y, content->Size.x, content->Size.y,
            footer->Pos.x, footer->Pos.y, footer->Size.x, footer->Size.y};
    };
    const auto savedContext = hintContext;
    const auto savedStyle = ImGui::GetStyle();
    ImGui::GetStyle().WindowBorderSize = ImGui::GetStyle().ChildBorderSize = 3;
    ImGui::GetStyle().FramePadding = {10, 5};
    connected = true;
    for (const auto size : {ImVec2(800, 600), ImVec2(620, 440), ImVec2(900, 650), ImVec2(800, 600)}) {
        panelWidth = size.x; panelHeight = size.y;
        hintContext.keyboard = true;
        GN::NotifyInputDevice(RE::INPUT_DEVICE::kKeyboard);
        Frame();
        const auto expected = geometry();
        for (const bool keyboard : {false, true, false, true}) {
            hintContext.keyboard = keyboard;
            GN::NotifyInputDevice(keyboard ? RE::INPUT_DEVICE::kKeyboard : RE::INPUT_DEVICE::kGamepad);
            Frame();
            Require(geometry() == expected, "Keyboard/controller switching moved or resized a pane or footer");
        }
    }

    // Both native scrollbars must still reveal and allow clicks on their
    // last controls; moving the hints over full-height panes would fail this.
    longSidebar = true;
    Frame(); Frame();
    ImGui::SetScrollY(tree, tree->ScrollMax.y);
    ImGui::SetScrollY(content, content->ScrollMax.y);
    Frame(); Frame();
    Require(tree->InnerClipRect.Contains(bottomTreeRect) && content->InnerClipRect.Contains(bottomPageRect),
        "The footer hides a pane's last control at maximum scroll");
    Click(bottomTreeRect); Click(bottomPageRect);
    Require(bottomTreeClicks == 1 && bottomPageClicks == 1, "Bottom controls became unclickable");
    longSidebar = false;
    ImGui::SetScrollY(tree, 0); ImGui::SetScrollY(content, 0);
    Frame(); Frame();

    openPopup = true; Frame(); Frame();
    Require(popup && footerReplaced && std::strstr(GImGui->NavWindow->Name, "DDC popup"),
        "The internal keyboard footer stole popup focus");
    rawB = true; Frame(); Frame();

    selected = 3; Frame(); Frame();
    Require(!footerReplaced && content->Pos.y + content->Size.y == content->ParentWindow->ContentRegionRect.Max.y,
        "DDC's reserved footer space leaked into another mod");
    selected = 1; Frame(); Frame();
    Require(footerReplaced, "The internal keyboard footer did not return with DDC");
    hintContext = savedContext;
    ImGui::GetStyle() = savedStyle;
    connected = false;
    Frame(); Frame();
}

static void CheckLocationPopupHints()
{
    const auto savedContext = hintContext;
    connected = true;
    openLocationPopup = true;
    Frame(); Frame();
    Require(popupHintBar.WindowForFrame() == footer && footerReplaced,
        "Opening location overrides left hints behind the modal");
    const auto size = locationWindow->Size;
    const auto pos = locationWindow->Pos;
    const auto footerSize = footer->Size, footerPos = footer->Pos;
    for (const bool keyboard : {false, true, false, true}) {
        hintContext.keyboard = keyboard;
        hintContext.clipboardSubject = "Entry";
        hintContext.clipboardHasContents = true;
        GN::NotifyInputDevice(keyboard ? RE::INPUT_DEVICE::kKeyboard : RE::INPUT_DEVICE::kGamepad);
        Frame();
        Require(footerReplaced && footer == popupHintBar.WindowForFrame(), "Location hints vanished on a device switch");
        Require(locationWindow->Size == size && locationWindow->Pos == pos &&
            footer->Size == footerSize && footer->Pos == footerPos,
            "Changing input devices changed location popup/footer bounds");
        Require(HasHint(keyboard ? DietDrCamera::MenuControllerHints::ArrowSymbols : "D-pad", "Move cursor") &&
            HasHint(keyboard ? "Enter" : "A", "Select / edit") && HasHint("Key C", "Copy Entry") &&
            HasHint("Key V", "Paste Entry") && !HasHint("Key Q", "Open Quick Tune"),
            "Location popup hints omitted active controls or included the main page's Quick Tune action");
        if (!keyboard) Require(HasHint("B", "Close popup"), "Location controller hints omitted closing the popup");
        hintContext.clipboardHasContents = false; Frame();
        Require(!HasHint("Key V", "Paste Entry"), "Location hints advertised Paste with an empty clipboard");
        hintContext.clipboardSubject = {}; Frame();
        Require(!HasHint("Key C", "Copy Entry"), "Location hints retained a stale copy target");
    }
    slider = true; Frame();
    Require(HasHint("Enter", "Keep changes"), "Location hints did not follow slider editing");
    slider = false;
    ImGui::SetScrollY(locationSettings, locationSettings->ScrollMax.y);
    Frame(); Frame();
    Require(locationSettings->InnerClipRect.Contains(locationBottomRect), "Location hints hide the last setting");
    Click(locationBottomRect); Click(locationResetRect);
    Require(locationBottomClicks == 1 && locationResetClicks == 1, "Location footer blocked settings or Reset All");
    Click(locationDoneRect); Frame();
    Require(!popup && !popupHintBar.WindowForFrame() && footerReplaced && footer->ParentWindow == content->ParentWindow,
        "Closing location overrides did not restore the main footer");
    hintContext = savedContext;
    connected = false;
    Frame(); Frame();
}

int main() try
{
    CheckClipboardHintTargets();
    CheckScrollingClipboardHints();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1000, 800}; io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    LoadSymbolFont();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (auto type : {ImGuiContextHookType_NewFramePre, ImGuiContextHookType_NewFramePost, ImGuiContextHookType_EndFramePre}) {
        ImGuiContextHook hook{}; hook.Type = type; hook.Callback = Hook;
        ImGui::AddContextHook(GImGui, &hook);
    }
    GN::NotifyInputDevice(RE::INPUT_DEVICE::kGamepad);
    Frame(); Frame(); Frame();
    Require(footer && !footerReplaced, "DDC replaced the empty framework page's hints");
    Key(ImGuiKey_GamepadDpadDown);
    Key(ImGuiKey_GamepadFaceDown);
    Require(selected == 1 && entries == 1 && capture.IsSidebar(), "Selecting DDC automatically entered its controls");
    Require(ImGui::GetFocusID() == treeIds[0], "Selecting DDC did not keep focus in the section list");
    Require(customActivations == 0, "Section-selection A also activated a DDC control");
    Require(capture.OwnsCapture(), "DDC did not capture raw Back");
    Require(footerReplaced && HasHint("D-pad Right", "Enter right pane") && !HasHint("RB", "Enter right pane"),
        "DDC sidebar did not show its actual entry control");
    Key(ImGuiKey_GamepadDpadRight);
    Require(!capture.IsSidebar(), "Right did not enter DDC's controls");
    Require(footerReplaced && HasHint("B", "Back to sections") && HasHint("LB / RB", "Switch tabs"),
        "DDC page hints do not match section return and bumper tabs");
    hintContext.copy = "X"; hintContext.paste = "Y"; hintContext.quickTune = "Back";
    hintContext.clipboardSubject = "Entry";
    Frame();
    Require(HasHint("X", "Copy Entry") && !HasHint("Y", "Paste Entry"),
        "The footer displayed Paste before anything was copied");
    hintContext.clipboardHasContents = true;
    Frame();
    Require(HasHint("X", "Copy Entry") && HasHint("Y", "Paste Entry") && HasHint("Back", "Open Quick Tune"),
        "DDC hints ignored custom controller bindings");
    CheckCenteredHintLayout();
    hintContext.clipboardSubject = "Entire Tab"; Frame();
    Require(HasHint("X", "Copy Entire Tab") && HasHint("Y", "Paste Entire Tab"),
        "Clipboard hints did not explain copying a whole tab");
    hintContext.clipboardSubject = "Transition Override"; Frame();
    Require(HasHint("X", "Copy Transition Override") && HasHint("Y", "Paste Transition Override"),
        "Override descriptions did not reach the grouped footer");
    hintContext.clipboardHasContents = false; Frame();
    Require(HasHint("X", "Copy Transition Override") && !HasHint("Y", "Paste Transition Override"),
        "The grouped footer retained Paste after clearing the clipboard");
    hintContext.clipboardHasContents = true;
    hintContext.clipboardSubject = {};
    Frame();
    Require(!HasHint("X", "Copy Transition Override") && !HasHint("Y", "Paste Transition Override"),
        "Leaving a clipboard target did not clear its displayed shortcuts immediately");
    hintContext.clipboardSubject = "Entry";
    hintContext.copy = DietDrCamera::MenuControllerHints::BindingName("X", false, false);
    hintContext.paste = DietDrCamera::MenuControllerHints::BindingName("Key 0xA2", false, false);
    hintContext.quickTune = DietDrCamera::MenuControllerHints::BindingName("R Stick", true, false);
    panelWidth = 480; Frame(); Frame();
    Require(HasHint("Key X", "Copy Entry") && !HasHint("X", "Copy Entry") && HasHint("Key 0xA2", "Paste Entry"),
        "Rebinding did not update the existing footer immediately");
    Require(HasHint("R-stick click", "Open Quick Tune") && HasHint("LT / RT / Right stick", "Scroll up / down"),
        "A stick-click binding must be distinguished from stick movement for scrolling");
    panelWidth = 800;
    slider = true; Frame();
    Require(HasHint("LB / RB", "Larger steps (10x)") && HasHint("B", "Cancel changes") &&
        !HasHint("LB / RB", "Switch tabs") && !HasHint("Key X", "Copy Entry"),
        "Slider footer retained page actions or lost cancel/coarse adjustment");
    slider = false; binding = true; Frame();
    Require(HasHint("Esc", "Clear binding") && !HasHint("A", "Select / edit"),
        "Binding footer retained navigation actions while capture was active");
    binding = false;
    hintContext.playStation = true;
    hintContext.copy = DietDrCamera::MenuControllerHints::BindingName("X", true, true);
    hintContext.paste.clear();
    hintContext.quickTune = DietDrCamera::MenuControllerHints::BindingName("X", false, true); Frame();
    Require(HasHint("Cross", "Select / edit") && HasHint("Square", "Copy Entry") && HasHint("L1 / R1", "Switch tabs") &&
        HasHint("Key X", "Open Quick Tune") && !HasHint("Key 0xA2", "Paste Entry"),
        "PlayStation labels, keyboard key identity or cleared bindings were stale");
    hintContext = {}; Frame();
    lateForeignWindow = true; Frame();
    Require(footer && !footerReplaced, "DDC replaced hints after a foreign window took focus late in the frame");
    lateForeignWindow = false; ImGui::FocusWindow(content); Frame(); Frame();
    Require(frameworkHighlightVertices == 0, "Framework drew a second highlight over DDC's tabs");
    Click(ddcFirstTabRect);
    Key(ImGuiKey_GamepadR1);
    Require(frameworkHighlightVertices == 0, "Mouse tab selection left the framework's blue focus box on DDC");
    Click(ddcTextRect);
    Require(ImGui::GetActiveID() == ddcTextId && ImGui::GetFocusID() == ddcTextId,
        "DDC capture cleared a real text editor's focus");
    io.AddInputCharactersUTF8("updated"); Frame();
    Require(std::string_view(ddcText) == "updated", "DDC capture interrupted typing");
    Key(ImGuiKey_Enter);
    Require(capture.OwnsCapture(), "DDC did not resume its cursor after text editing");
    const float scrollBefore = content->Scroll.y;
    Key(ImGuiKey_GamepadRStickDown);
    Require(content->Scroll.y == scrollBefore, "Framework right-stick scrolling ran alongside DDC");
    Key(ImGuiKey_GamepadR1);
    Require(selected == 1 && capture.OwnsCapture(), "RB let the framework take over DDC controls");
    Key(ImGuiKey_GamepadFaceDown);
    Require(customActivations == 1, "DDC activation did not work after entry");
    Key(ImGuiKey_GamepadFaceRight);
    Require(frameworkBacks == 0 && selected == 1 && capture.IsSidebar(), "B discarded the DDC page");
    Require(frameworkHighlightVertices > 0, "DDC hid the framework's sidebar highlight");
    Key(ImGuiKey_GamepadFaceDown);
    Require(selected == 1 && entries == 1 && capture.IsSidebar(), "Reselecting DDC automatically entered its controls");
    Require(ImGui::GetFocusID() == treeIds[0], "Reselecting DDC moved focus out of the section list");
    Require(customActivations == 1, "Reselecting the section reused the selection press");
    Key(ImGuiKey_GamepadDpadDown);
    Require(ImGui::GetFocusID() == treeIds[1], "Old sidebar Down navigation failed");
    Key(ImGuiKey_GamepadFaceDown);
    Require(selected == 2 && entries == 2 && capture.IsSidebar(), "Changing DDC sections automatically entered the page");
    Require(ImGui::GetFocusID() == treeIds[1], "Changing DDC sections lost sidebar focus");
    Require(customActivations == 1, "Changing sections reused the selection press");
    Key(ImGuiKey_GamepadDpadRight);
    Require(!capture.IsSidebar() && selected == 2, "Right did not return to the visible DDC page");
    openPopup = true; Frame(); Frame();
    Require(footerReplaced && HasHint("B", "Close popup") && !HasHint("B", "Back to sections"),
        "DDC popup footer did not use popup Back");
    Key(ImGuiKey_GamepadFaceRight);
    Require(!popup && selected == 2 && !capture.IsSidebar() && frameworkBacks == 0, "Popup B escaped to the framework");
    settingsOpen = focusSettings = true; Frame();
    Require(!footerReplaced, "DDC hints leaked into framework settings");
    Require(!capture.OwnsCapture(), "Capture survived focus moving to framework settings");
    Frame();
    Require(!capture.IsUsable(), "DDC still handled input behind framework settings");
    settingsOpen = false;
    ImGui::FocusWindow(tree); Frame(); Frame();
    FocusTree(folderId, folderRect);
    Key(ImGuiKey_GamepadFaceDown);
    Require(capture.IsSidebar(), "Expanding/collapsing a folder entered the DDC page");
    Key(ImGuiKey_GamepadDpadDown);
    Require(ImGui::GetFocusID() == treeIds[2], "Folder collapse or sidebar movement failed");
    Key(ImGuiKey_GamepadFaceDown);
    Require(selected == 3 && !capture.OwnsCapture(), "Capture leaked onto another mod's page");
    Require(footer && !footerReplaced, "DDC hints leaked onto another mod's page");
    Key(ImGuiKey_GamepadDpadDown);
    Require(ImGui::GetFocusID() == otherIds[1], "The other mod lost native controller navigation");
    Require(frameworkHighlightVertices > 0, "DDC hid the other mod's focus highlight");
    Key(ImGuiKey_GamepadFaceDown);
    Require(otherActivations == 1, "The other mod's button did not activate exactly once");
    Key(ImGuiKey_GamepadFaceRight);
    Require(frameworkBacks == 1 && selected == 0, "The other mod lost framework Back behavior");
    ImGui::FocusWindow(tree); Frame();
    FocusTree(folderId, folderRect); Key(ImGuiKey_GamepadFaceDown); Key(ImGuiKey_GamepadDpadDown); Key(ImGuiKey_GamepadFaceDown);
    Require(selected == 1, "Could not revisit DDC after another mod");
    Click(treeRects[2]);
    Require(selected == 3 && !capture.OwnsCapture(), "Clicking another mod did not release capture");
    Click(otherRects[0], true);
    Require(ImGui::GetActiveID() == otherIds[0], "DDC cleared another mod's active mouse widget");
    ImGui::GetIO().AddMouseButtonEvent(0, false); Frame();
    Click(treeRects[0]);
    Require(selected == 1 && capture.IsSidebar(), "Mouse section selection automatically entered DDC");
    Require(!footerReplaced, "DDC forced controller hints after mouse input");
    Key(ImGuiKey_GamepadDpadRight);
    Require(!capture.IsSidebar(), "Right could not enter a mouse-selected DDC page");
    Click(treeRects[0]);
    Require(capture.IsSidebar(), "Clicking the current section did not return controller focus to the list");
    Key(ImGuiKey_GamepadDpadDown);
    Require(ImGui::GetFocusID() == treeIds[1], "Section navigation failed after mouse reselection");
    Click(ddcTextRect);
    Require(ImGui::GetActiveID() == ddcTextId, "Sidebar mode prevented mouse text editing");
    io.AddInputCharactersUTF8("from sidebar"); Frame();
    Require(std::string_view(ddcText) == "from sidebar", "Sidebar focus interrupted typing in the page");
    Key(ImGuiKey_Enter);
    connected = false; Frame();
    Require(!capture.OwnsCapture(), "Controller disconnect left capture active");
    Require(!footerReplaced, "Controller disconnect left DDC hints active");
    keyboardEnabled = true;
    hintContext = {};
    hintContext.keyboard = true;
    hintContext.copy = "Key C"; hintContext.paste = "Key V"; hintContext.quickTune = "Key Q";
    hintContext.clipboardSubject = "Entry";
    hintContext.clipboardHasContents = true;
    GN::NotifyInputDevice(RE::INPUT_DEVICE::kKeyboard);
    Frame(); Frame();
    Require(capture.IsUsable() && capture.OwnsCapture() && footerReplaced,
        "Keyboard navigation or its footer requires a connected controller");
    Key(ImGuiKey_RightArrow);
    Require(!capture.IsSidebar(), "Keyboard Right did not enter the settings");
    Key(ImGuiKey_RightArrow); Require(keyboardCursor == 1, "Keyboard Right did not navigate horizontally");
    Key(ImGuiKey_DownArrow); Require(keyboardCursor == 3, "Keyboard Down did not navigate vertically");
    Key(ImGuiKey_LeftArrow); Require(keyboardCursor == 2, "Keyboard Left did not navigate horizontally");
    Key(ImGuiKey_UpArrow); Require(keyboardCursor == 0, "Keyboard Up did not navigate vertically");
    Key(ImGuiKey_LeftArrow);
    Require(capture.IsSidebar() && GImGui->NavWindow == tree && !GImGui->NavDisableHighlight,
        "Keyboard Left did not return to the section sidebar");
    const auto beforeSidebarDown = ImGui::GetFocusID();
    Key(ImGuiKey_DownArrow);
    Require(ImGui::GetFocusID() != beforeSidebarDown, "Keyboard Down did not move in the section sidebar");
    Key(ImGuiKey_UpArrow);
    Require(ImGui::GetFocusID() == beforeSidebarDown, "Keyboard Up did not move in the section sidebar");
    FocusTree(treeIds[0], treeRects[0]);
    const int beforeSectionEnter = customActivations;
    Key(ImGuiKey_DownArrow);
    Require(ImGui::GetFocusID() == treeIds[1], "Keyboard Down did not focus the next section");
    Key(ImGuiKey_Enter);
    Require(selected == 2 && capture.IsSidebar() && customActivations == beforeSectionEnter,
        "Enter did not select the focused section, or also activated a page control");
    Key(ImGuiKey_UpArrow); Key(ImGuiKey_KeypadEnter);
    Require(selected == 1 && capture.IsSidebar(), "Numpad Enter did not select the focused section");
    Require(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down,
        "Keyboard section selection left a synthetic controller button held");
    Key(ImGuiKey_RightArrow);
    Require(HasHint(DietDrCamera::MenuControllerHints::ArrowSymbols, "Move cursor") &&
        HasHint("Enter", "Select / edit") && HasHint("Key C", "Copy Entry") &&
        HasHint("Key V", "Paste Entry") && HasHint("Key Q", "Open Quick Tune") &&
        ImGuiMCP::ImGui::ImDrawListManager::groups.size() == 5,
        "Keyboard hints include extra controls or omit the configured hotkeys");
    Require(ImGuiMCP::ImGui::ImDrawListManager::textDraws.front().font == FontAwesome::solid,
        "Arrow hints did not use the framework's symbol font");
    for (const ImWchar glyph : {0xF060, 0xF061, 0xF062, 0xF063})
        Require(FontAwesome::solid->FindGlyphNoFallback(glyph) != nullptr, "An arrow symbol is missing");
    Require(FontAwesome::solid->FindGlyphNoFallback(' ') != nullptr &&
        !FontAwesome::solid->FindGlyphNoFallback(' ')->Visible, "Arrow spacing rendered as an extra icon");
    const int beforeEnter = customActivations;
    Key(ImGuiKey_Enter); Key(ImGuiKey_KeypadEnter);
    Require(customActivations == beforeEnter + 2, "Enter did not activate page controls exactly once per press");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    for (int heldFrame = 0; heldFrame < 45; ++heldFrame) Frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); Frame();
    Require(customActivations == beforeEnter + 3, "Holding Enter repeatedly activated page controls");
    Key(ImGuiKey_Tab); Key(ImGuiKey_Backspace);
    Require(keyboardCursor == 0, "A non-arrow keyboard key navigated the DDC cursor");
    binding = true; Key(ImGuiKey_DownArrow); Key(ImGuiKey_Enter); binding = false;
    Require(customActivations == beforeEnter + 3, "A key being bound also activated a page control");
    Require(keyboardCursor == 0, "An arrow being bound also navigated the menu");
    Click(ddcTextRect); Key(ImGuiKey_RightArrow); Key(ImGuiKey_DownArrow);
    Require(ImGui::GetActiveID() == ddcTextId && keyboardCursor == 0,
        "Typing arrows moved the menu cursor or stole the text editor");
    Key(ImGuiKey_Enter);
    Require(customActivations == beforeEnter + 3, "Text editing Enter also activated a page control");
    const int otherBeforeKeyboardSelection = otherActivations;
    Key(ImGuiKey_LeftArrow);
    FocusTree(treeIds[2], treeRects[2]);
    Key(ImGuiKey_Enter);
    Require(selected == 3 && !capture.OwnsCapture() && !footerReplaced,
        "Keyboard capture or hints leaked to another mod");
    Require(otherActivations == otherBeforeKeyboardSelection && !ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down,
        "Keyboard section selection activated another mod's controls or left a controller button held");
    Click(treeRects[0]); Frame();
    Require(footerReplaced, "Keyboard hints did not return on a DDC page");
    const float savedBorderSize = ImGui::GetStyle().WindowBorderSize;
    for (const float borderSize : {0.0f, 1.0f, 3.0f, 6.0f}) {
        ImGui::GetStyle().WindowBorderSize = borderSize;
        Frame(); Frame();
        Require(footerReplaced && footer->ParentWindow == content->ParentWindow &&
            content->ParentWindow->Size.y == panelHeight,
            "Keyboard footer changed the panel's height");
    }
    ImGui::GetStyle().WindowBorderSize = savedBorderSize;
    CheckFixedFooterLayout();
    CheckLocationPopupHints();
    connected = true; Frame(); Frame();
    panelOpen = false; Frame();
    Require(!capture.OwnsCapture(), "Closing the panel left capture active");
    Require(!footerReplaced, "Closing the panel left DDC hints active");
    std::cout << "3.15 capture, contextual hints, binding updates, layout, sidebar, popup, settings, other-mod and disconnect checks passed\n";
    ImGui::DestroyContext();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
