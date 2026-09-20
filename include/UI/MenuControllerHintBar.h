#pragma once

#include "UI/MenuControllerHints.h"
#include "UI/MenuHintBarLayout.h"
#include <algorithm>
#include <cfloat>

// Include the framework bindings first. The compatibility fixture supplies
// the same calls through native ImGui, including its actual draw-list code.
namespace DietDrCamera
{
    inline bool DrawMenuControllerHintBar(ImGuiMCP::ImGuiWindow* window,
        const MenuControllerHints::Context& context)
    {
        using namespace ImGuiMCP;
        namespace Gui = ImGuiMCP::ImGui;
        namespace Draw = ImGuiMCP::ImGui::ImDrawListManager;
        namespace Hints = MenuControllerHints;
        if (!window || !window->DrawList) return false;
        const auto hints = Hints::Build(context);
        constexpr auto capacity = Hints::SlotCount;
        std::array<const Hints::Hint*, capacity> visible{};
        for (const auto& hint : hints) visible[static_cast<std::size_t>(hint.slot)] = &hint;
        auto* font = Gui::GetFont();
        if (!font || !font->ContainerAtlas) return false;
        auto* symbolFont = font;
        if (context.keyboard) {
            FontAwesome::PushSolid();
            symbolFont = Gui::GetFont();
            FontAwesome::Pop();
        }
        const float nominalSize = Gui::ImGuiWindowManager::CalcFontSize(window);
        if (nominalSize <= 0 || window->Size.x <= 0 || window->Size.y <= 0) return false;
        const auto textWidth = [nominalSize](ImFont* textFont, const std::string& text) {
            ImVec2 size{};
            Gui::ImFontManger::CalcTextSizeA(&size, textFont, nominalSize, FLT_MAX, 0,
                text.c_str(), nullptr, nullptr);
            return size.x;
        };
        struct Measurement { float key, action; };
        std::array<Measurement, capacity> measures{};
        for (const auto& hint : Hints::LayoutCandidates(context)) {
            auto& m = measures[static_cast<std::size_t>(hint.slot)];
            m.key = std::max(m.key, textWidth(hint.symbolKeys ? symbolFont : font, hint.keys));
            m.action = std::max(m.action, textWidth(font, hint.action));
        }
        std::array<std::size_t, capacity> slots{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < capacity; ++i)
            if (measures[i].key > 0 || measures[i].action > 0) slots[count++] = i;
        if (count == 0) return false;
        // Keep the same compact gap between visible groups. Each binding and
        // its explanation share a border and background, with a divider inside.
        const float paddingX = std::max(5.0f, nominalSize * 0.3f);
        const float paddingY = std::max(2.0f, nominalSize * 0.08f);
        const float pairGap = std::max(8.0f, nominalSize * 0.4f);
        const float itemGap = std::max(8.0f, nominalSize * 0.3f);
        const float rowGap = std::max(3.0f, nominalSize * 0.12f);
        const float lineGap = 2.0f;
        const float margin = std::max(4.0f, nominalSize * 0.4f);
        const float availableWidth = window->Size.x - 2 * margin;
        const float availableHeight = window->Size.y - 4.0f;
        constexpr float stackedKeyScale = 0.82f;

        struct Layout { bool stacked; std::size_t split; float scale; };
        Layout layout{false, count, 0};
        const auto consider = [&](bool stacked, std::size_t split) {
            const int rows = split < count ? 2 : 1;
            const float rowHeight = (availableHeight - (rows - 1) * rowGap) / rows;
            const float heightLimit = stacked
                ? (rowHeight - 2 * paddingY - lineGap) / (nominalSize * (1 + stackedKeyScale))
                : (rowHeight - 2 * paddingY) / nominalSize;
            float scale = std::min(1.0f, heightLimit);
            for (int row = 0; row < rows; ++row) {
                const std::size_t begin = row == 0 ? 0 : split;
                const std::size_t end = row == 0 ? split : count;
                const auto count = static_cast<float>(end - begin);
                float text = 0;
                for (auto i = begin; i < end; ++i)
                    text += stacked ? std::max(measures[slots[i]].key * stackedKeyScale, measures[slots[i]].action)
                                    : measures[slots[i]].key + measures[slots[i]].action;
                const float fixed = count * (2 * paddingX + (stacked ? 0 : pairGap)) + (count - 1) * itemGap;
                if (text > 0) scale = std::min(scale, (availableWidth - fixed) / text);
            }
            if (scale > layout.scale) layout = {stacked, split, scale};
        };
        // Fit the longest captions so changing context cannot change rows or
        // font size. Centering below uses only the boxes currently visible.
        consider(false, count);
        consider(true, count);
        for (std::size_t split = 1; split < count; ++split) consider(false, split);
        if (layout.scale <= 0) return false;
        const float fontSize = nominalSize * layout.scale;
        const float keyFontSize = layout.stacked ? fontSize * stackedKeyScale : fontSize;
        const float groupHeight = 2 * paddingY + fontSize + (layout.stacked ? keyFontSize + lineGap : 0);
        const int rows = layout.split < count ? 2 : 1;
        const float top = window->Pos.y + (window->Size.y - (groupHeight * rows + (rows - 1) * rowGap)) * 0.5f;
        std::array<Measurement, capacity> contents{};
        for (const auto& hint : hints) {
            contents[static_cast<std::size_t>(hint.slot)] = {
                textWidth(hint.symbolKeys ? symbolFont : font, hint.keys) * (keyFontSize / nominalSize),
                textWidth(font, hint.action) * layout.scale};
        }
        const auto groupWidth = [&](std::size_t index) {
            const auto& m = contents[slots[index]];
            return 2 * paddingX + (layout.stacked ? std::max(m.key, m.action)
                                                 : m.key + m.action + pairGap);
        };
        auto* draw = window->DrawList;
        const auto flags = draw->Flags;
        // Only this frame's identified DDC footer is replaced. Keeping its
        // draw list preserves normal window ordering, clipping and modal dimming.
        Draw::_ResetForNewFrame(draw);
        draw->Flags = flags;
        Draw::PushTextureID(draw, font->ContainerAtlas->TexID);
        Draw::PushClipRect(draw, window->ClipRect.Min, window->ClipRect.Max, false);
        Draw::AddRectFilled(draw, window->Pos,
            ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
            Gui::GetColorU32(ImGuiCol_ChildBg), 0, 0);
        Draw::AddRectFilled(draw, window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + 1.0f),
            Gui::GetColorU32(ImGuiCol_Separator), 0, 0);
        const auto textColor = Gui::GetColorU32(ImGuiCol_Text);
        const auto keyColor = Gui::GetColorU32(ImVec4(0.95f, 0.78f, 0.36f, 1.0f));
        const auto borderColor = Gui::GetColorU32(ImGuiCol_Border);
        const auto groupColor = Gui::GetColorU32(ImGuiCol_FrameBg);
        for (int row = 0; row < rows; ++row) {
            const std::size_t begin = row == 0 ? 0 : layout.split;
            const std::size_t end = row == 0 ? layout.split : count;
            float width = 0;
            std::size_t visibleCount = 0;
            for (auto i = begin; i < end; ++i) {
                if (!visible[slots[i]]) continue;
                if (visibleCount++ > 0) width += itemGap;
                width += groupWidth(i);
            }
            if (visibleCount == 0) continue;
            float x = window->Pos.x + (window->Size.x - width) * 0.5f;
            const float y = top + row * (groupHeight + rowGap);
            for (auto i = begin; i < end; ++i) {
                const auto* hint = visible[slots[i]];
                if (!hint) continue;
                // Advance by the current box, not its longest possible caption.
                // Hidden hints consume no horizontal space.
                const float keyWidth = contents[slots[i]].key;
                const float w = groupWidth(i);
                Draw::AddRectFilled(draw, ImVec2(x, y), ImVec2(x + w, y + groupHeight), borderColor, 3.0f, 0);
                Draw::AddRectFilled(draw, ImVec2(x + 1, y + 1), ImVec2(x + w - 1, y + groupHeight - 1), groupColor, 2.0f, 0);
                ImVec2 keyPos(x + paddingX, y + paddingY), actionPos{};
                if (layout.stacked) {
                    actionPos = ImVec2(keyPos.x, y + paddingY + keyFontSize + lineGap);
                } else {
                    const float divider = keyPos.x + keyWidth + pairGap * 0.5f;
                    Draw::AddRectFilled(draw, ImVec2(divider, y + paddingY),
                        ImVec2(divider + 1, y + groupHeight - paddingY), borderColor, 0, 0);
                    actionPos = ImVec2(keyPos.x + keyWidth + pairGap, keyPos.y);
                }
                Draw::AddText(draw, hint->symbolKeys ? symbolFont : font, keyFontSize, keyPos,
                    keyColor, hint->keys.c_str(), nullptr, 0, nullptr);
                Draw::AddText(draw, font, fontSize, actionPos, textColor, hint->action.c_str(), nullptr, 0, nullptr);
                x += w + itemGap;
            }
        }
        Draw::PopClipRect(draw);
        Draw::PopTextureID(draw);
        return true;
    }

    // Use the same space inside the main window as SMF's controller footer.
    // MenuHintBarLayout reserves it before the framework begins either pane.
    inline ImGuiMCP::ImGuiWindow* DrawMenuKeyboardHintBar(ImGuiMCP::ImGuiWindow* panel,
        const MenuControllerHints::Context& context)
    {
        using namespace ImGuiMCP;
        namespace Gui = ImGuiMCP::ImGui;
        if (!panel || MenuControllerHints::Build(context).empty()) return nullptr;
        ImGuiWindow* page = nullptr;
        ImGuiWindow* tree = nullptr;
        for (int i = 0; i < panel->DC.ChildWindows.Size; ++i) {
            auto* child = panel->DC.ChildWindows.Data[i];
            if (child->LastFrameActive != Gui::GetFrameCount()) continue;
            if (std::strstr(child->Name, "/SKSEModControlPanelMenuNode_")) page = child;
            if (std::strstr(child->Name, "/SKSEModControlPanelTreeView_")) tree = child;
        }
        if (!page || !tree) return nullptr;
        const auto* style = Gui::GetStyle();
        const float height = MenuHintBarHeight(panel);
        const float y = std::max(page->Pos.y + page->Size.y, tree->Pos.y + tree->Size.y) + style->ItemSpacing.y;
        const float bottom = panel->Pos.y + panel->Size.y - panel->WindowPadding.y - panel->DecoOuterSizeY2;
        // On the first keyboard frame entering DDC, the framework has already
        // sized both panes. Wait for their next Begin rather than cover them.
        if (y + height > bottom + 0.5f) return nullptr;
        Gui::Begin(panel->Name, nullptr, panel->Flags);
        Gui::SetCursorScreenPos(ImVec2(tree->Pos.x, y));
        Gui::BeginChild("##DDCKeyboardHints", ImVec2(0, height), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto* footer = Gui::GetCurrentWindow();
        DrawMenuControllerHintBar(footer, context);
        Gui::EndChild();
        Gui::End();
        return footer;
    }
}
