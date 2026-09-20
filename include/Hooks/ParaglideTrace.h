#pragma once

#include <chrono>

namespace RE { class TESCamera; class NiPoint3; }

namespace DietDrCamera::ParaglideTrace
{
    // Wraps the existing camera-update hook chain and the final noise pass.
    // Times are CPU wall time, not GPU present times or isolated engine cost.
    class Frame
    {
    public:
        explicit Frame(RE::TESCamera* camera);
        ~Frame();
        void AfterChain();
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;

    private:
        RE::TESCamera* camera_{};
        std::chrono::steady_clock::time_point start_{}, chainEnd_{};
        bool enabled_{};
    };

    void Follow(const RE::NiPoint3& engine, const RE::NiPoint3& rendered,
        float looseness, float lag, float rate, float flee, bool reset);
    void Wind(float ground, float altitude, float speed, bool teleportGuard);
}
