#pragma once

#include <algorithm>

namespace DietDrCamera
{
    struct EffectFrame
    {
        float envelopeDelta;
        float noiseDelta;

        // Both DDC menus hold action progress but keep sampling the noise.
        // Both views use the same split, so a paused duration never expires
        // merely because the user is watching or adjusting its texture.
        static constexpr EffectFrame FromRealDelta(float delta, bool paused, bool quickTuneOpen, bool mainMenuOpen = false)
        {
            const float real = delta > 0.0f ? std::min(delta, 0.05f) : 0.0f;
            const bool previewNoise = quickTuneOpen || mainMenuOpen;
            return {paused ? 0.0f : real, paused && !previewNoise ? 0.0f : real};
        }
    };
}
