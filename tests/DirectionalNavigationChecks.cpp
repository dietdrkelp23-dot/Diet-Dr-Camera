#include "UI/DirectionalNavigation.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace DietDrCamera;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static NavigationItem Item(std::uint32_t id, float x, float y, float w = 80, float h = 20)
{
    return {x, y, x + w, y + h, 0, id, 0, 600, 0, 1000};
}

static int Index(std::span<const NavigationItem> items, std::uint32_t id)
{
    for (int i = 0; i < static_cast<int>(items.size()); ++i) if (items[i].id == id) return i;
    throw std::runtime_error("Missing fixture item");
}

static void Expect(DirectionalNavigation& nav, std::span<const NavigationItem> items,
    std::uint32_t from, int dx, int dy, std::uint32_t expected, const char* message, int layer = 0)
{
    const int next = nav.Move(items, Index(items, from), dx, dy, layer);
    Require(expected == 0 ? next == -1 : next >= 0 && items[next].id == expected, message);
}

static void CheckOffsetControls()
{
    // The next row is slightly to the side, with an aligned control much farther
    // down. This is the reported strict-column failure, mirrored in both axes.
    for (int sign : {-1, 1}) {
        std::array items{Item(1, 300, 250), Item(2, 300 + sign * 90.0f, 285), Item(3, 300, 430)};
        DirectionalNavigation nav;
        Expect(nav, items, 1, 0, 1, 2, "Down skipped the nearby offset row");
        Expect(nav, items, 2, 0, -1, 1, "Up could not return from the offset row");
    }
    std::array items{Item(1, 100, 100), Item(2, 190, 128), Item(3, 500, 100), Item(4, 190, 350)};
    DirectionalNavigation nav;
    Expect(nav, items, 1, 1, 0, 2, "Right skipped a slightly staggered neighbor");
    Expect(nav, items, 2, -1, 0, 1, "Left could not return from a staggered neighbor");
    std::array isolated{items[0], items[3]};
    nav.Reset();
    Expect(nav, isolated, 1, 1, 0, 0, "Right became a large vertical jump");
    std::array sameRow{Item(1, 100, 100), Item(2, 190, 100, 80, 26), Item(3, 100, 140)};
    nav.Reset();
    Expect(nav, sameRow, 1, 0, 1, 3, "Down selected a taller control on the same row");
}

static void CheckRememberedColumn()
{
    // A full-width group header must not pull a right-column cursor to center.
    std::vector items{Item(1, 300, 100), Item(2, 80, 135, 300),
        Item(3, 80, 170), Item(4, 190, 170), Item(5, 300, 170),
        Item(6, 80, 205, 300), Item(7, 80, 240), Item(8, 190, 240), Item(9, 300, 240)};
    DirectionalNavigation nav;
    Expect(nav, items, 1, 0, 1, 2, "Could not enter a wide header");
    // Registration order changes as groups expand, but focus identity survives.
    std::reverse(items.begin(), items.end());
    Expect(nav, items, 2, 0, 1, 5, "Wide header lost the original right column");
    Expect(nav, items, 5, 0, 1, 6, "Could not enter the next wide header");
    Expect(nav, items, 6, 0, 1, 9, "Repeated wide headers lost the column");
    Expect(nav, items, 9, 0, -1, 6, "Up failed to reverse the route");
    Expect(nav, items, 6, 0, -1, 5, "Up lost the remembered column");
    Expect(nav, items, 5, -1, 0, 4, "Left failed to select the neighboring column");
    Expect(nav, items, 4, 0, 1, 6, "Down failed after a horizontal move");
    Expect(nav, items, 6, 0, 1, 8, "Horizontal move did not update the intended column");
    // Mouse selection/another page changes focus outside the navigator.
    Expect(nav, items, 3, 0, 1, 6, "External focus could not move down");
    Expect(nav, items, 6, 0, 1, 7, "External focus retained the old column");
}

static void CheckWindowsAndLayers()
{
    auto left = Item(1, 20, 100, 180);
    left.wx1 = 240;
    auto right = Item(2, 280, 132, 180);
    right.wx0 = 260;
    auto lower = Item(3, 20, 200, 180);
    lower.wx1 = 240;
    auto hidden = Item(4, 280, 650, 180);
    hidden.wx0 = 260;
    auto scrolled = Item(5, 20, 650, 180);
    scrolled.wx1 = 240;
    auto popup = Item(6, 20, 125, 180);
    popup.layer = 2;
    auto popupNext = Item(7, 20, 165, 180);
    popupNext.layer = 2;
    std::array items{left, right, lower, hidden, scrolled, popup, popupNext};
    DirectionalNavigation nav;
    Expect(nav, items, 1, 0, 1, 3, "Down escaped into the adjacent editor or modal");
    Expect(nav, items, 1, 1, 0, 2, "Right could not enter a staggered adjacent pane");
    Expect(nav, items, 2, 0, 1, 4, "Same-window offscreen row became unreachable");
    Expect(nav, items, 5, 1, 0, 0, "Right entered an invisible row in another pane");
    Expect(nav, items, 6, 0, 1, 7, "Popup cursor escaped its active layer", 2);
    std::array boundary{left, right};
    nav.Reset();
    Expect(nav, boundary, 1, 0, 1, 0, "Down at pane bottom jumped into its neighbor");
    // A child list can be entered from its parent toolbar and exited below.
    auto toolbar = Item(10, 20, 20, 180);
    auto first = left; first.id = 11; first.wy0 = 70; first.wy1 = 250;
    auto footer = Item(12, 20, 270, 180);
    std::array parentChild{toolbar, first, footer};
    nav.Reset();
    Expect(nav, parentChild, 10, 0, 1, 11, "Down skipped an embedded list");
    Expect(nav, parentChild, 11, 0, 1, 12, "Could not leave the child for its footer");
    Expect(nav, parentChild, 12, 0, -1, 11, "Could not reenter the child from below");
}

static void CheckListFooterColumns()
{
    // Remove is in the parent page, below a sparse bound list. The neighboring
    // library and editor have controls much closer to the button vertically.
    for (float scale : {0.5f, 2.0f / 3.0f, 1.0f, 1.2f}) {
        for (bool mirrored : {false, true}) {
            for (bool severalRows : {false, true}) {
                auto library = Item(1, 28, 750, 384);
                library.wx0 = 20; library.wx1 = 420;
                library.wy0 = 100; library.wy1 = 800;
                auto bound = Item(2, 448, 120, 244);
                bound.wx0 = 440; bound.wx1 = 700;
                bound.wy0 = 100; bound.wy1 = 800;
                auto editor = Item(3, 730, 680, 500);
                auto remove = Item(4, 440, 820, 260, 30);
                auto bind = Item(5, 20, 820, 400, 30);
                auto toolbar = Item(6, 440, 40, 260, 30);
                for (auto* item : {&editor, &remove, &bind, &toolbar}) {
                    item->wx1 = 1400; item->wy1 = 900;
                }
                auto hidden = bound; hidden.id = 7; hidden.y0 = 805; hidden.y1 = 825;
                std::vector items{library, bound, editor, remove, bind, toolbar, hidden};
                if (severalRows) {
                    auto last = bound; last.id = 8; last.y0 = 150; last.y1 = 170;
                    items.push_back(last);
                }
                for (auto& item : items) {
                    if (mirrored) {
                        const float x0 = item.x0, wx0 = item.wx0;
                        item.x0 = 1400 - item.x1; item.x1 = 1400 - x0;
                        item.wx0 = 1400 - item.wx1; item.wx1 = 1400 - wx0;
                    }
                    item.x0 = item.x0 * scale + 73; item.x1 = item.x1 * scale + 73;
                    item.wx0 = item.wx0 * scale + 73; item.wx1 = item.wx1 * scale + 73;
                    item.y0 = item.y0 * scale + 31; item.y1 = item.y1 * scale + 31;
                    item.wy0 = item.wy0 * scale + 31; item.wy1 = item.wy1 * scale + 31;
                }
                std::reverse(items.begin(), items.end());
                DirectionalNavigation nav;
                const auto last = severalRows ? 8u : 2u;
                Expect(nav, items, 4, 0, -1, last, "Up from Remove left the bound list's column");
                Expect(nav, items, last, 0, 1, 7, "Down left the list before reaching its offscreen row");
                std::erase_if(items, [](const auto& item) { return item.id == 7; });
                nav.Reset();
                Expect(nav, items, last, 0, 1, 4, "Down from the bound list missed Remove");
                Expect(nav, items, 4, mirrored ? 1 : -1, 0, 5, "Horizontal footer navigation stopped working");
                Expect(nav, items, 5, 0, -1, 1, "Up from Bind missed the library");
                Expect(nav, items, 6, 0, 1, 2, "Down from the toolbar missed its list's first visible row");
                // A real control between the button and the list still comes first.
                auto intervening = items[Index(items, 4)];
                intervening.id = 9;
                intervening.y0 = 800 * scale + 31; intervening.y1 = 815 * scale + 31;
                items.push_back(intervening);
                nav.Reset();
                Expect(nav, items, 4, 0, -1, 9, "Entering a list skipped a closer control in the same column");
            }
        }
    }
}

static void CheckPaneCrossingAndRelayout()
{
    auto libraryTop = Item(1, 20, 100, 180);
    auto libraryBottom = Item(2, 20, 500, 180);
    auto bound = Item(3, 280, 100, 180);
    for (auto* item : {&libraryTop, &libraryBottom}) item->wx1 = 240;
    bound.wx0 = 260; bound.wx1 = 500;
    auto hidden = bound; hidden.id = 4; hidden.y0 = 610; hidden.y1 = 630;
    std::array items{libraryTop, libraryBottom, bound, hidden};
    DirectionalNavigation nav;
    Expect(nav, items, 2, 1, 0, 3, "Right could not enter a sparse neighboring pane");
    Expect(nav, items, 3, -1, 0, 2, "Left lost the original row when returning from a sparse pane");
    Expect(nav, items, 2, 1, 0, 3, "Repeated pane crossing changed the route");
    // Reposition and scale the whole menu while the cursor is in the sparse pane.
    for (auto& item : items) {
        item.x0 = item.x0 * 0.75f + 95; item.x1 = item.x1 * 0.75f + 95;
        item.wx0 = item.wx0 * 0.75f + 95; item.wx1 = item.wx1 * 0.75f + 95;
        item.y0 = item.y0 * 0.75f + 300; item.y1 = item.y1 * 0.75f + 300;
        item.wy0 = item.wy0 * 0.75f + 300; item.wy1 = item.wy1 * 0.75f + 300;
    }
    Expect(nav, items, 3, -1, 0, 2, "Menu relayout lost the remembered row");
    Expect(nav, items, 3, 0, -1, 0, "Up escaped a sparse pane into its neighbor");
    Expect(nav, items, 2, 0, -1, 0, "Navigation moved a cursor from an inactive modal layer", 2);
}

static void CheckInlineResetButtons()
{
    // Main-menu sliders span the editor while their small Reset buttons sit
    // beside the labels above. A wide slider's center can be far from Reset.
    for (float width : {500.0f, 900.0f, 1400.0f}) {
        for (float scale : {0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.2f}) {
            for (bool mirror : {false, true}) {
                const float resetX = mirror ? width - 110.0f : 70.0f;
                std::array items{
                    Item(1, resetX, 40, 60, 18), Item(2, 20, 70, width, 28),
                    Item(3, resetX, 126, 60, 18), Item(4, 20, 156, width, 28),
                    Item(5, resetX, 212, 60, 18), Item(6, 20, 242, width, 28)};
                for (auto& item : items) {
                    item.x0 = item.x0 * scale + 83; item.x1 = item.x1 * scale + 83;
                    item.y0 = item.y0 * scale + 47; item.y1 = item.y1 * scale + 47;
                    item.wx0 = 83; item.wx1 = (width + 40) * scale + 83;
                    item.wy0 = 47; item.wy1 = 600 * scale + 47;
                }
                // Changing registration order must not change the route.
                std::reverse(items.begin(), items.end());
                DirectionalNavigation nav;
                Expect(nav, items, 4, 0, -1, 3, "Up skipped the current slider's Reset button");
                Expect(nav, items, 3, 0, 1, 4, "Down could not return from Reset to its slider");
                Expect(nav, items, 4, 0, 1, 5, "Down skipped the next slider's Reset button");
                Expect(nav, items, 5, 0, 1, 6, "Down from the next Reset skipped its slider");
                Expect(nav, items, 6, 0, -1, 5, "Up lost Reset after traversing wide sliders");
                Expect(nav, items, 5, 0, -1, 4, "Up from Reset skipped the previous slider");
            }
        }
    }
}

static void CheckVariedGrids()
{
    std::mt19937 random(20260909);
    std::uniform_real_distribution<float> jitter(-3.0f, 3.0f);
    // Hundreds of varied layouts, translated and scaled like the framework UI.
    // Expected neighbors come from the grid, independently of scoring details.
    for (int layout = 0; layout < 100; ++layout) {
        for (float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
            std::vector<NavigationItem> items;
            for (int row = 0; row < 8; ++row) {
                for (int col = 0; col < 4; ++col) {
                    auto item = Item(1 + row * 4 + col,
                        50 + col * 110.0f + jitter(random), 40 + row * 35.0f + jitter(random));
                    item.x0 = item.x0 * scale + 67; item.x1 = item.x1 * scale + 67;
                    item.y0 = item.y0 * scale + 23; item.y1 = item.y1 * scale + 23;
                    item.wx0 = 67; item.wx1 = 1000 * scale + 67;
                    item.wy0 = 23; item.wy1 = 600 * scale + 23;
                    items.push_back(item);
                }
            }
            std::shuffle(items.begin(), items.end(), random);
            for (int row = 0; row < 8; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const int id = 1 + row * 4 + col;
                    for (auto direction : {std::array{0, 1}, std::array{0, -1}, std::array{1, 0}, std::array{-1, 0}}) {
                        const int r = row + direction[1], c = col + direction[0];
                        if (r < 0 || r >= 8 || c < 0 || c >= 4) continue;
                        DirectionalNavigation nav;
                        Expect(nav, items, id, direction[0], direction[1], 1 + r * 4 + c,
                            "Varied/scaled grid skipped its immediate neighbor");
                    }
                }
            }
        }
    }
}

static void CheckClippedList()
{
    DirectionalNavigation nav;
    // The registry contains only the viewport and its nearby rows, not all
    // 2,000 library entries. Scrolling changes screen positions every frame.
    for (int direction : {1, -1}) {
        for (int row = direction > 0 ? 3 : 1996;
             direction > 0 ? row < 1997 : row > 2; row += direction) {
            std::vector<NavigationItem> visible;
            for (int offset = -2; offset <= 2; ++offset)
                visible.push_back(Item(1 + row + offset, 30, 300 + offset * 24.0f, 300));
            std::reverse(visible.begin(), visible.end());
            Expect(nav, visible, row + 1, 0, direction, row + direction + 1,
                "Clipped library skipped a row or lost focus after scrolling");
        }
    }
}

int main()
{
    try {
        CheckOffsetControls();
        CheckRememberedColumn();
        CheckWindowsAndLayers();
        CheckListFooterColumns();
        CheckPaneCrossingAndRelayout();
        CheckInlineResetButtons();
        CheckVariedGrids();
        CheckClippedList();
        std::cout << "Directional navigation checks passed (offsets, lanes, panes, list footers, layers, inline resets, 400 grids, 2,000-row list).\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
