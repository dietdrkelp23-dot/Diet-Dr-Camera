#include "PCH.h"
#include "Camera/EventBeatSources.h"

#include <array>
#include <atomic>
#include <mutex>

namespace DietDrCamera::EventBeatSources
{
    namespace
    {
        std::mutex                sQueueLock;
        std::array<BeatEvent, 64> sQueue;
        std::size_t               sQueueCount = 0;
        std::atomic<bool>         sActive{ false };
        std::uint32_t             sFirstFireMask = 0;  // [BEAT] log-once per id
    }

    void SetActive(bool a_active)
    {
        sActive.store(a_active, std::memory_order_relaxed);
        if (!a_active) {
            std::scoped_lock lock(sQueueLock);
            sQueueCount = 0;
        }
    }

    void Enqueue(BeatId a_id, const RE::NiPoint3* a_pos, float a_scale, bool a_playerOwned)
    {
        if (a_id >= BeatId::kCount) return;
        if (!sActive.load(std::memory_order_relaxed)) return;
        const auto idx = static_cast<std::uint32_t>(a_id);
        std::scoped_lock lock(sQueueLock);
        if (!(sFirstFireMask & (1u << idx))) {
            sFirstFireMask |= 1u << idx;
            spdlog::debug("[BEAT] {} first event", BeatDefOf(a_id).tomlKey);
        }
        if (sQueueCount >= sQueue.size()) return;
        auto& e       = sQueue[sQueueCount++];
        e.id          = a_id;
        e.hasPos      = a_pos != nullptr;
        e.pos         = a_pos ? *a_pos : RE::NiPoint3{};
        e.scale       = a_scale;
        e.playerOwned = a_playerOwned;
    }

    std::size_t Drain(BeatEvent* a_out, std::size_t a_max)
    {
        if (!a_out || a_max == 0) return 0;
        std::scoped_lock lock(sQueueLock);
        const std::size_t n = std::min(a_max, sQueueCount);
        for (std::size_t i = 0; i < n; ++i) a_out[i] = sQueue[i];
        sQueueCount = 0;
        return n;
    }
}
