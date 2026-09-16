#include "PCH.h"
#include "Camera/ArcheryHitShakeController.h"
#include "Camera/StateResolver.h"
#include "Core/ArcheryHitShake.h"
#include "Settings/EquippedItemBinding.h"
#include "Settings/SettingsManager.h"
#include <chrono>

namespace DietDrCamera::ArcheryHitShakeController
{
    namespace
    {
        ArcheryHitShake::ShotBridge bridge;
        namespace Motion = HitShakeMotion;
        using UpdateFn = void(*)(RE::Projectile*, float);
        using ImpactFn = void(*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3&, const RE::NiPoint3&,
                                RE::hkpCollidable*, std::int32_t, std::uint32_t);
        UpdateFn originalUpdate = nullptr;
        ImpactFn originalImpact = nullptr;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool PlayerArrow(RE::Projectile* projectile)
        {
            if (!projectile) return false;
            const auto& data = projectile->GetProjectileRuntimeData();
            const auto shooter = data.shooter.get();
            return shooter && shooter->IsPlayerRef() && data.ammoSource && data.weaponSource &&
                (data.weaponSource->IsBow() || data.weaponSource->IsCrossbow());
        }

        ArcheryHitShake::ProjectileIdentity ObserveProjectile(RE::Projectile* projectile, double now)
        {
            const auto& data = projectile->GetProjectileRuntimeData();
            const ArcheryHitShake::ProjectileIdentity id{projectile->GetFormID(), projectile->GetHandle().native_handle()};
            if (data.flags.none(RE::Projectile::Flags::kMoved))
                bridge.Launch(id, data.weaponSource->GetFormID(), now, data.livingTime);
            bridge.Observe(id, data.livingTime);
            return id;
        }

        void UpdateProjectile(RE::Projectile* projectile, float dt)
        {
            if (PlayerArrow(projectile)) ObserveProjectile(projectile, Now());
            originalUpdate(projectile, dt);
        }

        void AddImpact(RE::Projectile* projectile, RE::TESObjectREFR* target, const RE::NiPoint3& position,
                       const RE::NiPoint3& velocity, RE::hkpCollidable* collidable, std::int32_t arg6, std::uint32_t arg7)
        {
            // Copy before calling the next handler: it can change or destroy
            // the projectile. No collision outcome or trajectory is modified.
            const bool eligible = PlayerArrow(projectile) && target &&
                ArcheryHitShake::TargetEligible(target->IsActor(), target->IsPlayerRef());
            ArcheryHitShake::ProjectileIdentity id{};
            std::uint32_t weapon = 0, targetID = 0;
            Motion::Vector travel{};
            float distance = 0;
            const double now = Now();
            if (eligible) {
                id = ObserveProjectile(projectile, now); // also covers a contact on the first update
                const auto& data = projectile->GetProjectileRuntimeData();
                weapon = data.weaponSource->GetFormID();
                targetID = target->GetFormID();
                travel = {data.velocity.x, data.velocity.y, data.velocity.z};
                if (const auto shooter = data.shooter.get()) {
                    const auto from = shooter->GetPosition(), to = target->GetPosition();
                    distance = ArcheryHitShake::ImpactDistance({from.x, from.y, from.z}, {to.x, to.y, to.z});
                }
            }
            originalImpact(projectile, target, position, velocity, collidable, arg6, arg7);
            if (eligible) bridge.Contact(id, weapon, targetID, travel, now, distance);
        }

        SettingsManager::NoiseProfile* CurrentProfile(RE::PlayerCharacter* player, bool firstPerson,
                                                      ArcheryHitShake::Profile* snapshot = nullptr)
        {
            if (!player) return nullptr;
            bool left = false;
            auto* source = player->GetEquippedObject(false);
            auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon || (!weapon->IsBow() && !weapon->IsCrossbow())) {
                left = true;
                source = player->GetEquippedObject(true);
                weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
            }
            if (!weapon || (!weapon->IsBow() && !weapon->IsCrossbow())) return nullptr;
            auto& settings = SettingsManager::GetSingleton();
            const auto& sr = StateResolver::GetSingleton();
            const bool mounted = sr.GetState() == CameraState::Horseback;
            const auto key = HitShake::ArcheryKey(weapon->IsCrossbow(), sr.IsSneaking(), sr.IsBowZoomed());
            std::string resolved;
            auto* profile = settings.ResolveHitShakeProfile(key, firstPerson, MeleeWeaponType::Unarmed,
                ItemBindings::DescribeEquipped(player, left), {}, &resolved, mounted);
            if (snapshot) {
                snapshot->weapon = weapon->GetFormID();
                snapshot->key = mounted ? (sr.IsBowZoomed() ? 9 : 8) : HitShake::ArcheryIndex(key);
                if (profile) snapshot->tuning = profile->hitShake;
                std::snprintf(snapshot->entry.data(), snapshot->entry.size(), "%s", resolved.c_str());
            }
            return profile;
        }
    }

    void Install()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        // SE/AE vtable slots from CommonLibSSE-NG. Chain the current handlers,
        // including DDC's existing aim correction, without taking aim ownership.
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_ArrowProjectile[0]};
        originalUpdate = reinterpret_cast<UpdateFn>(table.write_vfunc(0xAB, &UpdateProjectile));
        originalImpact = reinterpret_cast<ImpactFn>(table.write_vfunc(0xBD, &AddImpact));
        spdlog::debug("[HITSHAKE-ARCHERY] native arrow/bolt launch and contact observers installed");
    }

    void Reset() { bridge.SetView(-1); }

    HitShake::Tuning* CurrentTuning(bool firstPerson)
    {
        auto* p = CurrentProfile(RE::PlayerCharacter::GetSingleton(), firstPerson);
        return p ? &p->hitShake : nullptr;
    }

    void Update(RE::PlayerCharacter* player, RE::PlayerCamera* camera, bool firstPerson,
                double now, HitShake::Mixer& mixer)
    {
        bridge.SetView(firstPerson ? 1 : 0);
        ArcheryHitShake::Profile prepared;
        CurrentProfile(player, firstPerson, &prepared);
        bridge.Publish(prepared, now);
        const auto& m = camera->cameraRoot->world.rotate;
        const Motion::Basis basis{{m.entry[0][0], m.entry[1][0], m.entry[2][0]},
                                  {m.entry[0][1], m.entry[1][1], m.entry[2][1]},
                                  {m.entry[0][2], m.entry[1][2], m.entry[2][2]}};
        std::array<ArcheryHitShake::Impact, 32> impacts;
        const double contactNow = Now();
        const auto count = bridge.Drain(contactNow, impacts);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& hit = impacts[i];
            const auto axes = ArcheryHitShake::ImpactAxes(hit.velocity, basis, firstPerson);
            const auto tuning = ArcheryHitShake::ImpactTuning(hit.profile.tuning, firstPerson, hit.distance);
            static std::array<std::array<int, 2>, 2> diagnostics{};
            auto& lines = diagnostics[firstPerson ? 1 : 0][hit.profile.tuning.strength > 0 ? 1 : 0];
            if (lines < 32) {
                ++lines;
                spdlog::debug(
                "[HITSHAKE-ARCHERY] view={} projectile={:08X} reference={:08X} target={:08X} weapon={:08X} requested={} entry={} strength={:.2f} distance={:.1f} falloff={:.3f} effective={:.2f} flight_ms={:.1f} queue_ms={:.2f} axes=({:.2f},{:.2f},{:.2f})",
                firstPerson ? "1p" : "3p", hit.projectile.form, hit.projectile.reference, hit.target, hit.profile.weapon,
                HitShake::kArcheryKeys[hit.profile.key], hit.profile.entry.data(), hit.profile.tuning.strength,
                hit.distance, ArcheryHitShake::DistanceScale(hit.distance), tuning.strength,
                (hit.time - hit.launchedAt) * 1000, (contactNow - hit.time) * 1000,
                axes.pitch, axes.yaw, axes.roll);
            }
            mixer.Arm(tuning, axes,
                      hit.projectile.reference ^ (hit.target * 1664525u), firstPerson, static_cast<float>(contactNow - hit.time),
                      HitShake::Onset::Contact);
        }
    }
}
