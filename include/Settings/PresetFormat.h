#pragma once

#include <toml++/toml.hpp>
#include <cmath>
#include <limits>

namespace DietDrCamera
{
    // Public storage contract starts here. Increment for new persisted fields
    // as well as changed meanings: older builds must not erase newer tuning.
    inline constexpr int kCurrentPresetFormat = 8;

    inline bool HasFinitePresetValues(const toml::node& node)
    {
        if (const auto* value = node.as_floating_point())
            return std::isfinite(value->get()) && std::abs(value->get()) <= (std::numeric_limits<float>::max)();
        if (const auto* array = node.as_array()) {
            for (const auto& item : *array) if (!HasFinitePresetValues(item)) return false;
        } else if (const auto* table = node.as_table()) {
            for (const auto& [_, item] : *table) if (!HasFinitePresetValues(item)) return false;
        }
        return true;
    }

    inline bool CanReadPreset(const toml::table& table)
    {
        if (!HasFinitePresetValues(table)) return false;
        if (table.contains("meta") && !table["meta"].is_table()) return false;
        const auto node = table["meta"]["format"];
        if (!node) return true;  // unstamped development files / global preferences
        if (!node.is_integer()) return false;
        const auto format = node.value<std::int64_t>();
        return format && *format >= 0 && *format <= kCurrentPresetFormat;
    }
}
