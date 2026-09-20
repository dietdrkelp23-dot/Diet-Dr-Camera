#pragma once

#include "UI/DirectionalNavigation.h"

namespace DietDrCamera
{
    inline NavigationItem CurrentMenuNavigationItem(int layer, bool useClipRect)
    {
        namespace Gui = ImGuiMCP::ImGui;
        ImGuiMCP::ImVec2 lo, hi, pos, size, contentMin, contentMax;
        Gui::GetItemRectMin(&lo); Gui::GetItemRectMax(&hi);
        Gui::GetWindowPos(&pos); Gui::GetWindowSize(&size);
        Gui::GetWindowContentRegionMin(&contentMin);
        Gui::GetWindowContentRegionMax(&contentMax);
        // Long Selectable labels report an unclipped width. The navigation
        // cursor uses only the part inside the containing list or column.
        float x0 = std::max(lo.x, pos.x + contentMin.x);
        float x1 = std::min(hi.x, pos.x + contentMax.x);
        if (x1 <= x0) { x0 = lo.x; x1 = hi.x; }
        NavigationItem item{x0, lo.y, x1, hi.y, layer, Gui::GetItemID(),
            pos.y, pos.y + size.y, pos.x, pos.x + size.x};
        if (useClipRect) {
            // The validated ImGui layout exposes the current table column's
            // clip rectangle without changing the physical window bounds used
            // by scroll ownership and cursor restoration.
            const auto& clip = Gui::GetCurrentWindow()->ClipRect;
            item.clipX0 = clip.Min.x; item.clipY0 = clip.Min.y;
            item.clipX1 = clip.Max.x; item.clipY1 = clip.Max.y;
            item.x0 = std::max(item.x0, item.clipX0);
            item.x1 = std::min(item.x1, item.clipX1);
        }
        return item;
    }
}
