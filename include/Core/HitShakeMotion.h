#pragma once

#include "Core/HitShake.h"

namespace DietDrCamera::HitShakeMotion
{
    struct Vector
    {
        float x = 0, y = 0, z = 0;
        Vector operator+(Vector v) const { return {x + v.x, y + v.y, z + v.z}; }
        Vector operator-(Vector v) const { return {x - v.x, y - v.y, z - v.z}; }
        Vector operator*(float s) const { return {x * s, y * s, z * s}; }
        float Dot(Vector v) const { return x * v.x + y * v.y + z * v.z; }
        float Length() const { return std::sqrt(Dot(*this)); }
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    };

    struct Basis
    {
        Vector right{1, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
        Vector ToLocal(Vector v) const { return {v.Dot(right), v.Dot(forward), v.Dot(up)}; }
        Vector ToWorld(Vector v) const { return right * v.x + forward * v.y + up * v.z; }
    };

    struct Pose
    {
        double time = 0;
        Vector grip{}, tip{};  // world axes, relative to the skeleton root's origin and scale
    };

    inline Vector RelativeToRoot(Vector world, Vector origin, float scale)
    {
        // Remove locomotion while retaining body rotation: spinning attacks
        // can carry the weapon around without much motion relative to the hand.
        return (world - origin) * (1 / scale);
    }

    struct Estimate
    {
        Vector velocity{};
        float age = 0;
        bool valid = false;
    };

    // Per striking hand. Only value snapshots are retained; never skeleton,
    // equipped-item or settings pointers. A short quadratic tangent captures
    // curved and reversing swings without averaging a whole attack together.
    class History
    {
    public:
        void Clear() { count = 0; }

        bool Bind(std::uintptr_t rig, std::uintptr_t node, std::uint32_t weapon, float scale)
        {
            const std::array<std::uintptr_t, 3> next{rig, node, weapon};
            if (!rig || !node || !std::isfinite(scale) || scale <= 0) { Clear(); identity = {}; return true; }
            const bool changed = next != identity || std::abs(scale - lastScale) > 0.001f;
            if (changed) Clear();
            identity = next;
            lastScale = scale;
            return changed;
        }

        void Add(Pose pose)
        {
            if (!std::isfinite(pose.time) || !pose.grip.Finite() || !pose.tip.Finite() ||
                pose.grip.Length() > 10000 || pose.tip.Length() > 10000) { Clear(); return; }
            if (count) {
                const auto& last = poses[count - 1];
                const double dt = pose.time - last.time;
                if (dt >= 0 && dt < 0.001) return; // repeated camera calls in one frame
                const float reach = (std::max)((pose.tip - pose.grip).Length(), (last.tip - last.grip).Length());
                if (dt <= 0 || dt > 0.125 || (pose.grip - last.grip).Length() > 128 ||
                    (pose.tip - last.tip).Length() > (std::max)(128.0f, reach * 2.5f)) Clear();
            }
            if (count == poses.size()) {
                for (std::size_t i = 1; i < count; ++i) poses[i - 1] = poses[i];
                --count;
            }
            poses[count++] = pose;
        }

        Estimate At(double hitTime, Vector target, bool targetKnown) const
        {
            if (count < 2 || !std::isfinite(hitTime)) return {};
            // Include at most the camera frame bracketing the event. Never use
            // later recovery motion to decide an earlier contact's direction.
            std::size_t end = count;
            for (std::size_t i = 0; i < count; ++i) if (poses[i].time >= hitTime) { end = i + 1; break; }
            if (end < 2 || poses[end - 1].time - hitTime > 0.04 || hitTime - poses[end - 1].time > 0.06) return {};
            const auto& latest = poses[end - 1];
            const auto blade = latest.tip - latest.grip;
            const float length2 = blade.Dot(blade);
            // Estimate the striking portion of the rendered weapon from the
            // target's location. Fists/claws have coincident endpoints.
            const float along = targetKnown && target.Finite() && length2 > 1
                ? std::clamp((target - latest.grip).Dot(blade) / length2, 0.2f, 1.0f) : 0.8f;
            auto point = [&](std::size_t i) { return poses[i].grip + (poses[i].tip - poses[i].grip) * along; };

            // Hitstop can leave an identical pose at the end. Use the most
            // recent moving interval ending near the contact, not a zero
            // velocity or a long-ago windup. Keep native hit time unchanged.
            while (end > 1 && (point(end - 1) - point(end - 2)).Length() < 0.01f) --end;
            if (end < 2 || hitTime - poses[end - 1].time > 0.06) return {};
            const std::size_t c = end - 1;
            std::size_t b = c - 1;
            while (b > 0 && poses[c].time - poses[b].time < 0.008) --b;
            const double dt = poses[c].time - poses[b].time;
            if (dt < 0.001 || dt > 0.075) return {};
            Vector velocity = (point(c) - point(b)) * static_cast<float>(1.0 / dt);

            // Three well-spaced samples avoid magnifying sub-millisecond
            // timestamp noise. Limit extrapolation when a hitstop is detected.
            if (b > 0) {
                std::size_t a = b - 1;
                while (a > 0 && poses[b].time - poses[a].time < 0.008) --a;
                const double ta = poses[a].time - poses[b].time;
                const double tc = poses[c].time - poses[b].time;
                const double t = std::clamp(hitTime, poses[b].time, poses[c].time) - poses[b].time;
                if (ta < -0.001 && tc - ta <= 0.1) {
                    // Differentiate the interpolating quadratic about the
                    // middle pose so large world/time origins cannot cancel.
                    const Vector before = point(a) - point(b), after = point(c) - point(b);
                    const Vector tangent = before * static_cast<float>((2 * t - tc) / (ta * (ta - tc))) +
                                           after * static_cast<float>((2 * t - ta) / (tc * (tc - ta)));
                    const float speed = velocity.Length(), fittedSpeed = tangent.Length();
                    if (tangent.Finite() && fittedSpeed <= (std::max)(speed * 3, 10.0f)) velocity = tangent;
                }
            }
            const float speed = velocity.Length();
            if (!velocity.Finite() || !std::isfinite(speed) || speed < 1 || speed > 100000) return {};
            return {velocity, static_cast<float>((std::max)(0.0, hitTime - poses[c].time)), true};
        }

    private:
        std::array<Pose, 64> poses{};
        std::size_t count = 0;
        std::array<std::uintptr_t, 3> identity{};
        float lastScale = 0;
    };

    inline HitShake::Rotation Axes(Vector worldVelocity, Basis camera, HitShake::Rotation fallback, bool firstPerson)
    {
        if (!worldVelocity.Finite()) return fallback;
        const float scale = (std::max)({std::abs(worldVelocity.x), std::abs(worldVelocity.y), std::abs(worldVelocity.z)});
        if (!std::isfinite(scale) || scale < 1e-6f) return fallback;
        const Vector scaled = worldVelocity * (1 / scale);
        const auto v = camera.ToLocal(scaled * (1 / scaled.Length()));
        if (!v.Finite()) return fallback;
        // Continuous screen-space recoil for slashes of any angle. Only a
        // nearly head-on thrust needs a depth cue: its screen direction is
        // inherently tiny, so give it a stable upward kick.
        const float screen = std::clamp(std::sqrt(v.x * v.x + v.z * v.z), 0.0f, 1.0f);
        const float depthCue = 0.2f * std::abs(v.y) * std::pow(1 - screen, 4.0f);
        HitShake::Rotation axes{-v.z + depthCue, v.x, v.x * (firstPerson ? 0.025f : 0.10f)};
        const float length = std::sqrt(axes.pitch * axes.pitch + axes.yaw * axes.yaw + axes.roll * axes.roll);
        if (!std::isfinite(length) || length < 1e-4f) return fallback;
        axes.pitch /= length; axes.yaw /= length; axes.roll /= length;
        return axes;
    }
}
