#include "PCH.h"
#include "Camera/DamageReactionController.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/VanityCamera.h"
#include "Core/DamageReactionAttacks.h"
#include "Camera/CreatureMagic.h"
#include "Settings/SettingsManager.h"
#include <chrono>

namespace DietDrCamera::DamageReactionController
{
    namespace
    {
        namespace Reaction = DamageReaction;
        Reaction::Inbox inbox;
        Reaction::Mixer mixer;
        Reaction::AttackClips attackClips;
        double lastTick = 0;
        double lastScopeScan = 0;
        thread_local unsigned captureDepth = 0;
        struct CaptureScope { CaptureScope() { ++captureDepth; } ~CaptureScope() { --captureDepth; } };
        double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        std::string_view Text(const char* value) { return value ? value : ""; }
        Reaction::Family Family(RE::Actor* actor)
        {
            const auto* race = actor ? actor->GetRace() : nullptr;
            if (!race) return Reaction::Family::Other;
            return Reaction::CreatureFamily(Text(race->GetFormEditorID()), Text(race->skeletonModels[0].GetModel()),
                race->HasKeywordString("ActorTypeGiant"), race->HasKeywordString("ActorTypeDragon"),
                race->HasKeywordString("ActorTypeCreature") || race->HasKeywordString("ActorTypeAnimal"),
                race->HasKeywordString("ActorTypeDwarven"));
        }
        void RefreshAttackScopes(RE::PlayerCharacter* player, double now)
        {
            if (lastScopeScan && now-lastScopeScan < .20) return;
            lastScopeScan = now;
            std::array<Reaction::AttackClips::Scope, 16> scopes{};
            std::size_t count = 0;
            if (auto* processes = RE::ProcessLists::GetSingleton()) for (const auto& handle : processes->highActorHandles) {
                const auto actor = handle.get();
                if (!actor || actor.get() == player || !actor->Is3DLoaded() || actor->IsDead() ||
                    actor->GetPosition().GetDistance(player->GetPosition()) > 1800 || !Reaction::NeedsAttackClip(Family(actor.get()))) continue;
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
                if (!actor->GetAnimationGraphManager(manager) || !manager) continue;
                for (const auto& graph : manager->graphs) if (graph && count < scopes.size())
                    scopes[count++] = {reinterpret_cast<std::uintptr_t>(&graph->characterInstance), actor->GetFormID()};
                if (count == scopes.size()) break;
            }
            attackClips.SetScopes(scopes, count);
        }

        Reaction::Vector TowardSource(RE::TESObjectREFR* victim, RE::TESObjectREFR* source)
        {
            if (!source || source == victim) return {};
            const auto direction = source->GetPosition() - victim->GetPosition();
            return {direction.x, direction.y, direction.z};
        }
        Reaction::Contact Snapshot(RE::TESObjectREFR* victim, RE::TESObjectREFR* source)
        {
            Reaction::Contact hit;
            hit.time = Now();
            hit.attacker = source ? source->GetFormID() : 0;
            hit.kind = source && source != victim ? Reaction::Kind::Unarmed : Reaction::Kind::Environment;
            hit.towardSource = TowardSource(victim, source);
            return hit;
        }
        Reaction::Kind WeaponKind(RE::TESForm* form)
        {
            const auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon) return Reaction::Kind::Unarmed;
            switch (weapon->GetWeaponType()) {
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow: return Reaction::Kind::Arrow;
            case RE::WEAPON_TYPE::kOneHandDagger:
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kOneHandAxe: return Reaction::Kind::Blade;
            case RE::WEAPON_TYPE::kTwoHandSword: return Reaction::Kind::HeavyBlade;
            case RE::WEAPON_TYPE::kOneHandMace: return Reaction::Kind::Blunt;
            case RE::WEAPON_TYPE::kTwoHandAxe:
                return weapon->HasKeywordString("WeapTypeBattleaxe") ? Reaction::Kind::HeavyBlade : Reaction::Kind::HeavyBlunt;
            default: return Reaction::Kind::Unarmed;
            }
        }
        Reaction::Kind MagicKind(const RE::EffectSetting* effect)
        {
            return CreatureMagic::EffectKind(CreatureMagic::Describe(effect));
        }

        // Each concrete magic-effect vtable keeps its own chained original.
        // Only damaging health-value applications count as magic contacts.
        // Utility debuffs (including block slowdown) use the same callback.
        // Tick magnitude identifies loss versus restoration; it never scales motion.
        template <class Effect>
        struct EffectObserver
        {
            using Fn = void(*)(RE::ValueModifierEffect*, RE::Actor*, float, RE::ActorValue);
            static inline Fn original = nullptr;
            static void Apply(RE::ValueModifierEffect* effect, RE::Actor* target, float amount, RE::ActorValue value)
            {
                if (captureDepth || !target || !target->IsPlayerRef() || !std::isfinite(amount)) {
                    original(effect, target, amount, value);
                    return;
                }
                const auto* base = effect->GetBaseObject();
                if (!base || !Reaction::IsHarmfulEffectTick(amount, base->IsHostile(), base->IsDetrimental(),
                    value == RE::ActorValue::kHealth)) {
                    original(effect, target, amount, value);
                    return;
                }
                const auto token = inbox.Token();
                if (!token) { original(effect, target, amount, value); return; }
                const auto caster = effect->GetCasterActor();
                auto hit = Snapshot(target, caster.get());
                hit.kind = MagicKind(base);
                hit.origin = Reaction::Contact::Origin::Effect;
                hit.source = effect->spell ? effect->spell->GetFormID() : base ? base->GetFormID() : 0;
                hit.sustained = effect->duration > 0 ||
                    (base && base->data.castingType == RE::MagicSystem::CastingType::kConcentration);
                {
                    CaptureScope scope;
                    original(effect, target, amount, value);
                }
                inbox.Push(hit, token);
            }
            static void Install()
            {
                REL::Relocation<std::uintptr_t> table{Effect::VTABLE[0]};
                original = reinterpret_cast<Fn>(table.write_vfunc(0x20, Apply));
            }
        };
        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                if (!event || !event->target || !event->target->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
                const auto token = inbox.Token();
                if (!token) return RE::BSEventNotifyControl::kContinue;
                auto* source = event->source ? RE::TESForm::LookupByID(event->source) : nullptr;
                const bool bash = event->flags.any(RE::TESHitEvent::Flag::kBashAttack);
                if (auto* explosion = source ? source->As<RE::BGSExplosion>() : nullptr; explosion && !bash) {
                    // Enchanted explosions deliver their element through the
                    // existing damage callback. Native blast-only damage and
                    // stagger enchantments otherwise have no value callback.
                    bool healthEffect = false;
                    if (explosion->formEnchanting) for (const auto* effect : explosion->formEnchanting->effects) {
                        if (!effect || !effect->baseEffect) continue;
                        const auto info = CreatureMagic::Describe(effect->baseEffect);
                        healthEffect |= CreatureMagic::Harmful(info) && CreatureMagic::AffectsHealth(info);
                    }
                    const auto control = CreatureMagic::Impact(explosion->formEnchanting);
                    if (!healthEffect && (control || explosion->data.damage > 0)) {
                        auto hit = Snapshot(event->target.get(), event->cause.get());
                        hit.kind = control.value_or(Reaction::Kind::Force);
                        hit.source = explosion->formEnchanting ? explosion->formEnchanting->GetFormID() : event->source;
                        hit.origin = Reaction::Contact::Origin::MagicImpact;
                        inbox.Push(hit, token);
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Actual magic hit events cover hostile control/resource
                // attacks which cannot produce a health-value callback.
                if (auto* magic = source ? source->As<RE::MagicItem>() : nullptr; magic && !bash) {
                    if (event->cause && event->cause != event->target) if (const auto kind = CreatureMagic::Impact(magic)) {
                        auto hit = Snapshot(event->target.get(), event->cause.get());
                        hit.kind = *kind; hit.source = event->source;
                        hit.origin = Reaction::Contact::Origin::MagicImpact;
                        hit.blocked = event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
                        inbox.Push(hit, token);
                        static std::atomic<unsigned> logged{0};
                        if (logged.fetch_add(1, std::memory_order_relaxed) < 64)
                            spdlog::info("[DAMAGE-MAGIC] hit attacker={:08X} spell={:08X} kind={} blocked={}",
                                hit.attacker, hit.source, Reaction::KindName(hit.kind), hit.blocked);
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Damaging ticks still arrive through the effect observers.
                // Shield bashes can name armor instead of a weapon.
                if (source && !source->As<RE::TESObjectWEAP>() && !bash) return RE::BSEventNotifyControl::kContinue;
                auto hit = Snapshot(event->target.get(), event->cause.get());
                hit.source = event->source;
                hit.kind = WeaponKind(source);
                hit.power = event->flags.any(RE::TESHitEvent::Flag::kPowerAttack);
                hit.blocked = event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
                auto* actor = event->cause ? event->cause->As<RE::Actor>() : nullptr;
                if (actor) {
                    const auto family = Family(actor);
                    const bool melee = hit.kind != Reaction::Kind::Arrow || bash;
                    const auto* process = melee ? actor->GetActorRuntimeData().currentProcess : nullptr;
                    const RE::NiPointer<RE::BGSAttackData> attack = process && process->high ? process->high->attackData : nullptr;
                    const auto attackName = attack ? Text(attack->event.c_str()) : std::string_view{};
                    const auto clip = process && Reaction::NeedsAttackClip(family) ? attackClips.At(hit.attacker, hit.time) : Reaction::AttackClips::Snapshot{};
                    hit.kind = Reaction::ClassifyAttack(family, hit.kind, attackName, clip.Name(), bash);
                    hit.power = hit.power || (attack && attack->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack));
                    const auto strike = Reaction::LocalStrike(hit.kind, attackName, clip.Name());
                    const float heading = actor->GetAngleZ();
                    const Reaction::Basis attackerBasis{{std::cos(heading),-std::sin(heading),0},
                        {std::sin(heading),std::cos(heading),0},{0,0,1}};
                    hit.strikeDirection = attackerBasis.ToWorld(strike);
                    static std::array<unsigned, static_cast<std::size_t>(Reaction::Family::Count)> logged{};
                    auto& budget = logged[static_cast<std::size_t>(family)];
                    if (budget < 24) {
                        ++budget;
                        const auto* race = actor->GetRace();
                        spdlog::info("[DAMAGE-ATTACK] attacker={:08X} race={} family={} kind={} event={} clip={} power={} blocked={}",
                            hit.attacker, race ? Text(race->GetFormEditorID()) : "", static_cast<int>(family),
                            Reaction::KindName(hit.kind), attackName, clip.Name(), hit.power, hit.blocked);
                    }
                } else if (bash) hit.kind = Reaction::Kind::Blunt;
                inbox.Push(hit, token);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Install()
    {
        static bool installed = false;
        if (installed) return;
        EffectObserver<RE::ValueModifierEffect>::Install();
        EffectObserver<RE::DualValueModifierEffect>::Install();
        EffectObserver<RE::PeakValueModifierEffect>::Install();
        EffectObserver<RE::AbsorbEffect>::Install();
        EffectObserver<RE::AccumulatingValueModifierEffect>::Install();
        EffectObserver<RE::TargetValueModifierEffect>::Install();
        EffectObserver<RE::ValueAndConditionsEffect>::Install();
        installed = true;
        spdlog::info("[DAMAGE-REACTION] weapon, discrete magic contact and harmful tick observers installed; continuous breath remains a stream; no health scaling");
    }
    void ObserveProjectile(RE::Projectile* projectile, RE::TESObjectREFR* target)
    {
        if (!projectile || !target || !target->IsPlayerRef()) return;
        const auto token = inbox.Token();
        if (!token) return;
        const auto& data = projectile->GetProjectileRuntimeData();
        const auto source = data.shooter.get();
        Reaction::Trajectory context;
        context.time = Now();
        context.attacker = source ? source->GetFormID() : 0;
        context.source = data.spell ? data.spell->GetFormID() : data.weaponSource ? data.weaponSource->GetFormID() : 0;
        context.towardSource = {-data.velocity.x, -data.velocity.y, -data.velocity.z};
        inbox.Describe(context, token);
        // This hook runs at a real body contact, never at launch or proximity.
        // Poison spit has a distinct impact followed by the existing soft DOT.
        // Continuous dragon breath/centurion steam cannot enter this path.
        if (source && source.get() != target && data.spell) if (const auto kind = CreatureMagic::Impact(data.spell)) {
            auto hit = Snapshot(target, source.get());
            hit.kind = *kind; hit.source = context.source;
            hit.towardSource = context.towardSource;
            hit.origin = Reaction::Contact::Origin::MagicImpact;
            hit.projectile = projectile->GetFormID();
            inbox.Push(hit, token);
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 64)
                spdlog::info("[DAMAGE-MAGIC] contact attacker={:08X} spell={:08X} projectile={:08X} kind={}",
                    hit.attacker, hit.source, hit.projectile, Reaction::KindName(hit.kind));
        }
    }
    void ObserveClip(RE::hkbClipGenerator* clip, const RE::hkbCharacter* character, bool active, bool refresh)
    {
        if (!attackClips.Enabled() || !clip || !character) return;
        attackClips.Observe(reinterpret_cast<std::uintptr_t>(character), reinterpret_cast<std::uintptr_t>(clip),
            active ? Text(clip->name.c_str()) : std::string_view{}, active, refresh, Now());
    }
    void Reset() { inbox.Clear(); mixer.Clear(); attackClips.Clear(); lastTick = lastScopeScan = 0; }

    Reaction::Rotation Update(bool cameraAvailable)
    {
        static HitSink sink;
        static bool subscribed = false;
        if (!subscribed) if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESHitEvent>(&sink);
            subscribed = true;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto& settings = SettingsManager::GetSingleton();
        const bool fp = camera && camera->IsInFirstPerson();
        const auto state = camera && camera->currentState ? camera->currentState->id : RE::CameraState::kTotal;
        const auto tuning = Reaction::Sanitize(fp ? settings.damageReactionFp : settings.damageReaction);
        if (!cameraAvailable || !camera || !camera->cameraRoot || !player || !player->Is3DLoaded() ||
            player->IsDead() || player->IsInKillMove() || !ui || ui->GameIsPaused() || CameraEffectClock::IsPaused() ||
            ui->IsMenuOpen("Dialogue Menu") || VanityCamera::IsActive() || settings.diagnosticSuspendOverrides ||
            (!fp && state != RE::CameraState::kThirdPerson && state != RE::CameraState::kMount) || tuning.intensity <= 0) {
            Reset(); return {};
        }
        const double now = Now();
        if (lastTick && now - lastTick > 0.25) Reset();
        if (inbox.SetView(fp ? 1 : 0)) {
            mixer.Clear(); lastTick = now;
            static unsigned activations = 0;
            if (activations++ < 16) spdlog::info("[DAMAGE-REACTION] accepting impacts view={} intensity={:.2f}", fp ? "1p" : "3p", tuning.intensity);
        }
        const float dt = lastTick ? static_cast<float>(now - lastTick) : 0;
        lastTick = now;
        RefreshAttackScopes(player, now);
        mixer.Advance(dt, fp);
        const auto& rotation = camera->cameraRoot->world.rotate;
        const Reaction::Basis basis{{rotation.entry[0][0], rotation.entry[1][0], rotation.entry[2][0]},
            {rotation.entry[0][1], rotation.entry[1][1], rotation.entry[2][1]},
            {rotation.entry[0][2], rotation.entry[1][2], rotation.entry[2][2]}};
        std::array<Reaction::Contact, 64> contacts;
        const auto count = inbox.Drain(now, contacts);
        for (std::size_t i = 0; i < count; ++i) {
            mixer.Add(contacts[i], tuning, basis, fp, now);
            // Continuous ticks must not consume the discrete-impact or other
            // view's diagnostic budget before a weapon lands.
            static std::array<unsigned, 4> diagnostics{};
            auto& logged = diagnostics[(fp ? 2 : 0) + (contacts[i].sustained ? 1 : 0)];
            if (logged < 24) {
                ++logged;
                const auto axes = Reaction::ContactDirection(contacts[i], basis, fp);
                spdlog::info("[DAMAGE-REACTION] impact view={} kind={} intensity={:.2f} power={} blocked={} sustained={} source={:08X} direction=({:.2f},{:.2f},{:.2f}) recoilAxes=({:.3f},{:.3f},{:.3f})",
                    fp ? "1p" : "3p", Reaction::KindName(contacts[i].kind), tuning.intensity, contacts[i].power, contacts[i].blocked, contacts[i].sustained,
                    contacts[i].source, contacts[i].towardSource.x, contacts[i].towardSource.y, contacts[i].towardSource.z,
                    axes.pitch, axes.yaw, axes.roll);
            }
        }
        return mixer.Advance(0, fp);
    }
}
