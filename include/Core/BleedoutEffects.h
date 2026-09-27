#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace DietDrCamera::BleedoutEffects
{
    enum class Event { None, Ragdoll, Death };

    // The event, rather than a camera state, owns the envelope. Repeated camera
    // requests, menu returns and changing the camera toggle cannot restart it.
    class SlowMotion
    {
    public:
        bool Enter(Event event, float strength, float duration)
        {
            if (event == event_) return false;
            Reset();
            event_ = event;
            if (event == Event::None || !std::isfinite(strength) || !std::isfinite(duration) ||
                strength <= 0.001f || duration <= 0.001f) return true;
            strength_ = std::clamp(strength / 100.0f, 0.0f, 0.9f);
            duration_ = (std::max)(duration, 0.5f);
            active_ = true;
            return true;
        }

        void Reset() { *this = {}; }
        void RequestFade() { fadeRequested_ = active_; }
        bool Active() const { return active_; }
        Event CurrentEvent() const { return event_; }

        // nullopt means we do not own time. Completion emits one final 1.0,
        // then stays idle until a new event. Menus pause both envelope edges.
        std::optional<float> Advance(float dt, bool paused)
        {
            if (!active_) return std::nullopt;
            if (paused) return 1.0f - amount_ * strength_;
            dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
            const float rampOut = std::clamp(duration_ * 0.35f, 0.10f, 0.80f);
            if (fadeRequested_) {
                fadeRequested_ = false;
                if (!fading_) {
                    fading_ = true;
                    fadeFrom_ = amount_;
                }
            }
            elapsed_ += dt;
            if (fading_) {
                fadeElapsed_ += dt;
                amount_ = fadeFrom_ * Smooth(1.0f - fadeElapsed_ / rampOut);
            } else {
                const float rampIn = std::clamp(duration_ * 0.12f, 0.05f, 0.20f);
                if (elapsed_ < rampIn) {
                    const float x = elapsed_ / rampIn;
                    amount_ = 1.0f - (1.0f - x) * (1.0f - x);
                } else if (elapsed_ > duration_ - rampOut) {
                    amount_ = Smooth((duration_ - elapsed_) / rampOut);
                } else {
                    amount_ = 1.0f;
                }
            }
            if ((!fading_ && elapsed_ >= duration_) ||
                (fading_ && fadeElapsed_ >= rampOut)) {
                active_ = false;
                amount_ = 0.0f;
            }
            return std::clamp(1.0f - amount_ * strength_, 0.1f, 1.0f);
        }

    private:
        static float Smooth(float x)
        {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
        }
        Event event_ = Event::None;
        bool active_ = false;
        bool fadeRequested_ = false;
        bool fading_ = false;
        float elapsed_ = 0.0f;
        float strength_ = 0.0f;
        float duration_ = 0.0f;
        float amount_ = 0.0f;
        float fadeFrom_ = 0.0f;
        float fadeElapsed_ = 0.0f;
    };

}
