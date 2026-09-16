#include "PCH.h"
#include "Camera/HitShakeController.h"
#include "Camera/ArcheryHitShakeController.h"
#include "Camera/MagicHitShakeController.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/StateResolver.h"
#include "Camera/VanityCamera.h"
#include "Core/HitShakeMotion.h"
#include "Settings/EquippedItemBinding.h"
#include "Settings/SettingsManager.h"
#include <RE/T/TESHitEvent.h>
#include <chrono>

namespace DietDrCamera::HitShakeController
{
    namespace
    {
        HitShake::ContactQueue queue;
        HitShake::Mixer mixer;
        double lastTick = 0.0;
        std::array<std::string, 2> attackKeys;
        std::array<double, 2> attackTimes{};
        std::array<std::array<RE::FormID, 2>, 2> attackWeapons{};

        namespace Motion = HitShakeMotion;
        Motion::Vector Vector(const RE::NiPoint3& p) { return {p.x, p.y, p.z}; }
        Motion::Basis Basis(const RE::NiMatrix3& m)
        {
            return {{m.entry[0][0], m.entry[1][0], m.entry[2][0]},
                    {m.entry[0][1], m.entry[1][1], m.entry[2][1]},
                    {m.entry[0][2], m.entry[1][2], m.entry[2][2]}};
        }

        struct TrackedHand
        {
            Motion::History history;
            Motion::Vector localTip{};
            RE::NiTransform root{};
            RE::FormID weapon = 0;
            bool unarmed = false;
            bool valid = false;
        };
        std::array<TrackedHand, 2> hands;

        RE::NiAVObject* WeaponObject(RE::PlayerCharacter* player, RE::NiAVObject* rig,
                                    RE::TESObjectWEAP* weapon, bool firstPerson, bool left)
        {
            // The equipped clone follows the rendered weapon even when a
            // skeleton changes its attachment. Identical dual-wield forms
            // retain distinct right-weapon and left/shield biped slots.
            if (auto biped = player->GetBiped(firstPerson)) {
                if (left) {
                    auto& part = biped->objects[RE::BIPED_OBJECT::kShield];
                    if (part.item == weapon && part.partClone) return part.partClone.get();
                } else for (int slot = RE::BIPED_OBJECT::kOneHandSword; slot <= RE::BIPED_OBJECT::kCrossbow; ++slot) {
                    auto& part = biped->objects[slot];
                    if (part.item == weapon && part.partClone) return part.partClone.get();
                }
            }
            return rig->GetObjectByName(left ? "SHIELD" : "WEAPON");
        }

        Motion::Vector WeaponTip(RE::NiAVObject* object)
        {
            // Find a representative striking point on the actual mesh once
            // per equipped clone. Triangle bounds exclude particle trails and
            // preserve arbitrary model orientation; no weapon-type angle table.
            std::array<RE::NiAVObject*, 128> pending{};
            std::size_t count = 1, visited = 0;
            pending[0] = object;
            Motion::Vector tip{0, 50, 0};
            float extent = 0;
            while (count && visited++ < pending.size()) {
                auto* part = pending[--count];
                if (auto* geometry = part->AsGeometry()) {
                    using Type = RE::BSGeometry::Type;
                    if (geometry->GetType().any(Type::kTriShape, Type::kDynamicTriShape, Type::kMeshLODTriShape,
                            Type::kLODMultiIndexTriShape, Type::kMultiIndexTriShape, Type::kSubIndexTriShape)) {
                        const auto& bound = geometry->GetModelData().modelBound;
                        const auto center = Basis(object->world.rotate).ToLocal(Motion::RelativeToRoot(
                            Vector(geometry->world * bound.center), Vector(object->world.translate), object->world.scale));
                        const float radius = bound.radius * std::abs(geometry->world.scale / object->world.scale);
                        const float distance = center.Length();
                        if (center.Finite() && std::isfinite(radius) && radius > 0 && distance > 1 &&
                            distance + radius > extent && distance + radius <= 500) {
                            extent = distance + radius;
                            tip = center * ((distance + radius * 0.75f) / distance);
                        }
                    }
                }
                if (auto* node = part->AsNode()) for (const auto& child : node->GetChildren())
                    if (child && count < pending.size()) pending[count++] = child.get();
            }
            return tip;
        }

        void SampleHands(RE::PlayerCharacter* player, bool firstPerson, double now)
        {
            for (int hand = 0; hand < 2; ++hand) {
                auto& sample = hands[hand];
                const bool left = hand == 1;
                auto* item = player->GetEquippedObject(left);
                auto* weapon = item ? item->As<RE::TESObjectWEAP>() : nullptr;
                const bool unarmed = !item || (weapon && weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee);
                sample.valid = false;
                if (!unarmed && (!weapon || weapon->IsBow() || weapon->IsCrossbow() || weapon->IsStaff())) {
                    sample.history.Clear();
                    continue;
                }
                RE::NiAVObject* rig = nullptr;
                RE::NiAVObject* object = nullptr;
                // Use the visible POV rig. Transformations or body-camera mods
                // can lack a first-person hand mesh, so try the third-person rig.
                for (bool fp : {firstPerson, false}) {
                    rig = player->Get3D(fp);
                    if (!rig || (fp && rig->GetAppCulled())) continue;
                    object = unarmed ? rig->GetObjectByName(left ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]")
                                     : WeaponObject(player, rig, weapon, fp, left);
                    if (object) break;
                }
                if (!rig || !object || !std::isfinite(rig->world.scale) || rig->world.scale <= 0 ||
                    !std::isfinite(object->world.scale) || object->world.scale <= 0) {
                    sample.history.Clear();
                    continue;
                }
                const auto source = item ? item->GetFormID() : 0;
                if (sample.history.Bind(reinterpret_cast<std::uintptr_t>(rig), reinterpret_cast<std::uintptr_t>(object),
                                        source, object->world.scale / rig->world.scale)) {
                    sample.localTip = unarmed ? Motion::Vector{} : WeaponTip(object);
                }
                sample.root = rig->world;
                sample.weapon = source;
                sample.unarmed = unarmed;
                const RE::NiPoint3 tip{sample.localTip.x, sample.localTip.y, sample.localTip.z};
                auto relative = [&](const RE::NiPoint3& point) {
                    return Motion::RelativeToRoot(Vector(point), Vector(sample.root.translate), sample.root.scale);
                };
                sample.history.Add({now, relative(object->world.translate), relative(object->world * tip)});
                sample.valid = true;
            }
        }

        bool MotionAxes(const HitShake::Contact& hit, bool left, bool unarmed,
                        const RE::NiMatrix3& camera, bool firstPerson, HitShake::Rotation& axes)
        {
            const auto& sample = hands[left ? 1 : 0];
            if (!sample.valid || (sample.weapon != hit.source && !(sample.unarmed && unarmed))) return false;
            Motion::Vector target{};
            bool targetKnown = false;
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(hit.target)) {
                auto position = ref->GetPosition();
                if (const auto* root = ref->Get3D(); root && root->worldBound.radius > 0 && root->worldBound.radius < 2000)
                    position = root->worldBound.center;
                else position.z += 50;
                target = Motion::RelativeToRoot(Vector(position), Vector(sample.root.translate), sample.root.scale);
                targetKnown = target.Finite();
            }
            const auto motion = sample.history.At(hit.time, target, targetKnown);
            if (!motion.valid) return false;
            // History removes common translation/scale but retains body spins.
            // Project world-space motion into the fresh, unshaken camera root
            // (right/forward/up columns, unlike the child NiCamera convention).
            axes = Motion::Axes(motion.velocity, Basis(camera), axes, firstPerson);
            return true;
        }

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event,
                RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                // Skyrim's native damage events own every impact. No damage
                // interception, physics writes, or inventory reads here.
                if (!event || !event->cause || !event->cause->IsPlayerRef() || !event->target ||
                    event->target->IsPlayerRef() || event->projectile ||
                    event->flags.any(RE::TESHitEvent::Flag::kBashAttack)) return RE::BSEventNotifyControl::kContinue;
                HitShake::Contact contact{event->source, event->target->GetFormID(), Now(),
                    event->flags.any(RE::TESHitEvent::Flag::kPowerAttack),
                    event->flags.any(RE::TESHitEvent::Flag::kSneakAttack)};
                // Capture the animation's hand while this hit is dispatched;
                // dual-wield combos may change it before the camera drains.
                auto* player = event->cause->As<RE::Actor>();
                auto* proc = player ? player->GetActorRuntimeData().currentProcess : nullptr;
                bool attackPower = false;
                if (proc && proc->high) {
                    const RE::NiPointer<RE::BGSAttackData> attack = proc->high->attackData;
                    if (attack) {
                        if (attack->data.flags.any(RE::AttackData::AttackFlag::kBashAttack))
                            return RE::BSEventNotifyControl::kContinue;
                        contact.left = attack->IsLeftAttack();
                        contact.leftKnown = true;
                        attackPower = attack->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
                    }
                }
                contact.power = HitShake::ContactPower(contact.power, attackPower,
                    StateResolver::GetSingleton().HasLivePowerAttackHint());
                queue.Push(contact);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        HitShake::Family Family()
        {
            const auto& sr = StateResolver::GetSingleton();
            switch (sr.GetState()) {
            case CameraState::Melee: return HitShake::Family::Melee;
            case CameraState::Werewolf: return HitShake::Family::Werewolf;
            case CameraState::VampireLordMelee: return HitShake::Family::VampireLord;
            case CameraState::Horseback: return HitShake::Family::Mounted;
            default: return HitShake::Family::None;
            }
        }

        std::string CurrentKey(bool power)
        {
            const auto& sr = StateResolver::GetSingleton();
            const auto family = Family();
            if (family != HitShake::Family::Mounted && sr.GetSubState() != CameraSubState::Attack) return {};
            const auto side = sr.GetMountAttackSide();
            return std::string(HitShake::AttackKey(family, power, sr.IsAttackSprint(), sr.IsAttackSneak(),
                static_cast<int>(sr.GetPowerAttackDirection()),
                side == StateResolver::MountAttackSide::Left ? -1 : side == StateResolver::MountAttackSide::Right ? 1 : 0));
        }

        SettingsManager::NoiseProfile* ProfileFor(RE::PlayerCharacter* player, std::string_view key,
                                                  bool firstPerson, RE::TESForm* source, bool& left,
                                                  std::string* outKey = nullptr)
        {
            auto& settings = SettingsManager::GetSingleton();
            auto* equipped = player->GetEquippedObject(left);
            if (equipped != source && player->GetEquippedObject(!left) == source) {
                // Return the corrected striking hand as well as its profile;
                // automatic direction must use the same hand as item routing.
                left = !left;
                equipped = source;
            }
            const auto item = equipped == source ? ItemBindings::DescribeEquipped(player, left) : ItemBindings::DescribeForm(source);
            std::string_view customKeyword;
            if (auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr) for (const auto& custom : settings.customWeaponTypes) {
                auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(custom.keyword);
                if (kw && weapon->HasKeyword(kw)) { customKeyword = custom.keyword; break; }
            }
            return settings.ResolveHitShakeProfile(key, firstPerson, ClassifyMeleeWeaponForm(source), item, customKeyword, outKey);
        }
    }

    void Reset()
    {
        ArcheryHitShakeController::Reset();
        MagicHitShakeController::Reset();
        queue.SetView(-1);
        mixer.Clear();
        attackKeys = {};
        attackTimes = {};
        attackWeapons = {};
        hands = {};
        lastTick = 0.0;
    }

    HitShake::Tuning* CurrentTuning(bool firstPerson)
    {
        if (auto* archery = ArcheryHitShakeController::CurrentTuning(firstPerson)) return archery;
        if (auto* magic = MagicHitShakeController::CurrentTuning(firstPerson)) return magic;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        const auto key = CurrentKey(StateResolver::GetSingleton().IsPowerAttacking());
        if (key.empty()) return nullptr;
        auto* proc = player->GetActorRuntimeData().currentProcess;
        bool left = proc && proc->high && proc->high->attackData && proc->high->attackData->IsLeftAttack();
        if (auto* p = ProfileFor(player, key, firstPerson, player->GetEquippedObject(left), left)) return &p->hitShake;
        return nullptr;
    }

    HitShake::Mixer::Rotation Update(bool cameraAvailable)
    {
        static HitSink sink;
        static bool subscribed = false;
        if (!subscribed) if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(&sink);
            subscribed = true;
            spdlog::debug("[HITSHAKE] player melee hit event sink installed");
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto& settings = SettingsManager::GetSingleton();
        const auto state = camera && camera->currentState ? camera->currentState->id : RE::CameraState::kTotal;
        const bool firstPerson = camera && camera->IsInFirstPerson();
        const bool allowedState = firstPerson || state == RE::CameraState::kThirdPerson || state == RE::CameraState::kMount;
        if (!cameraAvailable || !player || !player->Is3DLoaded() || player->IsDead() || player->IsInKillMove() ||
            !ui || ui->GameIsPaused() || CameraEffectClock::IsPaused() || ui->IsMenuOpen("Dialogue Menu") ||
            !allowedState || VanityCamera::IsActive() || settings.diagnosticSuspendOverrides) {
            Reset();
            return {};
        }
        const double now = Now();
        if (lastTick > 0.0 && now - lastTick > 0.25) queue.SetView(-1);
        if (queue.SetView(firstPerson ? 1 : 0)) {
            ArcheryHitShakeController::Reset();
            MagicHitShakeController::Reset();
            // First frame after a handoff cannot borrow the previous view's
            // attack context or an already running pulse.
            mixer.Clear();
            attackKeys = {};
            attackTimes = {};
            attackWeapons = {};
            hands = {};
        }
        const float dt = lastTick > 0.0 ? static_cast<float>(now - lastTick) : 0.0f;
        lastTick = now;
        SampleHands(player, firstPerson, now);
        // Advance existing tails before adding contacts. A new hit starts at
        // its actual age, not one entire camera frame before it happened.
        mixer.Advance(dt);
        ArcheryHitShakeController::Update(player, camera, firstPerson, now, mixer);
        MagicHitShakeController::Update(player, camera, firstPerson, now, mixer);
        const auto& sr = StateResolver::GetSingleton();
        const std::string currentKey = CurrentKey(sr.IsPowerAttacking());
        if (HitShake::IsAttackKey(currentKey)) {
            const int power = currentKey.find("power_attack") != currentKey.npos ? 1 : 0;
            attackKeys[power] = currentKey;
            attackTimes[power] = now;
            for (int hand = 0; hand < 2; ++hand) {
                const auto* object = player->GetEquippedObject(hand == 1);
                attackWeapons[power][hand] = object ? object->GetFormID() : 0;
            }
        }

        std::array<HitShake::Contact, 32> contacts;
        const auto count = queue.Drain(now, contacts);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& hit = contacts[i];
            auto* source = hit.source ? RE::TESForm::LookupByID(hit.source) : nullptr;
            auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
            // Reject enchantment/spell ticks (including concentration and DoT)
            // and every ranged weapon. Source zero is native unarmed contact.
            if (hit.source && !weapon) continue;
            if (weapon && (weapon->IsBow() || weapon->IsCrossbow() || weapon->IsStaff())) continue;
            const bool beast = Family() == HitShake::Family::Werewolf || Family() == HitShake::Family::VampireLord;
            const int power = hit.power || (beast && sr.IsPowerAttacking()) ? 1 : 0;
            const bool sourceMatches = hit.source == attackWeapons[power][0] || hit.source == attackWeapons[power][1] ||
                (weapon && weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee);
            std::string key = sourceMatches && !attackKeys[power].empty() && now - attackTimes[power] <= 0.25
                ? attackKeys[power] : CurrentKey(power != 0);
            if (key.empty()) continue; // no active/recent swing: ignore attributed trap/script damage

            bool left = false;
            auto* proc = player->GetActorRuntimeData().currentProcess;
            if (proc && proc->high && proc->high->attackData) left = proc->high->attackData->IsLeftAttack();
            if (hit.leftKnown) left = hit.left;
            std::string resolvedKey;
            const auto* p = ProfileFor(player, key, firstPerson, source, left, &resolvedKey);
            const int meleeIndex = HitShake::MeleeIndex(key);
            const int direction = meleeIndex >= 6 ? meleeIndex - 6 : -1;
            auto axes = HitShake::AutomaticAxes(left, direction, firstPerson);
            const bool measured = MotionAxes(hit, left, !weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee,
                                             camera->cameraRoot->world.rotate, firstPerson, axes);
            // Separate budgets keep normal-hit testing from hiding the first
            // power contacts or explicit-zero decisions later in the session.
            static std::array<int, 4> diagnostics{};
            auto& lines = diagnostics[power * 2 + (p && p->hitShake.strength > 0.0f ? 1 : 0)];
            if (lines < 24) {
                ++lines;
                spdlog::debug("[HITSHAKE] view={} target={:08X} weapon={:08X} left={} power={} requested={} entry={} strength={:.2f} direction={} axes=({:.2f},{:.2f},{:.2f})",
                    firstPerson ? "1p" : "3p", hit.target, hit.source, left, power != 0, key, resolvedKey,
                    p ? p->hitShake.strength : 0.0f, measured ? "motion" : "attack", axes.pitch, axes.yaw, axes.roll);
            }
            if (!p || p->hitShake.strength <= 0.0f) continue;
            static std::uint32_t sequence = 0;
            const auto seed = (hit.target * 1664525u) ^ hit.source ^ (++sequence * 1013904223u);
            mixer.Arm(p->hitShake, axes, seed,
                      firstPerson, static_cast<float>(now - hit.time));
        }
        return mixer.Advance(0.0f);
    }
}
