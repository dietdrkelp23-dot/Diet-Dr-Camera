#pragma once
#include "Camera/AnimationCatalog.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace DietDrCamera
{
    inline std::uint32_t FindAnimationBinding(const std::unordered_map<std::string, std::uint32_t>& index,
        const std::string& key, const std::string& familyKey)
    {
        for (const auto& candidate : {key, LegacyAnimationPathKey(key), familyKey, LegacyAnimationPathKey(familyKey)})
            if (const auto found = index.find(candidate); found != index.end()) return found->second;
        return 0;
    }
    struct ActiveAnimationMatch { const void* clip; std::uint32_t uid; };

    inline std::uint32_t UpdateAnimationMatches(std::vector<ActiveAnimationMatch>& active,
        const void* clip, std::uint32_t uid, bool refresh)
    {
        const auto found = std::find_if(active.begin(), active.end(), [&](const auto& a) {
            return a.clip == clip && a.uid == uid;
        });
        if (!uid || !refresh || found == active.end()) {
            std::erase_if(active, [&](const auto& a) { return a.clip == clip; });
            if (uid) active.push_back({clip, uid});
        }
        return active.empty() ? 0 : active.back().uid;
    }
}
