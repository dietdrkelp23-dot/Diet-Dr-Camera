#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace DietDrCamera
{
    // Identity, never value equality: a newly bound copy can have exactly the
    // same settings as its parent and still be the active edit target.
    template <class Profile, class Storage>
    std::optional<std::size_t> ProfileStorageIndex(const Profile* profile, const Storage& storage)
    {
        if (!profile || storage.empty()) return std::nullopt;
        const auto address = reinterpret_cast<std::uintptr_t>(profile);
        const auto first = reinterpret_cast<std::uintptr_t>(storage.data());
        if (address < first) return std::nullopt;
        const auto offset = address - first;
        if (offset % sizeof(Profile) || offset / sizeof(Profile) >= storage.size()) return std::nullopt;
        return offset / sizeof(Profile);
    }

    template <class Profile, class Storage, class Places, class Canonical>
    Profile* CanonicalLocationProfile(Profile* profile, const Storage& indoor,
                                      const Places& places, const Canonical& canonical)
    {
        const auto invert = [&](const Storage& storage) -> Profile* {
            const auto index = ProfileStorageIndex(profile, storage);
            return index && *index < canonical.size() ? canonical[*index] : nullptr;
        };
        if (auto* base = invert(indoor)) return base;
        for (const auto& place : places) {
            if (auto* base = invert(place.profiles)) return base;
            if (auto* base = invert(place.profilesIndoor)) return base;
        }
        return profile;
    }

    template <class Profile, class Chain, class Places>
    int ResolvedLocationProfileOwner(const Profile* profile, const Chain& chain,
                                     const Places& places, bool indoor)
    {
        if (!profile) return -1;
        for (int index : chain) {
            if (index < 0 || static_cast<std::size_t>(index) >= places.size()) continue;
            const auto& place = places[static_cast<std::size_t>(index)];
            if (!place.enabled) continue;
            const auto& profiles = indoor ? place.profilesIndoor : place.profiles;
            const auto& enabled = indoor ? place.profileSetIndoor : place.profileSet;
            if (const auto slot = ProfileStorageIndex(profile, profiles);
                slot && *slot < enabled.size() && enabled[*slot]) return index;
            // Specific weapons, animations and dialogue use stable keys rather
            // than the indexed Categories/Target Lock profile registry.
            for (const auto& [_, value] : place.bindingCam)
                if (&value == profile) return index;
            for (const auto& [_, value] : place.dlgLooks)
                if (&value == profile) return index;
        }
        return -1;
    }
}
