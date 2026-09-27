#pragma once
#include <atomic>

namespace DietDrCamera
{
    // Restore or echo a HUD transform only after trajectory tracing owns it.
    // Merely entering target lock must not publish stale/default coordinates.
    class CrosshairOwnership
    {
    public:
        bool IsOverriding() const { return trajectory_.load(std::memory_order_acquire); }
        bool EchoTrajectory() const { return IsOverriding(); }
        void ClaimTrajectory() { trajectory_.store(true, std::memory_order_release); }
        bool ClearTrajectory() { return trajectory_.exchange(false, std::memory_order_acq_rel); }
    private:
        std::atomic<bool> trajectory_{false};
    };
}
