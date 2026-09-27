#pragma once

#include "Core/Spring.h"
#include <algorithm>
#include <cmath>

namespace DietDrCamera
{
    // A half-turn must start from rest and retain velocity when the speaker
    // changes mid-sweep. A first-order exponential starts at maximum speed.
    inline void StepDialogueReverse(float& blend, float& velocity, bool reverse,
                                    float duration, float dt)
    {
        if (!(dt > 0.0f)) return;
        const float target = reverse ? 1.0f : 0.0f;
        const float omega = 4.75f / (std::max)(1.0f, duration); // ~95% at duration
        CriticalDampedSpringExact(blend, velocity, target, omega, (std::min)(dt, 0.1f));
        if (std::abs(target - blend) < 0.00001f && std::abs(velocity) < 0.00001f) {
            blend = target;
            velocity = 0.0f;
        }
    }

    // The reverse-shot anchor adds a lateral displacement outside the profile
    // side channel. Carry that displacement into gameplay instead of dropping
    // it on the close frame. Its clock is independent of profile retargets.
    class DialogueSideExit
    {
    public:
        struct Offsets { float expected; float actual; };

        void Reset() noexcept { *this = {}; }

        void Capture(float expected, float actual, float baseline, float duration) noexcept
        {
            m_expected = expected - baseline;
            m_actual = actual - baseline;
            m_duration = (std::max)(0.001f, duration);
            m_elapsed = 0.0f;
        }

        [[nodiscard]] Offsets Step(float baseline, float dt) noexcept
        {
            if (m_duration <= 0.0f) return { baseline, baseline };
            const float t = std::clamp(m_elapsed / m_duration, 0.0f, 1.0f);
            const float fade = 1.0f - t*t*t*(t*(t*6.0f - 15.0f) + 10.0f);
            const Offsets result{ baseline + m_expected * fade, baseline + m_actual * fade };
            // Sample before advancing: the close frame exactly retains both
            // engine offsets, even if the profile channel has already stepped.
            m_elapsed += (std::max)(0.0f, dt);
            if (t >= 1.0f) Reset();
            return result;
        }

    private:
        float m_expected = 0.0f;
        float m_actual = 0.0f;
        float m_duration = 0.0f;
        float m_elapsed = 0.0f;
    };
}
