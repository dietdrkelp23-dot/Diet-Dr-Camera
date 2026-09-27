#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DietDrCamera
{
    enum class EntryClipKind { None, Camera, Noise, FirstPerson,
                               Transitions, LocationOv, WeaponOv, ShoutSet,
                               ShoutNoiseSet,   // a state's whole shout NOISE block (key-listed)
                               DialogueLook,    // one dialogue look (bucket+index)
                               FpWeaponOv,      // FP weapon-type override set (button)
                               FpLocationOv,    // FP per-location slots for one state key (button)
                               NoiseWeaponOv,   // noise weapon-type override set (button)
                               EnemyOv,         // one entry's whole enemy-override column (button)
                               FxBeat,          // one Transformations beat row (Bats / Transformation / Revert)
                               CineSource,      // one Cinematic Effects creature shake (a Dragon / Centurion source)
                               EnvHalf, Scalar, Tab };  // one environment, or a complete tab with overrides

    // Single-entry tab pastes use the same cross-kind routes as an individual
    // row. Whole-tab pastes retain their separate identity-matching behavior.
    inline bool CanBroadcastEntryToTab(EntryClipKind source, EntryClipKind destination)
    {
        if (source == EntryClipKind::None || source == EntryClipKind::Tab ||
            destination == EntryClipKind::None || destination == EntryClipKind::Tab) return false;
        if (source == destination) return true;
        if (destination == EntryClipKind::Camera)
            return source == EntryClipKind::Transitions || source == EntryClipKind::LocationOv ||
                   source == EntryClipKind::EnemyOv;
        return source == EntryClipKind::Camera && destination == EntryClipKind::ShoutSet;
    }

    // Snapshot once, before any destination is changed. In particular, copying
    // from an entry in this same tab must not let a paste alter later sources.
    // Keys and row order deliberately do not restrict a single-entry broadcast.
    template <class Entry, class Targets, class Compatible, class Apply>
    std::size_t BroadcastEntryToTab(const Entry& entry, const Targets& targets,
                                  Compatible compatible, Apply apply)
    {
        const auto snapshot = entry;
        std::size_t applied = 0;
        for (const auto& target : targets)
            if (compatible(snapshot, target) && apply(snapshot, target)) ++applied;
        return applied;
    }

    // These labels describe the equivalent base row in different category tabs.
    // Keep full transformation names and every other sub-state distinct.
    inline std::string MakeTabEntryKey(std::string_view label, int environment)
    {
        if (label == "Sheathed") label = "Unsheathed";
        return std::string(label) + "/" + std::to_string(environment);
    }

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
