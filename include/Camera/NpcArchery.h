#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace DietDrCamera::NpcNoise
{
    inline bool FreshArcheryShot(float age) { return age >= 0.0f && age < 0.5f; }

    // Projectile callbacks may run off the camera thread. Only value records
    // cross this bounded queue; actor handles are resolved by the consumer.
    template <class Shot, std::size_t Capacity = 32>
    class ArcheryShotQueue
    {
    public:
        void SetActive(bool active)
        {
            _active.store(active);
            if (!active) {
                std::scoped_lock lock(_mutex);
                _count = 0;
                _recent.fill(0);
                _nextRecent = 0;
            }
        }
        bool Push(const Shot& shot, std::uint32_t projectile = 0)
        {
            if (!_active.load()) return false;
            std::scoped_lock lock(_mutex);
            if (!_active.load() || _count == Capacity) return false;
            // kMoved may remain clear across multiple projectile updates.
            // Keep identities across drains so those updates arm only once.
            if (projectile) {
                if (std::find(_recent.begin(), _recent.end(), projectile) != _recent.end()) return false;
                _recent[_nextRecent++ % Capacity] = projectile;
            }
            _shots[_count++] = shot;
            return true;
        }
        std::size_t Drain(std::array<Shot, Capacity>& out)
        {
            std::scoped_lock lock(_mutex);
            const auto count = _count;
            std::copy_n(_shots.begin(), count, out.begin());
            _count = 0;
            return count;
        }
    private:
        std::atomic<bool> _active{false};
        std::mutex _mutex;
        std::array<Shot, Capacity> _shots{};
        std::array<std::uint32_t, Capacity> _recent{};
        std::size_t _nextRecent = 0;
        std::size_t _count = 0;
    };
}
