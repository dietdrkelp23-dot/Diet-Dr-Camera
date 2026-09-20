#pragma once

namespace DietDrCamera
{
    struct ProjectileTracingSettings
    {
        bool archeryEnabled = false;
        bool spellEnabled = false;
        float reticleSize = 1.0f;
        float reticleThickness = 2.0f;
        float sneakEyeX = -500.0f;
        float sneakEyeY = -100.0f;
        bool operator==(const ProjectileTracingSettings&) const = default;
        bool Enabled() const { return archeryEnabled || spellEnabled; }
    };
}
