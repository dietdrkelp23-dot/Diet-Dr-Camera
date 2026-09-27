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
    if (!(expected == 0 ? next == -1 : next >= 0 && items[next].id == expected)) {
        std::cerr << "from " << from << " direction " << dx << "," << dy << " expected " << expected
            << " got " << (next < 0 ? 0 : items[next].id) << "\n";
        throw std::runtime_error(message);
    }
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

static void CheckSpatialHeaders()
{
    // Tabs, toggles, entries and actions use the same geometry. The expected
    // destination changes with the source column and the controls present.
    for (float scale : {0.5f, 2.0f / 3.0f, 1.0f, 1.2f}) {
        for (bool mirror : {false, true}) {
            for (int layer : {0, 1, 2}) {
                for (float tabX : {20.0f, 400.0f, 850.0f}) {
                    auto tab = Item(1, tabX, 20, 100, 24);
                    auto first = Item(2, 20, 160, 180);
                    auto second = Item(3, 20, 190, 180);
                    for (auto* row : {&first, &second}) {
                        row->wx0 = 10; row->wx1 = 230;
                        row->wy0 = 150; row->wy1 = 260;
                    }
                    auto resetAll = Item(4, 20, 280, 170);
                    auto reset = Item(5, 400, 130, 60);
                    auto slider = Item(6, 350, 160, 600, 28);
                    auto hidden = first; hidden.id = 8; hidden.y0 = 100; hidden.y1 = 120;
                    auto outdoor = Item(10, 20, 80, 85, 24);
                    auto indoor = Item(11, 115, 80, 85, 24);
                    std::vector items{tab, first, second, resetAll, reset, slider, hidden, outdoor, indoor};
                    for (auto& item : items) {
                        item.layer = layer;
                        if (mirror) {
                            const float x0 = item.x0, wx0 = item.wx0;
                            item.x0 = 1000 - item.x1; item.x1 = 1000 - x0;
                            item.wx0 = 1000 - item.wx1; item.wx1 = 1000 - wx0;
                        }
                        item.x0 = item.x0 * scale + 73; item.x1 = item.x1 * scale + 73;
                        item.wx0 = item.wx0 * scale + 73; item.wx1 = item.wx1 * scale + 73;
                        item.y0 = item.y0 * scale + 31; item.y1 = item.y1 * scale + 31;
                        item.wy0 = item.wy0 * scale + 31; item.wy1 = item.wy1 * scale + 31;
                    }
                    auto behindPopup = items[1]; behindPopup.id = 9;
                    behindPopup.layer = layer + 1;
                    items.push_back(behindPopup);
                    std::reverse(items.begin(), items.end());
                    DirectionalNavigation nav;
                    const auto below = tabX < 100 ? 10u : tabX < 500 ? 5u : 6u;
                    Expect(nav, items, 1, 0, 1, below,
                        "Down ignored the closest control below the current column", layer);
                    nav.Reset();
                    Expect(nav, items, 10, mirror ? -1 : 1, 0, 11,
                        "Horizontal navigation missed the neighboring toggle", layer);
                    Expect(nav, items, 11, 0, 1, 2,
                        "Down from a toggle missed the list directly below it", layer);
                    Expect(nav, items, 2, 0, 1, 3,
                        "Down skipped the next list row", layer);
                    std::erase_if(items, [](const auto& item) { return item.id == 10 || item.id == 11; });
                    nav.Reset();
                    Expect(nav, items, 1, 0, 1, tabX < 100 ? 2u : below,
                        "Removing the toggle did not expose the closest control below", layer);
                    std::erase_if(items, [](const auto& item) { return item.id == 2; });
                    nav.Reset();
                    Expect(nav, items, 1, 0, 1, tabX < 100 ? 3u : below,
                        "An unavailable entry changed navigation in an unrelated column", layer);
                }
            }
        }
    }
    // A child window's top edge is not a focusable control. A distant action
    // inside it must not beat either the toggle or the nested row above it.
    auto tab = Item(1, 20, 20);
    auto toggle = Item(2, 20, 50);
    auto row = Item(3, 20, 90);
    row.wx1 = 200; row.wy0 = 80; row.wy1 = 130;
    auto action = Item(4, 20, 160);
    action.wy0 = 42; action.wy1 = 500;
    std::array nested{tab, toggle, row, action};
    DirectionalNavigation nav;
    Expect(nav, nested, 1, 0, 1, 2, "A container boundary stole focus from the nearby toggle");
    Expect(nav, nested, 2, 0, 1, 3, "A nested list lost to a farther action in its parent");
    Expect(nav, nested, 3, 0, 1, 4, "Could not reach the action below the list");
    // Identical visual layouts navigate alike regardless of window ownership.
    auto near = Item(2, 20, 60);
    auto far = Item(3, 20, 78);
    std::array ownership{tab, near, far};
    for (bool child : {false,true}) {
        if (child) { ownership[1].wx1 = 200; ownership[1].wy0 = 55; ownership[1].wy1 = 100; }
        nav.Reset();
        Expect(nav, ownership, 1, 0, 1, 2, "Window ownership outweighed visible proximity");
    }
}

static void CheckTabEntry()
{
    for (float scale : {0.5f, 2.0f / 3.0f, 1.0f, 1.2f}) for (int layer : {0, 1, 2}) {
        auto tab = Item(1, 600, 20, 130, 24); tab.tab = true;
        auto toggle = Item(2, 20, 75, 280, 30); toggle.entryTab = tab.id;
        auto slider = Item(3, 20, 150, 930, 28);
        auto neighbor = Item(4, 450, 20, 130, 24); neighbor.tab = true;
        std::vector items{slider, neighbor, toggle, tab};
        for (auto& item : items) {
            item.layer = layer;
            item.x0 *= scale; item.x1 *= scale;
            item.y0 *= scale; item.y1 *= scale;
            item.wx0 *= scale; item.wx1 *= scale;
            item.wy0 *= scale; item.wy1 *= scale;
        }
        DirectionalNavigation nav;
        Expect(nav, items, 1, 0, 1, 2, "Vanity tab skipped its leading toggle", layer);
        Expect(nav, items, 2, 0, -1, 1, "Toggle returned to an unrelated tab", layer);
        Expect(nav, items, 1, 0, 1, 2, "Returning from the tab lost the toggle", layer);
        Expect(nav, items, 2, 0, 1, 3, "Tab entry lane stranded focus before the next control", layer);
        Expect(nav, items, 1, -1, 0, 4, "Tab entry changed horizontal navigation", layer);
        Expect(nav, items, 4, 0, 1, 3, "Explicit entry overrode another tab's geometry", layer);
        const auto original = items;
        items[Index(items,2)].layer = layer + 1;
        nav.Reset();
        Expect(nav, items, 1, 0, 1, 3, "Tab entry escaped into another modal layer", layer);
        items = original;
        items[Index(items,2)].wx1 = 400 * scale;
        nav.Reset();
        Expect(nav, items, 1, 0, 1, 3, "Tab entry escaped into another window", layer);
        items = original;
        items[Index(items,2)].clipY1 = 70 * scale;
        nav.Reset();
        Expect(nav, items, 1, 0, 1, 3, "Tab entry selected a clipped toggle", layer);
        items = original;
        std::erase_if(items, [](const auto& item) { return item.id == 2; });
        nav.Reset();
        Expect(nav, items, 1, 0, 1, 3, "Removed tab entry prevented ordinary navigation", layer);
    }
}

static void CheckShortNeighboringLists()
{
    for (float scale : {0.5f, 2.0f / 3.0f, 1.0f, 1.2f}) {
        for (bool mirror : {false, true}) {
            for (int layer : {0, 1, 2}) {
                auto first = Item(1, 20, 100, 180);
                auto last = Item(2, 20, 130, 180);
                for (auto* row : {&first, &last}) {
                    row->wx0 = 10; row->wx1 = 210;
                    row->wy0 = 90; row->wy1 = 160;
                }
                auto hidden = last; hidden.id = 3; hidden.y0 = 175; hidden.y1 = 195;
                auto resetAll = Item(4, 20, 175, 150);
                auto upperSlider = Item(5, 240, 290, 600, 28);
                auto lowerSlider = Item(6, 240, 390, 600, 28);
                auto upperReset = Item(7, 380, 265, 60);
                auto lowerReset = Item(8, 380, 365, 60);
                std::vector items{first, last, hidden, resetAll, upperSlider, lowerSlider, upperReset, lowerReset};
                for (auto& item : items) {
                    item.layer = layer;
                    if (mirror) {
                        const float x0 = item.x0, wx0 = item.wx0;
                        item.x0 = 1000 - item.x1; item.x1 = 1000 - x0;
                        item.wx0 = 1000 - item.wx1; item.wx1 = 1000 - wx0;
                    }
                    item.x0 = item.x0 * scale + 73; item.x1 = item.x1 * scale + 73;
                    item.wx0 = item.wx0 * scale + 73; item.wx1 = item.wx1 * scale + 73;
                    item.y0 = item.y0 * scale + 31; item.y1 = item.y1 * scale + 31;
                    item.wy0 = item.wy0 * scale + 31; item.wy1 = item.wy1 * scale + 31;
                }
                std::reverse(items.begin(), items.end());
                for (const auto slider : {5u, 6u}) {
                    DirectionalNavigation nav;
                    Expect(nav, items, slider, mirror ? 1 : -1, 0, 2,
                        "A slider below a short list could not enter that list", layer);
                    Expect(nav, items, 2, mirror ? -1 : 1, 0, slider,
                        "Returning from a short list lost the original slider row", layer);
                    Expect(nav, items, slider, 0, -1, slider + 2,
                        "Short-list entry changed vertical slider navigation", layer);
                }
                // A real nearby control on the pressed row beats a short
                // pane above it. Removing it restores access to that pane.
                auto neighbor = items[Index(items, 6)];
                neighbor.id = 9;
                neighbor.x0 = (mirror ? 820 : 120) * scale + 73;
                neighbor.x1 = (mirror ? 880 : 180) * scale + 73;
                items.push_back(neighbor);
                DirectionalNavigation nav;
                Expect(nav, items, 6, mirror ? 1 : -1, 0, 9,
                    "A short list stole focus from the closer control on the pressed row", layer);
                items.pop_back();
                nav.Reset();
                Expect(nav, items, 2, 0, -1, 1, "Up could not move within a short list", layer);
                Expect(nav, items, 1, mirror ? 1 : -1, 0, 0,
                    "Moving away from the editor should leave the list at its outside edge", layer);
            }
        }
    }
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
        CheckSpatialHeaders();
        CheckTabEntry();
        CheckShortNeighboringLists();
        CheckInlineResetButtons();
        CheckVariedGrids();
        CheckClippedList();
        std::cout << "Directional navigation checks passed (offsets, lanes, panes, list footers, layers, inline resets, 400 grids, 2,000-row list).\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
