#pragma once

#include <optional>

namespace DietDrCamera
{
    // Effects can finish after a preset reload or an editor deletes their
    // source entry. Retain the authored values without retaining that storage.
    template <class Profile>
    [[nodiscard]] std::optional<Profile> SnapshotProfile(const Profile* source)
    {
        if (source) return *source;
        return std::nullopt;
    }
}
