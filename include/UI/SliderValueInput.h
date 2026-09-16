#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace DietDrCamera
{
    inline std::optional<float> ParseSliderValue(std::string_view text, float minimum, float maximum)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos || !std::isfinite(minimum) ||
            !std::isfinite(maximum) || minimum > maximum) return std::nullopt;
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        if (text.starts_with('+')) {
            text.remove_prefix(1);
            if (text.starts_with('-') || text.starts_with('+')) return std::nullopt;
        }
        if (text.empty()) return std::nullopt;
        float value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            !std::isfinite(value)) return std::nullopt;
        return std::clamp(value, minimum, maximum);
    }
}
