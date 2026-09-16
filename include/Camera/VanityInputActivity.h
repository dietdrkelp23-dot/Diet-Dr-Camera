#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace DietDrCamera
{
    // Keep input-thread activity separate from camera-thread mode ownership.
    // Look resets the entry timer, but only actions end an active vanity view.
    class VanityInputActivity
    {
    public:
        struct Frame {
            bool any{};
            bool endsVanity{};
        };

        void RecordLook() { pending.fetch_or(kLook, std::memory_order_relaxed); }
        void RecordAction() { pending.fetch_or(kAction, std::memory_order_relaxed); }
        void RecordThumbstick(float x, float y, bool rightStick, std::string_view userEvent)
        {
            if (!(std::hypot(x, y) > 0.25f)) return;
            // Respect mapped Look/Move events, falling back to the physical
            // stick when another input provider leaves the event unnamed.
            if (userEvent == "Look" || (userEvent != "Move" && rightStick)) RecordLook();
            else RecordAction();
        }
        [[nodiscard]] Frame Consume()
        {
            const auto bits = pending.exchange(0, std::memory_order_relaxed);
            return {bits != 0, (bits & kAction) != 0};
        }
        void Reset() { pending.store(0, std::memory_order_relaxed); }

    private:
        static constexpr std::uint8_t kLook = 1, kAction = 2;
        std::atomic<std::uint8_t> pending{0};
    };
}
