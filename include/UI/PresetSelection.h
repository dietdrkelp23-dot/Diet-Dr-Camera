#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace DietDrCamera
{
    // The detail card follows preset identity, never a cached row index. Saving
    // as new, renaming, hotkey selection and list sorting can all move the row.
    class PresetSelection
    {
    public:
        void Select(std::string_view name) { _selected = name; }

        int Resolve(const std::vector<std::string>& presets, std::string_view active)
        {
            if (_lastActive != active) {
                _lastActive = active;
                _selected = active;
            }
            const auto indexOf = [&](std::string_view name) {
                for (std::size_t i = 0; i < presets.size(); ++i) {
                    if (presets[i] == name) return static_cast<int>(i);
                }
                return -1;
            };
            if (const int index = indexOf(_selected); index >= 0) return index;
            _selected = active;
            return indexOf(_selected);
        }

    private:
        std::string _selected;
        std::string _lastActive;
    };
}
