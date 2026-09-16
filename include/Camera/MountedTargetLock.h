#pragma once

#include "Core/Spring.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace DietDrCamera
{
    // A small correction to the freshly composed engine/TDM rotation. Actor
    // pitch and freeRotation.y remain inputs, never storage for this filter.
    // Acquire/switch use their existing motion; only steady tracking rejects bob.
    class MountedTargetLock
    {
    public:
        struct Quaternion { float w, x, y, z; };
        static constexpr float kMaxCorrection = 0.0436332313f;  // 2.5 degrees
        static constexpr float kTrackingDuration = 0.30f;

        static float TrackingDuration(float configured, bool mounted)
        {
            return mounted ? (std::max)(configured, kTrackingDuration) : configured;
        }

        static float RelativeYaw(float world, float reference)
        {
            return std::remainder(world - reference, 6.28318530718f);
        }

        static std::optional<float> Heading(Quaternion q)
        {
            if (!Normalize(q)) return std::nullopt;
            const float fx = 2.0f * (q.x * q.y - q.w * q.z);
            const float fy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            if (std::hypot(fx, fy) < 0.01f) return std::nullopt;
            return std::atan2(fx, fy);
        }

        // Skyrim heading increases clockwise from +Y. Correct the fresh
        // quaternion after the chained mounted UpdateRotation, where TDM has
        // already compensated freeRotation.x for the horse's heading change.
        // This prevents rider/horse heading differences entering the camera.
        static Quaternion AimYaw(Quaternion q, float desired)
        {
            const auto heading = Heading(q);
            if (!heading || !std::isfinite(desired)) return q;
            const float delta = -RelativeYaw(desired, *heading);
            const float w = std::cos(delta * 0.5f), z = std::sin(delta * 0.5f);
            return {w*q.w - z*q.z, w*q.x - z*q.y,
                    w*q.y + z*q.x, w*q.z + z*q.w};
        }

        void Reset() { *this = {}; }

        // Sample the fresh UpdateRotation result once per state update. Queries
        // to GetRotation must not advance or apply this filter. dt=0 freezes a
        // paused view. tracking=false retires
        // the last correction smoothly on switch, unlock, or dismount.
        float Sample(std::uint64_t frame, float raw, bool tracking, float dt)
        {
            if (!std::isfinite(raw) || !std::isfinite(dt) || dt < 0.0f) {
                Reset();
                return 0.0f;
            }
            if (_sampled && frame == _frame) return _correction;
            _frame = frame;
            _sampled = true;
            if (dt == 0.0f) return _correction;
            if (dt > 0.25f) {  // load/stall: do not chase a stale view
                Reset();
                _frame = frame;
                _sampled = true;
            }
            dt = (std::min)(dt, 0.10f);
            if (tracking) {
                if (!_tracking) {
                    _pitch = raw + _correction;
                    _pitchVelocity = 0.0f;
                    _tracking = true;
                    return _correction;
                }
                CriticalDampedSpringExact(_pitch, _pitchVelocity, raw, 22.0f, dt);
                const float error = _pitch - raw;
                _correction = std::clamp(error, -kMaxCorrection, kMaxCorrection);
                if (_correction != error) {
                    // Large intentional changes stay responsive and cannot
                    // leave a long spring tail after a fast turn or hill crest.
                    _pitch = raw + _correction;
                    _pitchVelocity = 0.0f;
                }
                _releaseVelocity = 0.0f;
            } else {
                _tracking = false;
                CriticalDampedSpringExact(_correction, _releaseVelocity, 0.0f, 24.0f, dt);
                if (std::abs(_correction) < 0.00001f && std::abs(_releaseVelocity) < 0.0001f) {
                    _correction = _releaseVelocity = 0.0f;
                }
            }
            return _correction;
        }

        static std::optional<float> Elevation(Quaternion q)
        {
            if (!Normalize(q)) return std::nullopt;
            const float fx = 2.0f * (q.x * q.y - q.w * q.z);
            const float fy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            const float fz = 2.0f * (q.y * q.z + q.w * q.x);
            const float horizontal = std::hypot(fx, fy);
            if (horizontal < 0.01f) return std::nullopt;
            return std::atan2(fz, horizontal);
        }

        // Rotate about the WORLD horizontal right axis. Local-X would couple
        // pitch into yaw when the original camera carries roll. Preserve the
        // engine's yaw and roll, including whichever horse convention it uses.
        static Quaternion Apply(Quaternion q, float correction)
        {
            const auto original = q;
            if (!std::isfinite(correction) || !Normalize(q)) return original;
            const float fx = 2.0f * (q.x * q.y - q.w * q.z);
            const float fy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            const float horizontal = std::hypot(fx, fy);
            if (horizontal < 0.01f) return original;
            // Keep the forward vector away from the pitch singularity.
            const float fz = 2.0f * (q.y * q.z + q.w * q.x);
            const float elevation = std::atan2(fz, horizontal);
            const float delta = std::clamp(elevation + correction, -1.55f, 1.55f) - elevation;
            const float w = std::cos(delta * 0.5f);
            const float s = std::sin(delta * 0.5f) / horizontal;
            const float x = fy * s, y = -fx * s;
            return {w*q.w - x*q.x - y*q.y,
                    w*q.x + x*q.w + y*q.z,
                    w*q.y - x*q.z + y*q.w,
                    w*q.z + x*q.y - y*q.x};
        }

    private:
        static bool Normalize(Quaternion& q)
        {
            const float norm = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
            if (!std::isfinite(norm) || norm < 0.000001f) return false;
            const float scale = 1.0f / std::sqrt(norm);
            q.w *= scale; q.x *= scale; q.y *= scale; q.z *= scale;
            return true;
        }

        std::uint64_t _frame{};
        bool _sampled{}, _tracking{};
        float _pitch{}, _pitchVelocity{}, _correction{}, _releaseVelocity{};
    };
}
