#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace DietDrCamera::CameraStateEntry
{
    // Reviewed across the supported SE/AE images. The displaced five bytes are
    // one position-independent MOV [RSP+8],RBX, before any stack adjustment.
    inline constexpr std::array<std::uint8_t, 16> prologue{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9,0x48,0x8B,0xDA};
    inline std::optional<std::array<std::uint8_t, 19>> Gateway(
        std::span<const std::uint8_t> code, std::uintptr_t continuation)
    {
        if (!continuation || code.size() < prologue.size() ||
            !std::equal(prologue.begin(), prologue.end(), code.begin())) return std::nullopt;
        std::array<std::uint8_t, 19> result{0x48,0x89,0x5C,0x24,0x08,0xFF,0x25,0,0,0,0};
        std::memcpy(result.data() + 11, &continuation, sizeof(continuation));
        return result;
    }
}
