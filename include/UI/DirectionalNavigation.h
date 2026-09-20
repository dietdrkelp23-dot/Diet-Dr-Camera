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
        float clipX0 = -3.4e38f, clipY0 = -3.4e38f;
        float clipX1 = 3.4e38f, clipY1 = 3.4e38f;

        float RegionX0() const { return std::max(wx0, clipX0); }
        float RegionY0() const { return std::max(wy0, clipY0); }
        float RegionX1() const { return std::min(wx1, clipX1); }
        float RegionY1() const { return std::min(wy1, clipY1); }
        bool Visible() const
        {
            return x1 > RegionX0() && x0 < RegionX1() && y1 > RegionY0() && y0 < RegionY1();
        }
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
            if (from.layer != activeLayer) return -1;
            const bool vertical = dy != 0;
            if (!_hasLane || _vertical != vertical || _itemID != from.id || _layer != from.layer) {
                _lane = vertical
                    ? (std::max(from.x0, from.RegionX0()) + std::min(from.x1, from.RegionX1())) * 0.5f
                    : (std::max(from.y0, from.RegionY0()) + std::min(from.y1, from.RegionY1())) * 0.5f;
                _vertical = vertical;
                _hasLane = true;
            } else if (_laneRegionSize > 0 && std::isfinite(_laneRegionSize)) {
                const float start = vertical ? from.RegionX0() : from.RegionY0();
                const float size = (vertical ? from.RegionX1() : from.RegionY1()) - start;
                if (size > 0 && std::isfinite(size))
                    _lane = start + (_lane - _laneRegionStart) * (size / _laneRegionSize);
            }
            _itemID = from.id;
            _layer = from.layer;

            int best = -1;
            auto bestRank = std::tuple(std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(), std::numeric_limits<std::uint32_t>::max());
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const auto& to = items[i];
                if (i == current || to.layer != activeLayer || to.x1 <= to.x0 || to.y1 <= to.y0) continue;
                const bool sameWindow = from.SameWindow(to);
                const bool sameColumn = sameWindow &&
                    std::abs(from.RegionX0() - to.RegionX0()) < 2.0f &&
                    std::abs(from.RegionX1() - to.RegionX1()) < 2.0f;
                if (!sameColumn && !to.Visible()) continue;
                // Table columns share an ImGui window. Their clip regions still
                // form separate columns, just like side-by-side child lists.
                if (vertical && std::min(from.RegionX1(), to.RegionX1()) <=
                    std::max(from.RegionX0(), to.RegionX0())) continue;
                // A parent toolbar/footer spans several children. Use the
                // actual button's column when entering or leaving those panes.
                if (vertical && !sameWindow &&
                    (std::min(from.x1, to.RegionX1()) <= std::max(from.x0, to.RegionX0()) ||
                     std::min(to.x1, from.RegionX1()) <= std::max(to.x0, from.RegionX0()))) continue;

                const float rowHeight = std::max(1.0f, std::min(from.y1 - from.y0, to.y1 - to.y0));
                // ImGui floors row advances to pixels, while Selectable hit
                // rectangles retain fractional font heights and spacing.
                const float epsilon = std::max(1.0f, rowHeight * 0.025f);
                // Cross-pane movement follows visible rectangles. Within one
                // list, retain complete rows so an offscreen neighbor can scroll
                // into view instead of being skipped for a footer.
                const float fx0 = sameColumn ? from.x0 : std::max(from.x0, from.RegionX0());
                const float fx1 = sameColumn ? from.x1 : std::min(from.x1, from.RegionX1());
                const float fy0 = sameColumn ? from.y0 : std::max(from.y0, from.RegionY0());
                const float fy1 = sameColumn ? from.y1 : std::min(from.y1, from.RegionY1());
                const float tx0 = sameColumn ? to.x0 : std::max(to.x0, to.RegionX0());
                const float tx1 = sameColumn ? to.x1 : std::min(to.x1, to.RegionX1());
                const float ty0 = sameColumn ? to.y0 : std::max(to.y0, to.RegionY0());
                const float ty1 = sameColumn ? to.y1 : std::min(to.y1, to.RegionY1());
                const float forward = vertical
                    ? (dy > 0 ? ty0 - fy1 : fy0 - ty1)
                    : (dx > 0 ? tx0 - fx1 : fx0 - tx1);
                // A wider/taller widget on the same row is not "below" it.
                if (forward < -epsilon) continue;
                const float fromLo = vertical ? fx0 : fy0;
                const float fromHi = vertical ? fx1 : fy1;
                const float toLo = vertical ? tx0 : ty0;
                const float toHi = vertical ? tx1 : ty1;
                const float crossGap = std::max({fromLo - toHi, toLo - fromHi, 0.0f});
                const float laneDistance = std::max({toLo - _lane, _lane - toHi, 0.0f});
                const float regionLo = vertical ? to.RegionX0() : to.RegionY0();
                const float regionHi = vertical ? to.RegionX1() : to.RegionY1();
                const float entryForward = vertical
                    ? (dy > 0 ? to.RegionY0() - fy1 : fy0 - to.RegionY1())
                    : (dx > 0 ? to.RegionX0() - fx1 : fx0 - to.RegionX1());
                const float regionGap = std::max({fromLo - regionHi, regionLo - fromHi, 0.0f});
                const bool enteringPane = entryForward >= -epsilon &&
                    (vertical || regionGap <= std::max(entryForward * 0.75f, rowHeight * 2.0f));
                if (!vertical && enteringPane && ChildAtLane(items, from, to, dx, activeLayer, epsilon)) continue;
                // Horizontal navigation can bridge staggered rows without
                // turning a left/right press into a large vertical jump. A
                // sparse neighboring pane is entered through its visible area.
                if (!vertical && !enteringPane &&
                    crossGap > std::max(forward * 0.75f, rowHeight * 2.0f)) continue;
                // A small Reset button above a wide slider shares its column.
                // Keep the remembered lane from outweighing that nearby row;
                // center distance still selects the intended column on ties.
                const float lanePenalty = sameWindow && crossGap == 0.0f
                    ? std::min(laneDistance * 0.2f, rowHeight * 0.5f) : laneDistance * 0.2f;
                float score = std::max(0.0f, forward) + crossGap * 0.5f + lanePenalty +
                    (sameWindow ? 0.0f : rowHeight * 2.0f);
                float insideDistance = 0.0f;
                if (enteringPane) {
                    // Choose the pane before its contents: empty space in a
                    // list must not send the cursor to a busier neighbor.
                    const float regionLaneDistance = std::max({regionLo - _lane, _lane - regionHi, 0.0f});
                    score = std::max(0.0f, entryForward) + regionGap * 0.5f + regionLaneDistance * 0.2f;
                    insideDistance = vertical ? std::max(0.0f, forward) : laneDistance;
                }
                const float centerDistance = std::abs((toLo + toHi) * 0.5f - _lane);
                const auto rank = std::tuple(score, insideDistance, centerDistance, to.id);
                if (rank < bestRank) { bestRank = rank; best = i; }
            }
            const auto& anchor = best >= 0 ? items[best] : from;
            _itemID = anchor.id;
            _laneRegionStart = vertical ? anchor.RegionX0() : anchor.RegionY0();
            _laneRegionSize = (vertical ? anchor.RegionX1() : anchor.RegionY1()) - _laneRegionStart;
            return best;
        }

    private:
        bool ChildAtLane(std::span<const NavigationItem> items, const NavigationItem& from,
            const NavigationItem& parent, int dx, int activeLayer, float epsilon) const
        {
            // A table column's footer shares the parent's full-height clip
            // region. When crossing through the list above it, that child is
            // the destination, even if its only row is near the top.
            for (const auto& child : items) {
                if (child.layer != activeLayer || !child.Visible() || child.SameWindow(parent)) continue;
                if (child.wx0 < parent.wx0 - epsilon || child.wx1 > parent.wx1 + epsilon ||
                    child.wy0 < parent.wy0 - epsilon || child.wy1 > parent.wy1 + epsilon) continue;
                if (child.RegionX0() < parent.RegionX0() - epsilon ||
                    child.RegionX1() > parent.RegionX1() + epsilon ||
                    _lane < child.RegionY0() || _lane >= child.RegionY1()) continue;
                const float distance = dx > 0 ? child.RegionX0() - from.x1 : from.x0 - child.RegionX1();
                if (distance >= -epsilon) return true;
            }
            return false;
        }

        float _lane = 0;
        float _laneRegionStart = 0, _laneRegionSize = 0;
        std::uint32_t _itemID = 0;
        int _layer = 0;
        bool _hasLane = false, _vertical = false;
    };
}
