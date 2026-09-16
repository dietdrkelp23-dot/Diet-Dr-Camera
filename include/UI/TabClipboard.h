#pragma once

#include <string>
#include <utility>
#include <vector>

namespace DietDrCamera
{
    // Location lists can be reordered by loading another preset after Copy.
    // Rewrite every captured index through stable identity matches; -1 skips a
    // missing or ambiguous place rather than applying its tuning elsewhere.
    template <class Slots>
    void RemapTabLocationSlots(Slots& slots, const std::vector<int>& destinations)
    {
        for (auto& slot : slots)
            slot.locIdx = slot.locIdx >= 0 && static_cast<std::size_t>(slot.locIdx) < destinations.size() ?
                destinations[static_cast<std::size_t>(slot.locIdx)] : -1;
    }
    // Match by meaning, never by position: tabs can have different sub-states.
    // Ambiguous identities are skipped instead of choosing an arbitrary row.
    inline std::vector<std::pair<std::size_t, std::size_t>> MatchTabEntries(
        const std::vector<std::string>& source, const std::vector<std::string>& destination)
    {
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        for (std::size_t d = 0; d < destination.size(); ++d) {
            if (destination[d].empty()) continue;
            std::size_t index = 0, count = 0, destinationCount = 0;
            for (std::size_t s = 0; s < source.size(); ++s)
                if (source[s] == destination[d]) { index = s; ++count; }
            for (const auto& key : destination) if (key == destination[d]) ++destinationCount;
            if (count == 1 && destinationCount == 1) matches.emplace_back(index, d);
        }
        return matches;
    }
}
