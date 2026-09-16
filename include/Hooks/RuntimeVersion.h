#pragma once
#include <array>
#include <cstdint>

namespace DietDrCamera::RuntimeVersion
{
    using Version = std::array<std::uint16_t, 4>;
    // Known non-VR releases with SKSE/Address Library support. New executables
    // need a layout review even when their function IDs remain unchanged.
    inline constexpr std::array supported{
        Version{1,5,3,0}, Version{1,5,16,0}, Version{1,5,23,0}, Version{1,5,39,0},
        Version{1,5,50,0}, Version{1,5,53,0}, Version{1,5,62,0}, Version{1,5,73,0},
        Version{1,5,80,0}, Version{1,5,97,0},
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
