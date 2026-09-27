#pragma once
#include <algorithm>
#include <cmath>

namespace DietDrCamera::NativeGameplayLook
{
    struct Delta { double yaw{}, pitch{}; };

    // x/y have already passed Skyrim's LookHandler and NormalizeLook, including
    // mouse X/Y scales, invert Y, controller curves and acceleration. The two
    // pitch paths use different gains. Match the frame-independent orbit
    // pitch used by Engine Fixes / SSE Display Tweaks (1 / 42.5), instead of
    // reintroducing the vanilla extra dt and slowing vertical look at high FPS.
    // Actor/first-person uses fLookingSpeed * FOV scale. Heading is clockwise.
    inline Delta Rotation(float x, float y, float frameSeconds, bool orbit,
        float orbitSpeed, float actorTurnSpeed, float lookingSpeed, float fovScale)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(frameSeconds) ||
            frameSeconds <= 0.0f || frameSeconds > 0.25f) return {};
        const float yaw = x * (orbit ? orbitSpeed : actorTurnSpeed) * frameSeconds;
        const float pitch = orbit ? y * orbitSpeed / 42.5f : y * lookingSpeed * fovScale;
        return {std::isfinite(yaw) ? -yaw : 0.0, std::isfinite(pitch) ? pitch : 0.0};
    }

    // PlayerCamera::Update derives compass yaw from target heading + the
    // state's yaw offset, after TESCamera::Update has finished.
    inline float HeadingOffset(double viewYaw, float targetHeading)
    {
        return static_cast<float>(std::remainder(-viewYaw - targetHeading, 6.28318530717958647692));
    }
}
