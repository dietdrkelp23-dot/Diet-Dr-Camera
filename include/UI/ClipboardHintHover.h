#pragma once

#include "UI/DirectionalNavigation.h"
#include <string_view>

namespace DietDrCamera
{
    // Footer metadata has no clipboard payload and never changes its contents.
    // A mouse hover or the clipboard's two-frame grace period cannot advertise
    // an action on a different controller item.
    class ClipboardHintHover
    {
    public:
        void Publish(int frame, const NavigationItem& item, const NavigationItem& cursor,
            bool controllerHover, std::string_view subject, std::span<const NavigationItem> liveItems)
        {
            if (!controllerHover || item.id == 0 || item.id != cursor.id || item.layer != cursor.layer) return;
            // Cursor coordinates are from the previous frame and move during
            // scrolling. Validate against this frame's registered widget instead.
            // This still rejects EndGroup borrowing a slider/reset child's ID.
            for (const auto& registered : liveItems) {
                if (registered.id != item.id || registered.layer != item.layer) continue;
                if (std::abs(item.x0 - registered.x0) >= 2 || std::abs(item.y0 - registered.y0) >= 2 ||
                    std::abs(item.x1 - registered.x1) >= 2 || std::abs(item.y1 - registered.y1) >= 2) continue;
                frame_ = frame;
                item_ = item.id;
                layer_ = item.layer;
                subject_ = subject;
                return;
            }
        }

        std::string_view Resolve(int frame, const NavigationItem& cursor,
            std::span<const NavigationItem> liveItems, int activeLayer) const
        {
            if (frame != frame_ || cursor.id == 0 || cursor.id != item_ ||
                cursor.layer != layer_ || layer_ != activeLayer) return {};
            // A selected row remains a target while scrolling brings it into
            // view. Removed/disabled widgets are absent from the live registry.
            for (const auto& item : liveItems)
                if (item.id == item_ && item.layer == layer_) return subject_;
            return {};
        }

    private:
        int frame_ = -1;
        std::uint32_t item_ = 0;
        int layer_ = -1;
        std::string_view subject_;
    };
}
