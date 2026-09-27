#pragma once

#include <algorithm>
#include <cmath>

namespace DietDrCamera::CombatFOV
{
    inline constexpr float kMinFOV = 50.0f, kMaxFOV = 140.0f;
    inline constexpr float kMinSpeed = 0.05f, kMaxSpeed = 10.0f;
    struct Tuning {
        bool enabled = false;
        float fov = 80.0f, transitionSpeed = 1.0f;
        bool operator==(const Tuning&) const = default;
    };
    inline Tuning Sanitize(Tuning p)
    {
        p.fov = std::isfinite(p.fov) ? std::clamp(p.fov,kMinFOV,kMaxFOV) : Tuning{}.fov;
        p.transitionSpeed = std::isfinite(p.transitionSpeed)
            ? std::clamp(p.transitionSpeed,kMinSpeed,kMaxSpeed) : Tuning{}.transitionSpeed;
        return p;
    }

    // dt is real elapsed time. Only our editor may advance a paused effect;
    // selecting this effect also previews combat without starting a fight.
    struct Frame {
        bool inCombat = false;
        bool paused = false;
        bool editorOpen = false;
        bool preview = false;
        float dt = 0.0f;
    };

    class Envelope
    {
    public:
        void Reset() { active = false; value = Tuning{}.fov; }
        float Hold(float normalFOV) const { return active ? value : SafeFOV(normalFOV); }
        float Update(Tuning tuning, float normalFOV, const Frame& frame)
        {
            if (frame.paused && !frame.editorOpen) return Hold(normalFOV);
            // Native menus can stop the camera writer altogether. Do not jump
            // ahead on the first frame after such a gap.
            const float dt = frame.dt < 0 || frame.dt > .25f ? 0.0f : frame.dt;
            return Step(tuning, normalFOV,
                frame.inCombat || (frame.editorOpen && frame.preview), dt);
        }
        float Step(Tuning tuning, float normalFOV, bool inCombat, float dt)
        {
            tuning = Sanitize(tuning);
            normalFOV = SafeFOV(normalFOV);
            const bool combat = tuning.enabled && inCombat;
            if (!active) {
                if (!combat) return normalFOV;
                value = normalFOV;
                active = true;
            }
            // Follow the currently resolved normal profile on exit: drawing a
            // weapon or changing perspective must not restore an old snapshot.
            const float target = combat ? tuning.fov : normalFOV;
            if (std::isfinite(dt) && dt > 0) {
                const float alpha = -std::expm1(-6.0f*tuning.transitionSpeed*(std::min)(dt,.05f));
                value += (target-value)*alpha;
                if (std::abs(value-target) < .001f) {
                    value = target;
                    if (!combat) active = false;
                }
            }
            return value;
        }
    private:
        static float SafeFOV(float fov)
        {
            return std::isfinite(fov) ? std::clamp(fov,1.0f,179.0f) : Tuning{}.fov;
        }
        bool active = false;
        float value = Tuning{}.fov;
    };
}
