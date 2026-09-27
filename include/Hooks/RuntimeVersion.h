#pragma once
#include <array>
#include <cstdint>

namespace DietDrCamera::RuntimeVersion
{
    using Version = std::array<std::uint16_t, 4>;
    // Supported non-VR targets start at SE 1.5.97. Earlier SE releases are
    // excluded because the required menu framework cannot load on 1.5.73.
    // New executables
    // need a layout review even when their function IDs remain unchanged.
    inline constexpr std::array supported{
        Version{1,5,97,0},
        Version{1,6,317,0}, Version{1,6,318,0}, Version{1,6,323,0}, Version{1,6,342,0},
        Version{1,6,353,0}, Version{1,6,629,0}, Version{1,6,640,0}, Version{1,6,659,0},
        Version{1,6,1130,0}, Version{1,6,1170,0}, Version{1,6,1179,0},
        Version{1,7,99,0}, Version{1,7,104,0}
    };

    constexpr bool IsKnown(Version version)
    {
        for (const auto& known : supported) if (known == version) return true;
        return false;
    }
}
