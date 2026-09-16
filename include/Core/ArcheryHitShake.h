#pragma once

#include "Core/HitShake.h"
#include "Core/HitShakeMotion.h"

namespace DietDrCamera::ArcheryHitShake
{
    // The camera thread publishes resolved values. Projectile callbacks never
    // inspect presets, inventory extras or scene objects, or retain game pointers.
    struct Profile
    {
        HitShake::Tuning tuning{};
        std::uint32_t weapon = 0;
        int key = -1;
        std::array<char, 192> entry{};
    };

    struct ProjectileIdentity
    {
        std::uint32_t form = 0;
        std::uint32_t reference = 0;
        bool operator==(const ProjectileIdentity&) const = default;
        explicit operator bool() const { return form && reference; }
    };

    struct Impact
    {
        Profile profile{};
        HitShakeMotion::Vector velocity{};
        ProjectileIdentity projectile{};
        std::uint32_t target = 0;
        double time = 0;
        double launchedAt = 0;
        float distance = 0;
    };

    inline bool Fresh(double now, double then, double limit)
    {
        const double age = now - then;
        return std::isfinite(age) && age >= 0 && age <= limit;
    }

    inline bool TargetEligible(bool actor, bool player) { return actor && !player; }

    inline float ImpactDistance(HitShakeMotion::Vector player, HitShakeMotion::Vector target)
    {
        if (!player.Finite() || !target.Finite()) return 0; // unknown geometry retains the ordinary response
        return std::hypot(target.x - player.x, target.y - player.y, target.z - player.z);
    }

    inline float DistanceScale(float distance)
    {
        if (std::isnan(distance) || distance <= 0) return 1;
        // Smooth at the origin and across the whole range: 90% at 1000
        // game units, 75% at 2000, 60% at 4000, tending to 50% far away.
        const double range = static_cast<double>(distance) / 2000.0;
        return static_cast<float>(0.5 + 0.5 / (1.0 + range * range));
    }

    class ShotBridge
    {
    public:
        void SetView(int next)
        {
            std::scoped_lock guard(lock);
            if (view == next) return;
            view = next;
            prepared = {};
            preparedAt = 0;
            shots = {};
            count = 0;
        }

        void Publish(Profile profile, double now)
        {
            std::scoped_lock guard(lock);
            prepared = profile;
            preparedAt = now;
        }

        bool Launch(ProjectileIdentity projectile, std::uint32_t weapon, double now, float livingTime = 0)
        {
            std::scoped_lock guard(lock);
            if (view < 0 || !projectile || !weapon || prepared.weapon != weapon ||
                prepared.key < 0 || prepared.key >= static_cast<int>(HitShake::kArcheryKeys.size()) ||
                !Fresh(now, preparedAt, 0.25) || !std::isfinite(livingTime) || livingTime < 0) return false;
            Shot* free = nullptr;
            for (auto& shot : shots) {
                if (shot.projectile && Fresh(now, shot.launchedAt, 30.0)) {
                    if (shot.projectile == projectile) {
                        // Form IDs are recycled. The reference handle includes
                        // its generation; a lifetime reset also handles an
                        // engine/mod pool reusing the same live reference.
                        if (livingTime >= shot.lastLivingTime) return false;
                        free = &shot;
                        break;
                    }
                } else if (!free) free = &shot;
            }
            if (!free) return false;
            *free = {prepared, projectile, now, livingTime, false};
            return true;
        }

        void Observe(ProjectileIdentity projectile, float livingTime)
        {
            if (!std::isfinite(livingTime) || livingTime < 0) return;
            std::scoped_lock guard(lock);
            for (auto& shot : shots) if (shot.projectile == projectile)
                shot.lastLivingTime = (std::max)(shot.lastLivingTime, livingTime);
        }

        bool Contact(ProjectileIdentity projectile, std::uint32_t weapon, std::uint32_t target,
                     HitShakeMotion::Vector velocity, double now, float distance = 0)
        {
            std::scoped_lock guard(lock);
            if (view < 0 || !projectile || !weapon || !target || !std::isfinite(now)) return false;
            for (auto& shot : shots) {
                if (shot.projectile != projectile || shot.profile.weapon != weapon || shot.contacted ||
                    !Fresh(now, shot.launchedAt, 30.0)) continue;
                // Retain the consumed identity across drains. Repeated collision
                // callbacks, ricochets and enchantment hits cannot rearm it.
                shot.contacted = true;
                if (count == impacts.size()) return false;
                impacts[count++] = {shot.profile, velocity, projectile, target, now, shot.launchedAt, distance};
                return true;
            }
            return false;
        }

        std::size_t Drain(double now, std::array<Impact, 32>& out)
        {
            std::scoped_lock guard(lock);
            std::size_t n = 0;
            std::size_t pending = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (Fresh(now, impacts[i].time, 0.25)) out[n++] = impacts[i];
                // A callback can arrive after the consumer sampled its clock
                // but before it acquired this lock. Keep it for the next frame.
                else if (impacts[i].time > now && Fresh(impacts[i].time, now, 0.25)) impacts[pending++] = impacts[i];
            }
            count = pending;
            return n;
        }

    private:
        struct Shot
        {
            Profile profile{};
            ProjectileIdentity projectile{};
            double launchedAt = 0;
            float lastLivingTime = 0;
            bool contacted = false;
        };
        std::mutex lock;
        int view = -1;
        Profile prepared{};
        double preparedAt = 0;
        std::array<Shot, 64> shots{};
        std::array<Impact, 32> impacts{};
        std::size_t count = 0;
    };

    inline HitShake::Tuning ImpactTuning(HitShake::Tuning tuning, bool firstPerson = false, float distance = 0)
    {
        tuning = HitShake::Sanitize(tuning);
        // First person needs a stronger response range. Apply before the
        // shared mixer limits, retaining its roll restraint and exact zero.
        tuning.strength *= (firstPerson ? 1.0f : 0.5f) * DistanceScale(distance);
        return tuning;
    }

    inline HitShake::Rotation ImpactAxes(HitShakeMotion::Vector travel, HitShakeMotion::Basis camera, bool firstPerson)
    {
        // Arrows approach almost along the view axis. Normalizing their tiny
        // screen projection as a melee slash amplifies small aim/gravity changes
        // into full sideways kicks and pitch reversals. Keep a dominant recoil
        // with a bounded continuous cue from the actual direction of travel.
        if (!travel.Finite()) return {1, 0, 0};
        const float scale = (std::max)({std::abs(travel.x), std::abs(travel.y), std::abs(travel.z)});
        if (scale < 1e-6f) return {1, 0, 0};
        const auto scaled = travel * (1 / scale);
        const auto local = camera.ToLocal(scaled * (1 / scaled.Length()));
        if (!local.Finite()) return {1, 0, 0};
        HitShake::Rotation axes{1 - 0.25f * local.z, 0.25f * local.x, (firstPerson ? 0.01f : 0.025f) * local.x};
        const float length = std::sqrt(axes.pitch * axes.pitch + axes.yaw * axes.yaw + axes.roll * axes.roll);
        if (!std::isfinite(length) || length < 1e-6f) return {1, 0, 0};
        return {axes.pitch / length, axes.yaw / length, axes.roll / length};
    }
}
