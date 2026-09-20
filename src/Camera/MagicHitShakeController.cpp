#include "PCH.h"
#include "Camera/MagicHitShakeController.h"
#include "Camera/DamageReactionController.h"
#include "Camera/CameraNoiseController.h"
#include "Hooks/MissileProjectileDetour.h"
#include "Camera/MagicCast.h"
#include "Camera/StateResolver.h"
#include "Core/MagicHitShake.h"
#include "Settings/EquippedItemBinding.h"
#include "Settings/SettingsManager.h"
#include <chrono>

namespace DietDrCamera::MagicHitShakeController
{
    namespace
    {
        MagicHitShake::ShotBridge bridge;
        using Source = RE::MagicSystem::CastingSource;
        using UpdateFn = void(*)(RE::Projectile*, float);
        using ImpactFn = void(*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3&, const RE::NiPoint3&,
                                RE::hkpCollidable*, std::int32_t, std::uint32_t);
        UpdateFn originalUpdate = nullptr;
        ImpactFn originalImpact = nullptr;
        UpdateFn originalConeUpdate = nullptr;
        ImpactFn originalConeImpact = nullptr;

        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        bool DiscreteMissile(const RE::BGSProjectile* base)
        {
            return base && base->IsMissile() &&
                base->data.flags.none(RE::BGSProjectileData::BGSProjectileFlags::kContinuousUpdate);
        }

        bool EligibleMagic(RE::MagicItem* magic)
        {
            if (!magic || magic->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget) return false;
            const auto type = magic->GetSpellType();
            if (type != RE::MagicSystem::SpellType::kSpell && type != RE::MagicSystem::SpellType::kStaffEnchantment &&
                type != RE::MagicSystem::SpellType::kEnchantment) return false;
            for (const auto* effect : magic->effects)
                if (effect && effect->baseEffect && DiscreteMissile(effect->baseEffect->data.projectileBase)) return true;
            return false;
        }

        bool PlayerMissile(RE::Projectile* projectile)
        {
            if (!projectile) return false;
            const auto& data = projectile->GetProjectileRuntimeData();
            const auto shooter = data.shooter.get();
            auto* base = projectile->GetBaseObject();
            return shooter && shooter->IsPlayerRef() && !data.ammoSource && EligibleMagic(data.spell) &&
                MagicHitShake::Eligible(true, base && DiscreteMissile(base->As<RE::BGSProjectile>()), false,
                    data.castingSource == Source::kLeftHand || data.castingSource == Source::kRightHand || data.castingSource == Source::kOther);
        }

        MagicHitShake::ProjectileIdentity Observe(RE::Projectile* projectile, double now)
        {
            const auto& data = projectile->GetProjectileRuntimeData();
            const MagicHitShake::ProjectileIdentity id{projectile->GetFormID(), projectile->GetHandle().native_handle()};
            if (data.flags.none(RE::Projectile::Flags::kMoved))
                bridge.Launch(id, data.spell->GetFormID(), data.weaponSource ? data.weaponSource->GetFormID() : 0,
                              static_cast<int>(data.castingSource), now, data.livingTime, data.flags.any(RE::Projectile::Flags::kIsDual));
            bridge.Observe(id, data.livingTime);
            return id;
        }

        void UpdateProjectile(RE::Projectile* projectile, float dt)
        {
            if (PlayerMissile(projectile)) Observe(projectile, Now());
            if (projectile) {
                CameraNoiseController::NotifyNpcMagicFlight(projectile, projectile->GetPosition());
                const auto& data = projectile->GetProjectileRuntimeData();
                const auto* base = projectile->GetBaseObject();
                if (data.spell && base && data.flags.none(RE::Projectile::Flags::kMoved) && DiscreteMissile(base->As<RE::BGSProjectile>())) {
                    const auto shooter = data.shooter.get();
                    if (shooter && !shooter->IsPlayerRef()) CameraNoiseController::NotifyNpcMagicShot(
                        data.shooter, data.spell->GetFormID(), projectile->GetFormID());
                }
            }
            originalUpdate(projectile, dt);
        }

        void AddImpact(RE::Projectile* projectile, RE::TESObjectREFR* target, const RE::NiPoint3& position,
                       const RE::NiPoint3& velocity, RE::hkpCollidable* collidable, std::int32_t arg6, std::uint32_t arg7)
        {
            CameraNoiseController::NotifyNpcMagicFlight(projectile, position, true, target && target->IsPlayerRef());
            DamageReactionController::ObserveProjectile(projectile, target);
            MissileProjectileDetour::ObserveImpact(projectile, position);
            const bool eligible = PlayerMissile(projectile) && target &&
                ArcheryHitShake::TargetEligible(target->IsActor(), target->IsPlayerRef());
            MagicHitShake::ProjectileIdentity id{};
            std::uint32_t magic = 0, targetID = 0;
            HitShakeMotion::Vector travel{};
            float distance = 0;
            const double now = Now();
            if (eligible) {
                id = Observe(projectile, now);
                const auto& data = projectile->GetProjectileRuntimeData();
                magic = data.spell->GetFormID(); targetID = target->GetFormID();
                travel = {data.velocity.x, data.velocity.y, data.velocity.z};
                if (const auto shooter = data.shooter.get()) {
                    const auto from = shooter->GetPosition(), to = target->GetPosition();
                    distance = ArcheryHitShake::ImpactDistance({from.x, from.y, from.z}, {to.x, to.y, to.z});
                }
            }
            // The next handler may destroy the projectile. Only copied values
            // cross this call; neither impacts nor trajectories are changed.
            originalImpact(projectile, target, position, velocity, collidable, arg6, arg7);
            if (eligible) bridge.Contact(id, magic, targetID, travel, now, distance);
        }

        void UpdateCone(RE::Projectile* projectile, float dt)
        {
            if (projectile) CameraNoiseController::NotifyNpcMagicFlight(projectile, projectile->GetPosition());
            originalConeUpdate(projectile, dt);
        }

        void ConeImpact(RE::Projectile* projectile, RE::TESObjectREFR* target, const RE::NiPoint3& position,
                        const RE::NiPoint3& velocity, RE::hkpCollidable* collidable, std::int32_t arg6, std::uint32_t arg7)
        {
            // Moving area waves survive actor contacts. A contact location is
            // on the cone's surface, not an observed flight position; use its
            // center and only cancel tracking when the wave hits the player.
            const bool hitPlayer = target && target->IsPlayerRef();
            if (projectile) CameraNoiseController::NotifyNpcMagicFlight(
                projectile, projectile->GetPosition(), hitPlayer, hitPlayer);
            originalConeImpact(projectile, target, position, velocity, collidable, arg6, arg7);
        }

        RE::MagicItem* HandMagic(RE::PlayerCharacter* player, bool left, RE::TESObjectWEAP*& staff)
        {
            auto* equipped = player->GetEquippedObject(left);
            staff = equipped ? equipped->As<RE::TESObjectWEAP>() : nullptr;
            if (staff && !staff->IsStaff()) { staff = nullptr; return nullptr; }
            RE::MagicItem* magic = equipped ? equipped->As<RE::MagicItem>() : nullptr;
            if (staff) {
                magic = staff->formEnchanting;
                if (auto* entry = player->GetEquippedEntryData(left); entry && entry->object == staff)
                    if (auto* enchantment = entry->GetEnchantment()) magic = enchantment;
            }
            return magic;
        }

        SettingsManager::NoiseProfile* ProfileFor(RE::PlayerCharacter* player, bool fp, bool left,
                                                  MagicHitShake::Profile* snapshot = nullptr, bool forceBoth = false)
        {
            if (!player) return nullptr;
            RE::TESObjectWEAP* staff = nullptr;
            auto* magic = HandMagic(player, left, staff);
            if (!EligibleMagic(magic)) return nullptr;
            const auto& sr = StateResolver::GetSingleton();
            auto* caster = player->GetMagicCaster(left ? Source::kLeftHand : Source::kRightHand);
            int hand = !staff && forceBoth ? 1 : left ? 0 : 2;
            const auto cast = ResolveMagicCastType(magic, staff ? MagicCastSource::Staff : MagicCastSource::Spell);
            std::string key;
            if (sr.IsVampireLord()) key = "transformations.vampire_lord.fire_and_forget";
            else {
                auto* effect = magic->GetCostliestEffectItem();
                if (!effect || !effect->baseEffect) return nullptr;
                const auto skill = effect->baseEffect->GetMagickSkill();
                const std::array skills{RE::ActorValue::kAlteration, RE::ActorValue::kConjuration, RE::ActorValue::kDestruction,
                                        RE::ActorValue::kIllusion, RE::ActorValue::kRestoration};
                auto school = std::find(skills.begin(), skills.end(), skill);
                if (school == skills.end()) return nullptr;
                if (!staff && caster && caster->currentSpell == magic && sr.GetCastingHand() == CastingHand::Both &&
                    sr.GetSchool() == static_cast<MagicSchool>(1 + (school - skills.begin())) && sr.GetCastType() == cast)
                    hand = 1;
                key = staff ? "staves." : "magic.";
                key += HitShake::kMagicSchools[school - skills.begin()];
                key += sr.IsSneaking() ? ".sneak." : ".";
                key += cast == CastType::Ritual ? "ritual" : "fire_and_forget";
                if (!staff) key += std::string(".hand.") + SettingsManager::GetMagicHandTomlKey(hand);
            }
            std::string resolved;
            auto* profile = SettingsManager::GetSingleton().ResolveHitShakeProfile(key, fp, MeleeWeaponType::Unarmed,
                ItemBindings::DescribeEquipped(player, left), {}, &resolved);
            if (snapshot) {
                snapshot->magic = magic->GetFormID(); snapshot->staff = staff ? staff->GetFormID() : 0; snapshot->hand = hand;
                snapshot->ritual = cast == CastType::Ritual;
                if (profile) snapshot->tuning = profile->hitShake;
                std::snprintf(snapshot->entry.data(), snapshot->entry.size(), "%s", (resolved.empty() ? key : resolved).c_str());
            }
            return profile;
        }

        struct CastSink final : RE::BSTEventSink<SKSE::ActionEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* event, RE::BSTEventSource<SKSE::ActionEvent>*) override
            {
                if (event && event->actor && event->actor->IsPlayerRef() && event->type == SKSE::ActionEvent::Type::kSpellCast)
                    bridge.BeginCast(static_cast<int>(event->slot.get()), Now());
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void Subscribe()
        {
            static CastSink sink;
            static bool subscribed = false;
            if (!subscribed) if (auto* source = SKSE::GetActionEventSource()) {
                source->AddEventSink(&sink);
                subscribed = true;
            }
        }
    }

    void Install()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_MissileProjectile[0]};
        originalUpdate = reinterpret_cast<UpdateFn>(table.write_vfunc(0xAB, &UpdateProjectile));
        originalImpact = reinterpret_cast<ImpactFn>(table.write_vfunc(0xBD, &AddImpact));
        REL::Relocation<std::uintptr_t> coneTable{RE::VTABLE_ConeProjectile[0]};
        originalConeUpdate = reinterpret_cast<UpdateFn>(coneTable.write_vfunc(0xAB, &UpdateCone));
        originalConeImpact = reinterpret_cast<ImpactFn>(coneTable.write_vfunc(0xBD, &ConeImpact));
        Subscribe();
        spdlog::info("[HITSHAKE-MAGIC] native missile launch/flyby/contact and moving-cone flyby/contact observers installed");
    }

    void Reset() { bridge.SetView(-1); }

    bool Available()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        RE::TESObjectWEAP* staff = nullptr;
        return EligibleMagic(HandMagic(player, false, staff)) || EligibleMagic(HandMagic(player, true, staff));
    }

    HitShake::Tuning* CurrentTuning(bool fp)
    {
        const auto state = StateResolver::GetSingleton().GetState();
        if (state != CameraState::Magic && state != CameraState::Staves && state != CameraState::VampireLordFireAndForget)
            return nullptr; // a spell in the spare hand must not hijack melee Quick Tune
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool left = StateResolver::GetSingleton().GetCastingHand() == CastingHand::Left;
        auto* profile = ProfileFor(player, fp, left);
        if (!profile) profile = ProfileFor(player, fp, !left);
        return profile ? &profile->hitShake : nullptr;
    }

    void Update(RE::PlayerCharacter* player, RE::PlayerCamera* camera, bool fp, double now, HitShake::Mixer& mixer)
    {
        Subscribe();
        bridge.SetView(fp ? 1 : 0);
        std::array<MagicHitShake::Profile, 4> prepared{};
        ProfileFor(player, fp, true, &prepared[0]);
        ProfileFor(player, fp, false, &prepared[1]);
        ProfileFor(player, fp, true, &prepared[2], true);
        ProfileFor(player, fp, false, &prepared[3], true);
        bridge.Publish(prepared, now);
        const auto& m = camera->cameraRoot->world.rotate;
        const HitShakeMotion::Basis basis{{m.entry[0][0], m.entry[1][0], m.entry[2][0]},
                                         {m.entry[0][1], m.entry[1][1], m.entry[2][1]},
                                         {m.entry[0][2], m.entry[1][2], m.entry[2][2]}};
        const double contactNow = Now();
        std::array<MagicHitShake::Impact, 32> impacts;
        const auto count = bridge.Drain(contactNow, impacts);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& hit = impacts[i];
            const auto axes = ArcheryHitShake::ImpactAxes(hit.velocity, basis, fp);
            const auto tuning = ArcheryHitShake::ImpactTuning(hit.profile.tuning, fp, hit.distance);
            static std::array<std::array<int, 2>, 2> diagnostics{};
            auto& lines = diagnostics[fp ? 1 : 0][hit.profile.tuning.strength > 0 ? 1 : 0];
            if (lines < 32) {
                ++lines;
                spdlog::debug("[HITSHAKE-MAGIC] view={} projectile={:08X} reference={:08X} target={:08X} spell={:08X} staff={:08X} hand={} entry={} strength={:.2f} distance={:.1f} falloff={:.3f} effective={:.2f} flight_ms={:.1f} queue_ms={:.2f}",
                    fp ? "1p" : "3p", hit.projectile.form, hit.projectile.reference, hit.target, hit.profile.magic,
                    hit.profile.staff, hit.profile.hand, hit.profile.entry.data(), hit.profile.tuning.strength,
                    hit.distance, ArcheryHitShake::DistanceScale(hit.distance), tuning.strength,
                    (hit.time - hit.launchedAt) * 1000, (contactNow - hit.time) * 1000);
            }
            mixer.Arm(tuning, axes, hit.projectile.reference ^ hit.target,
                      fp, static_cast<float>(contactNow - hit.time), HitShake::Onset::Contact);
        }
    }
}
