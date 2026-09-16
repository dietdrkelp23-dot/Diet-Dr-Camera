#pragma once
#include <toml++/toml.hpp>
#include <stdexcept>
#include <string>

namespace DietDrCamera
{
    inline void MergePresetTable(toml::table& into, const toml::table& from, const std::string& path)
    {
        for (const auto& [key, value] : from) {
            if (auto* existing = into.get(key.str())) {
                if (existing->is_table() && value.is_table())
                    MergePresetTable(*existing->as_table(), *value.as_table(), path + "." + std::string(key.str()));
                else
                    throw std::logic_error("Preset key collision: " + path + "." + std::string(key.str()));
            } else {
                into.insert(key, value);
            }
        }
    }

    inline void InsertPresetTable(toml::table& root, std::string_view path, const toml::table& value)
    {
        if (value.empty()) return;
        auto* current = &root;
        size_t start = 0;
        for (;;) {
            const auto dot = path.find('.', start);
            const auto key = path.substr(start, dot == path.npos ? dot : dot - start);
            auto* node = current->get(key);
            if (!node) {
                current->insert(key, toml::table{});
                node = current->get(key);
            }
            if (!node->is_table()) throw std::logic_error("Preset key collision: " + std::string(path));
            current = node->as_table();
            if (dot == path.npos) break;
            start = dot + 1;
        }
        MergePresetTable(*current, value, std::string(path));
    }
}
