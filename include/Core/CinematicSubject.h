#pragma once
#include "Core/CinematicViews.h"
#include <cctype>
#include <string_view>

namespace DietDrCamera::CinematicSubject
{
    using CinematicViews::Point;
    inline constexpr float kRange = 12000;
    inline constexpr float kSceneryRange = 1048576;
    inline bool RayMeetsBound(Point origin, Point direction, Point center, float radius, float range = kRange)
    {
        if (!origin.Finite() || !direction.Finite() || !center.Finite() || !std::isfinite(radius) || radius <= 0 ||
            !std::isfinite(range) || range <= 0) return false;
        const float length = direction.Length();
        if (length < .001f) return false;
        direction = direction * (1 / length);
        const auto offset = center - origin;
        const float along = offset.x*direction.x + offset.y*direction.y + offset.z*direction.z;
        const float closest = std::clamp(along,0.0f,range);
        return (offset-direction*closest).Length() <= radius;
    }
    inline std::optional<float> ForwardHitDistance(Point origin, Point direction, Point hit, float range)
    {
        if (!origin.Finite() || !direction.Finite() || !hit.Finite() || !std::isfinite(range) || range <= 0) return {};
        const auto offset = hit-origin;
        const float distance = offset.Length();
        const float along = offset.x*direction.x+offset.y*direction.y+offset.z*direction.z;
        if (!std::isfinite(distance) || distance <= .01f || distance > range || along <= 0) return {};
        return distance;
    }
    inline std::string Fold(std::string_view text)
    {
        std::string result(text);
        for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }
    inline std::string ModelName(std::string_view path)
    {
        const auto slash = path.find_last_of("/\\");
        auto stem = path.substr(slash == std::string_view::npos ? 0 : slash+1);
        if (const auto dot = stem.find_last_of('.'); dot != std::string_view::npos) stem = stem.substr(0,dot);
        std::string name;
        for (std::size_t i=0; i<stem.size(); ++i) {
            const auto c = static_cast<unsigned char>(stem[i]);
            if (c == '_') { name += ' '; continue; }
            if (i && std::isupper(c) && std::islower(static_cast<unsigned char>(stem[i-1]))) name += ' ';
            name += static_cast<char>(c);
        }
        return name.empty() ? "Unnamed Object" : name;
    }
}
