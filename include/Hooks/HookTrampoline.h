#pragma once

#include <cstddef>

namespace DietDrCamera::HookTrampoline
{
    // Five 14-byte call stubs plus the 14-byte camera-collision stub use 84 bytes.
    // Keep headroom for additional hooks, including the dormant 8-byte menu hook.
    inline constexpr std::size_t capacity = 256;

    // Called once before installing any DDC hooks. CommonLib NG initializes its
    // default trampoline only once; subsequent AllocTrampoline calls do not grow it.
    void Initialize();
}
