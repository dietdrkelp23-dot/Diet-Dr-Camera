#pragma once

#include "Core/Spring.h"

#include <algorithm>
#include <cstdint>

namespace DietDrCamera::FleeFraming
{
    inline float ApproachFactor(float speedTowardCamera)
    {
        if (!std::isfinite(speedTowardCamera)) return 0.0f;
        // Ignore idle motion, but reach full protection at an ordinary walking
        // approach instead of requiring sprint-like speed.
        const float a = std::clamp((speedTowardCamera - 40.0f) / 50.0f, 0.0f, 1.0f);
        return a * a * (3.0f - 2.0f * a);
    }

    inline void UpdateTighten(float& tighten, float& velocity, float target, float dt)
    {
        if (!std::isfinite(dt) || dt <= 0.0f) return;
        if (!std::isfinite(tighten) || !std::isfinite(velocity)) tighten = velocity = 0.0f;
        if (!std::isfinite(target)) target = 0.0f;
        target = std::clamp(target, 0.0f, 1.0f);
        const float omega = target > tighten ? 4.0f / 0.35f : 4.0f / 0.6f;
        CriticalDampedSpringExact(tighten, velocity, target, omega, std::clamp(dt, 0.0f, 0.1f));
        tighten = std::clamp(tighten, 0.0f, 1.0f);
    }

    inline float MomentumFactor(float movementSpeed)
    {
        if (!std::isfinite(movementSpeed)) return 0.0f;
        // Ordinary walking/running still releases at the approach threshold.
        // A movement shout can keep travelling much faster after a turn.
        const float a = std::clamp((movementSpeed - 800.0f) / 400.0f, 0.0f, 1.0f);
        return a * a * (3.0f - 2.0f * a);
    }

    template <class Point>
    void RetireHiddenLag(Point& laggedPlayer, const Point& player,
                         float previousTighten, float tighten)
    {
        const float previousFade = 1.0f - std::clamp(previousTighten, 0.0f, 1.0f);
        const float fade = 1.0f - std::clamp(tighten, 0.0f, 1.0f);
        if (fade <= previousFade) return;

        // Release should restore follow motion from the current visible offset,
        // not reveal the large trail accumulated behind a tightened camera.
        // Rebase the tracker by exactly the inverse change in the render fade:
        // newLag * newFade == oldLag * oldFade. This cannot introduce a snap,
        // including at partial strength; new movement rebuilds ordinary lag.
        const float retain = previousFade / fade;
        laggedPlayer = Point{player.x + (laggedPlayer.x - player.x) * retain,
                            player.y + (laggedPlayer.y - player.y) * retain,
                            player.z + (laggedPlayer.z - player.z) * retain};
    }

    // A movement shout has one windup edge and one launch edge. Its graph and
    // SKSE can both report the launch; later duplicate fire timestamps belong
    // to that same launch and must not cancel its own momentum protection.
    struct MovementImpulseTracker
    {
        std::uint64_t serial = 0;
        std::uint64_t lastStart = 0;
        bool fired = false;

        void Observe(std::uint64_t start, std::uint64_t fire)
        {
            if (start == 0) return;
            if (start != lastStart) {
                lastStart = start;
                fired = false;
                ++serial;
            }
            if (!fired && fire >= start) {
                fired = true;
                ++serial;
            }
        }
    };

    struct FollowState
    {
        float tighten = 0.0f;
        float velocity = 0.0f;
        float heldApproach = 0.0f;
        float smoothedSpeed = 0.0f;
        std::uint64_t lastMovementImpulse = 0;

        void Reset() { *this = {}; }

        template <class Point>
        void Update(Point& laggedPlayer, const Point& player,
                    float approachSpeed, float movementSpeed, float strength, float dt,
                    std::uint64_t movementImpulse = 0)
        {
            if (!std::isfinite(dt) || dt <= 0.0f) return;
            dt = std::min(dt, 0.1f);
            const bool newImpulse = movementImpulse != lastMovementImpulse;
            lastMovementImpulse = movementImpulse;
            if (newImpulse) {
                // A fresh movement shout must earn its own approach hold.
                // Keep the visible blend/spring so its old protection releases
                // smoothly; resetting those would bypass the hidden-lag rebase.
                heldApproach = 0.0f;
                smoothedSpeed = 0.0f;
            }
            strength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
            movementSpeed = std::isfinite(movementSpeed) ? std::clamp(movementSpeed, 0.0f, 6000.0f) : 0.0f;
            smoothedSpeed += (movementSpeed - smoothedSpeed) * (1.0f - std::exp(-dt / 0.25f));
            const float approach = ApproachFactor(approachSpeed);
            // Respond to a dash immediately, then ease the speed used for
            // release so its last fast frames cannot uncover the trail at once.
            const float momentum = MomentumFactor(std::max(movementSpeed, smoothedSpeed));
            if (momentum <= 0.0f || strength <= 0.001f) heldApproach = 0.0f;
            // The boundary frame's sampled velocity may still describe the
            // previous dash. It can drive ordinary approach framing, but must
            // not immediately re-arm the old momentum hold for the new dash.
            else if (!newImpulse) heldApproach = std::max(heldApproach, approach);

            // Speed alone never starts flee framing. Only an actual approach
            // during this fast movement can retain protection through its turn.
            const float previousTighten = tighten;
            UpdateTighten(tighten, velocity,
                strength * std::max(approach, heldApproach * momentum), dt);
            RetireHiddenLag(laggedPlayer, player, previousTighten, tighten);
        }
    };

    template <class Point>
    Point ApplyTighten(const Point& engineCamera, const Point& looseCamera, float tighten)
    {
        // Use only the spring output. An instantaneous distance floor after
        // this blend bypasses its easing when the approach threshold is crossed.
        const float fade = 1.0f - std::clamp(tighten, 0.0f, 1.0f);
        return Point{engineCamera.x + (looseCamera.x - engineCamera.x) * fade,
                     engineCamera.y + (looseCamera.y - engineCamera.y) * fade,
                     engineCamera.z + (looseCamera.z - engineCamera.z) * fade};
    }
}
