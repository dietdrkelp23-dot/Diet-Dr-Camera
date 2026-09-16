#pragma once

#include <algorithm>

namespace DietDrCamera
{
    inline float MenuScaleForDisplayHeight(float height)
    {
        return std::clamp(height / 2160.0f, 0.5f, 1.2f);
    }

    inline float MenuScrollableBodyHeight(float availableHeight, float bottomGap)
    {
        return std::max(1.0f, availableHeight - bottomGap);
    }

    // Layout measurements survive between frames, but their pixels belong to
    // the resolution at which they were measured. Keep edits and measurements
    // proportional when a page is revisited at another resolution.
    class ScaledMenuSize
    {
    public:
        explicit ScaledMenuSize(float sizeAt4K) : pixels_(sizeAt4K) {}

        float& Pixels(float scale)
        {
            if (scale != scale_) {
                pixels_ *= scale / scale_;
                scale_ = scale;
            }
            return pixels_;
        }

    private:
        float pixels_;
        float scale_ = 1.0f;
    };

    struct MenuWindowBounds
    {
        float x, y, width, height;
    };

    // Preserve alignment with the owning control until a window would leave
    // the screen. Oversized windows use the available area and scroll inside.
    inline MenuWindowBounds FitMenuWindow(MenuWindowBounds bounds,
        float displayWidth, float displayHeight, float margin)
    {
        const float inset = std::clamp(margin, 0.0f,
            std::max(0.0f, (std::min(displayWidth, displayHeight) - 1.0f) * 0.5f));
        bounds.width = std::clamp(bounds.width, 1.0f, std::max(1.0f, displayWidth - inset * 2.0f));
        bounds.height = std::clamp(bounds.height, 1.0f, std::max(1.0f, displayHeight - inset * 2.0f));
        bounds.x = std::clamp(bounds.x, inset, std::max(inset, displayWidth - inset - bounds.width));
        bounds.y = std::clamp(bounds.y, inset, std::max(inset, displayHeight - inset - bounds.height));
        return bounds;
    }
}
