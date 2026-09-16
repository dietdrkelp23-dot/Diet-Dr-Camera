#pragma once

#include <algorithm>

namespace DietDrCamera
{
    enum class POVButtonState { Unknown, Released, Held };

    // A real POV hold must never be cancelled. Require a continuously released
    // physical binding AND stalled movement in ordinary gameplay before repair.
    class POVSlideRecovery
    {
    public:
        bool Update(float delta, bool slideActive, bool stalledGameplay, POVButtonState button)
        {
            if (!slideActive || !stalledGameplay || button != POVButtonState::Released) {
                _releasedTime = 0.0f;
                return false;
            }
            _releasedTime += (std::clamp)(delta, 0.0f, 0.05f);
            if (_releasedTime < 0.2f) return false;
            _releasedTime = 0.0f;
            return true;
        }

    private:
        float _releasedTime = 0.0f;
    };
}
