#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>

namespace DietDrCamera
{
    struct NavigationItem
    {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int layer = 0;
        std::uint32_t id = 0;
        float wy0 = -3.4e38f, wy1 = 3.4e38f;
        float wx0 = -3.4e38f, wx1 = 3.4e38f;
        bool tab = false;

        bool Visible() const { return y1 > wy0 && y0 < wy1; }
        bool SameWindow(const NavigationItem& other) const
        {
            return std::abs(wy0 - other.wy0) < 2.0f && std::abs(wy1 - other.wy1) < 2.0f &&
                   std::abs(wx0 - other.wx0) < 2.0f && std::abs(wx1 - other.wx1) < 2.0f;
        }
    };

    // Layout-based navigation shared by every DDC page and Quick Tune. A lane
    // survives wide headers/buttons, just as a text cursor remembers its column
    // while moving over a short line. Changing direction or focus resets it.
    class DirectionalNavigation
    {
    public:
        void Reset() { _hasLane = false; }

        int Move(std::span<const NavigationItem> items, int current, int dx, int dy, int activeLayer)
        {
            if (current < 0 || current >= static_cast<int>(items.size()) ||
                (dx == 0) == (dy == 0)) return -1;
            const auto& from = items[current];
            const bool vertical = dy != 0;
            if (!_hasLane || _vertical != vertical || _itemID != from.id || _layer != from.layer) {
                _lane = vertical ? (from.x0 + from.x1) * 0.5f : (from.y0 + from.y1) * 0.5f;
                _vertical = vertical;
                _hasLane = true;
            }
            _itemID = from.id;
            _layer = from.layer;

            int best = -1;
            auto bestRank = std::tuple(std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(), std::numeric_limits<std::uint32_t>::max());
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const auto& to = items[i];
                if (i == current || to.layer != activeLayer || to.x1 <= to.x0 || to.y1 <= to.y0) continue;
                const bool sameWindow = from.SameWindow(to);
                if (!sameWindow && !to.Visible()) continue;
                // Up/down may enter a child or its parent, but not the separate
                // column beside it. Left/right is the deliberate pane crossing.
                if (vertical && !sameWindow &&
                    std::min(from.wx1, to.wx1) <= std::max(from.wx0, to.wx0)) continue;

                const float rowHeight = std::max(1.0f, std::min(from.y1 - from.y0, to.y1 - to.y0));
                const float epsilon = rowHeight * 0.025f;
                const float forward = vertical
                    ? (dy > 0 ? to.y0 - from.y1 : from.y0 - to.y1)
                    : (dx > 0 ? to.x0 - from.x1 : from.x0 - to.x1);
                // A wider/taller widget on the same row is not "below" it.
                if (forward < -epsilon) continue;
                const float fromLo = vertical ? from.x0 : from.y0;
                const float fromHi = vertical ? from.x1 : from.y1;
                const float toLo = vertical ? to.x0 : to.y0;
                const float toHi = vertical ? to.x1 : to.y1;
                const float crossGap = std::max({fromLo - toHi, toLo - fromHi, 0.0f});
                const float laneDistance = std::max({toLo - _lane, _lane - toHi, 0.0f});
                // Horizontal navigation can bridge staggered rows without
                // turning a left/right press into a large vertical jump.
                if (!vertical && crossGap > std::max(forward * 0.75f, rowHeight * 2.0f)) continue;
                // A small Reset button above a wide slider shares its column.
                // Keep the remembered lane from outweighing that nearby row;
                // center distance still selects the intended column on ties.
                const float lanePenalty = vertical && sameWindow && crossGap == 0.0f
                    ? std::min(laneDistance * 0.2f, rowHeight * 0.5f) : laneDistance * 0.2f;
                const float score = std::max(0.0f, forward) + crossGap * 0.5f + lanePenalty +
                    (sameWindow ? 0.0f : rowHeight * 2.0f);
                const float centerDistance = std::abs((toLo + toHi) * 0.5f - _lane);
                const auto rank = std::tuple(score, centerDistance, to.id);
                if (rank < bestRank) { bestRank = rank; best = i; }
            }
            if (best >= 0) _itemID = items[best].id;
            return best;
        }

    private:
        float _lane = 0;
        std::uint32_t _itemID = 0;
        int _layer = 0;
        bool _hasLane = false, _vertical = false;
    };
}
