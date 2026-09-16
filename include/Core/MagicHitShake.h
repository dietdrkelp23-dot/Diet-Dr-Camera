#pragma once

#include "Core/ArcheryHitShake.h"

namespace DietDrCamera::MagicHitShake
{
    using ArcheryHitShake::Fresh;
    using ArcheryHitShake::ProjectileIdentity;

    struct Profile
    {
        HitShake::Tuning tuning{};
        std::uint32_t magic = 0, staff = 0;
        int hand = -1; // settings order: left, both, right
        std::array<char, 192> entry{};
        bool ritual = false;
    };

    struct Impact
    {
        Profile profile{};
        HitShakeMotion::Vector velocity{};
        ProjectileIdentity projectile{};
        std::uint32_t target = 0;
        double time = 0, launchedAt = 0;
        float distance = 0;
    };

    // Actual engine casting type and projectile class decide eligibility. A
    // master-tier staff may route to Ritual while still firing a excluded beam.
    inline bool Eligible(bool fireAndForget, bool missile, bool continuous, bool handCast)
    {
        return fireAndForget && missile && !continuous && handCast;
    }

    class ShotBridge
    {
    public:
        void SetView(int next)
        {
            std::scoped_lock guard(lock);
            if (view == next) return;
            view = next;
            prepared = {}; shots = {}; casts = {}; epochs = {}; epochTimes = {};
            preparedAt = 0; count = 0;
        }

        void Publish(const std::array<Profile, 4>& profiles, double now)
        {
            std::scoped_lock guard(lock);
            prepared = profiles; // left/right, then their Both Hands variants
            preparedAt = now;
        }

        void BeginCast(int source, double now)
        {
            if (source < 0 || source > 1 || !std::isfinite(now)) return;
            std::scoped_lock guard(lock);
            if (view < 0) return;
            ++epochs[source];
            epochTimes[source] = now;
        }

        bool Launch(ProjectileIdentity id, std::uint32_t magic, std::uint32_t staff,
                    int source, double now, float livingTime = 0, bool dual = false)
        {
            std::scoped_lock guard(lock);
            if (view < 0 || !id || !magic || source < 0 || source > 2 ||
                !Fresh(now, preparedAt, 0.25) || !std::isfinite(livingTime) || livingTime < 0) return false;
            auto matches = [&](const Profile& p) {
                return p.magic == magic && (!staff || p.staff == staff) && p.hand >= 0 && p.hand < 3;
            };
            const int offset = dual ? 2 : 0;
            int matchedSource = source;
            const Profile* profile = source < 2 && matches(prepared[source + offset]) ? &prepared[source + offset] : nullptr;
            if (!profile) for (int i = 0; i < 2; ++i) {
                const auto& p = prepared[i + ((dual || (source == 2 && prepared[i].ritual)) ? 2 : 0)];
                if (!matches(p)) continue;
                // Unknown/other source is safe only when the matching hand is
                // unambiguous (or both snapshots describe the same dual cast).
                if (profile && ((!dual && !(source == 2 && p.ritual && profile->ritual)) || profile->staff != p.staff)) return false;
                profile = &p;
                matchedSource = i;
            }
            if (!profile) return false;
            const bool sharedCast = dual || (source == 2 && profile->ritual);
            Shot* free = nullptr;
            for (auto& shot : shots) {
                if (shot.id && Fresh(now, shot.time, 30.0)) {
                    if (shot.id == id) {
                        if (livingTime >= shot.livingTime) return false;
                        free = &shot; break;
                    }
                } else if (!free) free = &shot;
            }
            if (!free) return false;
            const int lane = matchedSource;
            const int groupHand = sharedCast ? 1 : lane == 0 ? 0 : 2;
            const auto epoch = sharedCast ? epochs[0] + epochs[1] : epochs[lane];
            const double epochTime = sharedCast ? (std::max)(epochTimes[0], epochTimes[1]) : epochTimes[lane];
            const bool signalled = epoch && Fresh(now, epochTime, 3.0);
            Cast* group = nullptr;
            Cast* available = nullptr;
            for (auto& cast : casts) {
                if (!cast.id || !Fresh(now, cast.time, 30.0)) { if (!available) available = &cast; continue; }
                if (cast.magic != magic || cast.staff != profile->staff || cast.hand != groupHand) continue;
                // Native cast-start signals distinguish rapid casts. A small
                // launch window handles animations which omit those signals,
                // and the two start callbacks belonging to one dual cast.
                if ((signalled && cast.signalled && cast.epoch == epoch) ||
                    ((!signalled && !cast.signalled) && Fresh(now, cast.time, 0.05)) ||
                    (sharedCast && Fresh(now, cast.time, 0.02))) { group = &cast; break; }
            }
            if (!group) {
                if (!available) return false;
                group = available;
                *group = {++nextCast, magic, profile->staff, groupHand, epoch, now, signalled, false};
            }
            *free = {id, *profile, group->id, now, livingTime};
            return true;
        }

        void Observe(ProjectileIdentity id, float livingTime)
        {
            if (!std::isfinite(livingTime) || livingTime < 0) return;
            std::scoped_lock guard(lock);
            for (auto& shot : shots) if (shot.id == id) shot.livingTime = (std::max)(shot.livingTime, livingTime);
        }

        bool Contact(ProjectileIdentity id, std::uint32_t magic, std::uint32_t target,
                     HitShakeMotion::Vector velocity, double now, float distance = 0)
        {
            std::scoped_lock guard(lock);
            if (view < 0 || !id || !target || !magic) return false;
            for (const auto& shot : shots) {
                if (shot.id != id || shot.profile.magic != magic || !Fresh(now, shot.time, 30.0)) continue;
                for (auto& cast : casts) if (cast.id == shot.cast) {
                    if (cast.contacted) return false;
                    cast.contacted = true; // consume even a zero or a full queue
                    if (count == impacts.size()) return false;
                    impacts[count++] = {shot.profile, velocity, id, target, now, shot.time, distance};
                    return true;
                }
            }
            return false;
        }

        std::size_t Drain(double now, std::array<Impact, 32>& out)
        {
            std::scoped_lock guard(lock);
            std::size_t n = 0, pending = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (Fresh(now, impacts[i].time, 0.25)) out[n++] = impacts[i];
                else if (Fresh(impacts[i].time, now, 0.25)) impacts[pending++] = impacts[i];
            }
            count = pending;
            return n;
        }

    private:
        struct Shot { ProjectileIdentity id{}; Profile profile{}; std::uint64_t cast = 0; double time = 0; float livingTime = 0; };
        struct Cast {
            std::uint64_t id = 0;
            std::uint32_t magic = 0, staff = 0;
            int hand = -1;
            std::uint64_t epoch = 0;
            double time = 0;
            bool signalled = false, contacted = false;
        };
        std::mutex lock;
        int view = -1;
        std::array<Profile, 4> prepared{};
        double preparedAt = 0;
        std::array<std::uint64_t, 2> epochs{};
        std::array<double, 2> epochTimes{};
        std::uint64_t nextCast = 0;
        std::array<Shot, 128> shots{};
        std::array<Cast, 64> casts{};
        std::array<Impact, 32> impacts{};
        std::size_t count = 0;
    };
}
