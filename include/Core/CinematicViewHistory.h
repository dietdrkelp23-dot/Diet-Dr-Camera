#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace DietDrCamera::CinematicViews
{
    using History = std::unordered_set<std::string>;
    // Compact, bounded strings instead of form IDs: encounter identity survives
    // load-order changes and is independent of which preset is currently active.
    inline std::vector<std::uint8_t> EncodeHistory(const History& history)
    {
        std::vector<std::uint8_t> bytes;
        std::size_t count = 0;
        for (const auto& id : history) {
            if (count >= 4096) break;
            if (id.empty() || id.size() > 64 || bytes.size() + id.size() + 1 > 266240) continue;
            bytes.push_back(static_cast<std::uint8_t>(id.size()));
            bytes.insert(bytes.end(),id.begin(),id.end());
            ++count;
        }
        return bytes;
    }
    inline bool DecodeHistory(std::span<const std::uint8_t> bytes, History& history)
    {
        if (bytes.size() > 266240) return false;
        History parsed;
        for (std::size_t pos = 0; pos < bytes.size();) {
            const auto size = bytes[pos++];
            if (size == 0 || size > 64 || size > bytes.size()-pos || parsed.size() >= 4096) return false;
            std::string id(reinterpret_cast<const char*>(bytes.data()+pos),size);
            if (id.find('\0') != std::string::npos || !parsed.insert(std::move(id)).second) return false;
            pos += size;
        }
        history = std::move(parsed);
        return true;
    }
}
