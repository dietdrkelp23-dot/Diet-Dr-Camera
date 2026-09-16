#include "UI/MenuLayout.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static bool Near(float a, float b)
{
    return std::abs(a - b) < 0.01f;
}

struct Display { float width, height, scale; };
static constexpr std::array kDisplays{
    Display{1280, 720, 0.5f}, Display{1920, 1080, 0.5f},
    Display{2560, 1440, 2.0f / 3.0f}, Display{3440, 1440, 2.0f / 3.0f},
    Display{5120, 1440, 2.0f / 3.0f}, Display{3840, 2160, 1.0f},
    Display{5120, 2880, 1.2f}, Display{7680, 4320, 1.2f}};

static void CheckMeasurementsAcrossResolutions()
{
    for (const auto& first : kDisplays) {
        Require(Near(MenuScaleForDisplayHeight(first.height), first.scale),
            "Menu scale changed with display height or readability limits");
        for (float authoredSize : {0.0f, 355.0f, 360.0f, 520.0f, 1100.0f}) {
            // File-scope caches start before the display is known. Opening a
            // different page first must not leave them at their DLL-load size.
            ScaledMenuSize size(authoredSize);
            Require(Near(size.Pixels(first.scale), authoredSize * first.scale),
                "First use retained an unscaled layout measurement");
            // Child-window measurements and user resizing write pixels back.
            const float editedAt4K = authoredSize + 87.0f;
            size.Pixels(first.scale) = editedAt4K * first.scale;
            for (int pass = 0; pass < 20; ++pass) {
                for (const auto& next : kDisplays) {
                    const float scale = MenuScaleForDisplayHeight(next.height);
                    Require(Near(size.Pixels(scale), editedAt4K * next.scale),
                        "Resolution change lost an edit or retained old pixels");
                    Require(Near(size.Pixels(scale), editedAt4K * next.scale),
                        "Drawing another frame applied the scale twice");
                }
            }
            Require(Near(size.Pixels(first.scale), editedAt4K * first.scale),
                "Resolution round trip drifted from the edited measurement");
        }
    }
    ScaledMenuSize dormant(520.0f), active(360.0f);
    dormant.Pixels(0.5f) = 310.0f;
    active.Pixels(1.0f) = 480.0f;
    active.Pixels(2.0f / 3.0f);
    Require(Near(dormant.Pixels(1.0f), 620.0f),
        "A page skipped during resolution changes lost its own measurement scale");
    Require(Near(active.Pixels(1.0f), 480.0f),
        "Visiting another page changed the active page's measured width");
}

static void CheckFloatingPanelBounds()
{
    for (const auto& display : kDisplays) {
        const float margin = 16.0f * display.scale;
        const MenuWindowBounds aligned{800.0f * display.scale, 40.0f * display.scale,
            600.0f * display.scale, 500.0f * display.scale};
        const auto normal = FitMenuWindow(aligned, display.width, display.height, margin);
        Require(Near(normal.x, aligned.x) && Near(normal.y, aligned.y) &&
            Near(normal.width, aligned.width) && Near(normal.height, aligned.height),
            "A panel that fits lost its alignment or size");

        // The lower Quick Tune section may be partly below the viewport, and
        // its transition panel includes extra rows beyond that section's end.
        for (float originY : {-300.0f, 0.0f, display.height - 40.0f, display.height + 300.0f}) {
            for (float height : {aligned.height, display.height * 2.0f}) {
                const auto fitted = FitMenuWindow({aligned.x, originY, aligned.width, height},
                    display.width, display.height, margin);
                Require(fitted.x >= margin && fitted.y >= margin &&
                    fitted.x + fitted.width <= display.width - margin + 0.01f &&
                    fitted.y + fitted.height <= display.height - margin + 0.01f,
                    "A scrolled or tall transition panel leaves controls off screen");
                if (height == aligned.height) {
                    Require(Near(fitted.height, height), "An ordinary panel was needlessly shortened");
                } else {
                    Require(fitted.height < height && Near(fitted.y, margin) &&
                        Near(fitted.y + fitted.height, display.height - margin),
                        "An oversized panel did not use the full scrollable viewport");
                }
            }
        }
        const auto wide = FitMenuWindow({display.width + 10.0f, 40.0f, display.width * 2.0f, 200.0f},
            display.width, display.height, margin);
        Require(Near(wide.x, margin) && Near(wide.x + wide.width, display.width - margin),
            "A wide floating panel overflows the display");
    }
}

static void CheckNpcNoiseScrolling()
{
    for (const auto& display : kDisplays) {
        // Both POV groups now contain five sliders. Their content can be
        // taller than the visible pane, especially after resizing the host.
        const float contentHeight = 1200.0f * display.scale;
        const float lastControlHeight = 30.0f * display.scale;
        for (float available : {96.0f * display.scale, 280.0f * display.scale,
                                display.height * 0.6f, display.height * 0.85f}) {
            const float body = MenuScrollableBodyHeight(available, 8.0f * display.scale);
            Require(body > 0 && body <= available,
                "Cinematic child extends below its parent and clips its scroll destination");
            const float maxScroll = std::max(0.0f, contentHeight - body);
            const float finalBottom = contentHeight - maxScroll;
            Require(finalBottom <= available && finalBottom - lastControlHeight >= 0,
                "Last NPC slider/reset cannot be fully reached at maximum scroll");
        }
    }
    Require(Near(MenuScrollableBodyHeight(0, 8), 1) &&
            Near(MenuScrollableBodyHeight(-20, 8), 1),
        "Collapsed host produces zero/negative ImGui child dimensions");
}

int main()
{
    try {
        CheckMeasurementsAcrossResolutions();
        CheckFloatingPanelBounds();
        CheckNpcNoiseScrolling();
        std::cout << "Menu layout checks passed (720p through 8K, ultrawide, resolution changes, floating panels)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Menu layout check failed: " << e.what() << '\n';
        return 1;
    }
}
