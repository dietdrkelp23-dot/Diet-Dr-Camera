#include "PCH.h"
#include "Camera/StateResolver.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/AnimationCameraController.h"
#include "Camera/CameraNoiseController.h"
#include "Hooks/HookManager.h"
#include "LockOn/TDMIntegration.h"
#include "Settings/SettingsManager.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>

namespace DietDrCamera
{
    namespace
    {
        constexpr std::string_view kWerewolfRaceEDID    = "WerewolfBeastRace";
        constexpr std::string_view kVampireLordRaceEDID = "DLC1VampireBeastRace";

        bool IsWerewolfRace(const RE::TESRace* a_race)
        {
            if (!a_race) return false;
            const char* edid = a_race->GetFormEditorID();
            return edid && kWerewolfRaceEDID == edid;
        }

        bool IsVampireLordRace(const RE::TESRace* a_race)
        {
            if (!a_race) return false;
            const char* edid = a_race->GetFormEditorID();
            return edid && kVampireLordRaceEDID == edid;
        }

        // True if the form is a weapon whose type counts as melee. Null forms
        // (empty hand) return false here — the unarmed case is handled by the
        // caller, which has visibility into both hands.
        bool IsMeleeWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            if (!weap) return false;
            const auto type = weap->GetWeaponType();
            return type >= RE::WEAPON_TYPE::kHandToHandMelee &&
                   type <= RE::WEAPON_TYPE::kTwoHandAxe;
        }

        // Classify player's current loadout. Either hand may carry the melee
        // weapon — sword-right + staff-left and staff-right + sword-left both
        // need to flag as "melee equipped" so the mixed-loadout branch in
        // Update() can fire. Truly unarmed (both hands empty of any equipped
        // form) also counts as melee.
        bool PlayerHasMeleeEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return true;
            auto* right = a_player->GetEquippedObject(false);
            auto* left  = a_player->GetEquippedObject(true);
            if (IsMeleeWeapon(right)) return true;
            if (IsMeleeWeapon(left))  return true;
            if (!right && !left) return true; // unarmed
            return false;
        }

        bool IsBowWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            return weap && weap->GetWeaponType() == RE::WEAPON_TYPE::kBow;
        }

        bool IsCrossbowWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            return weap && weap->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow;
        }

        bool PlayerHasBowEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            return IsBowWeapon(a_player->GetEquippedObject(false));
        }

        bool PlayerHasCrossbowEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            return IsCrossbowWeapon(a_player->GetEquippedObject(false));
        }

        // True if `a_form` is a one-handed melee weapon. Excludes unarmed
        // (null), bows, crossbows, and staves. Matches the WEAPON_TYPE
        // values from kHandToHandMelee through kOneHandMace inclusive.
        bool IsOneHandedMeleeWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            if (!weap) return false;
            const auto type = weap->GetWeaponType();
            return type == RE::WEAPON_TYPE::kHandToHandMelee ||
                   type == RE::WEAPON_TYPE::kOneHandSword ||
                   type == RE::WEAPON_TYPE::kOneHandDagger ||
                   type == RE::WEAPON_TYPE::kOneHandAxe ||
                   type == RE::WEAPON_TYPE::kOneHandMace;
        }

        // True if `a_form` is a two-handed melee weapon (greatswords,
        // battleaxes, warhammers).
        bool IsTwoHandedMeleeWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            if (!weap) return false;
            const auto type = weap->GetWeaponType();
            return type == RE::WEAPON_TYPE::kTwoHandSword ||
                   type == RE::WEAPON_TYPE::kTwoHandAxe;
        }

        // Player has a 1H melee weapon equipped in either hand.
        bool PlayerHasOneHandedEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            if (IsOneHandedMeleeWeapon(a_player->GetEquippedObject(false))) return true;
            if (IsOneHandedMeleeWeapon(a_player->GetEquippedObject(true))) return true;
            return false;
        }

        // Player has a 2H melee weapon equipped (main hand only — the
        // 2H types occupy both slots).
        bool PlayerHasTwoHandedEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            return IsTwoHandedMeleeWeapon(a_player->GetEquippedObject(false));
        }

        // True if `a_form` is an Armor with the Shield biped slot set.
        // BGSBipedObjectForm::FirstPersonFlag::kShield = 1<<9.
        bool IsShieldArmor(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* armor = a_form->As<RE::TESObjectARMO>();
            if (!armor) return false;
            using Slot = RE::BGSBipedObjectForm::FirstPersonFlag;
            return (armor->bipedModelData.bipedObjectSlots & Slot::kShield) != Slot::kNone;
        }

        // Player has a shield equipped (left hand only — shields always go
        // in the off-hand slot).
        bool PlayerHasShieldEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            return IsShieldArmor(a_player->GetEquippedObject(true));
        }

        // True if the form is a staff.
        bool IsStaffWeapon(const RE::TESForm* a_form)
        {
            if (!a_form) return false;
            auto* weap = a_form->As<RE::TESObjectWEAP>();
            if (!weap) return false;
            return weap->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
        }

        // Staves can occupy either hand — dual-staff loadouts exist, and an
        // off-hand-only staff is common with a melee main-hand. Either slot
        // counts.
        bool PlayerHasStaffEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            if (IsStaffWeapon(a_player->GetEquippedObject(false))) return true;
            if (IsStaffWeapon(a_player->GetEquippedObject(true))) return true;
            return false;
        }

        // True if the form is a player spell (FormType::Spell). GetEquippedObject
        // does return spell forms when a spell is equipped to a hand.
        bool IsSpellForm(const RE::TESForm* a_form)
        {
            return a_form && a_form->Is(RE::FormType::Spell);
        }

        // Spells can occupy either hand independently — left-hand spell with
        // right-hand sword/staff, dual-cast same spell, or two different
        // spells. Any hand counts.
        bool PlayerHasSpellEquipped(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return false;
            if (IsSpellForm(a_player->GetEquippedObject(false))) return true;
            if (IsSpellForm(a_player->GetEquippedObject(true))) return true;
            return false;
        }

        // Map an ActorValue (from the costliest effect's associatedSkill) to
        // our MagicSchool enum. Anything outside the five magic skills falls
        // back to None.
        MagicSchool ActorValueToSchool(RE::ActorValue a_av)
        {
            switch (a_av) {
            case RE::ActorValue::kAlteration:  return MagicSchool::Alteration;
            case RE::ActorValue::kConjuration: return MagicSchool::Conjuration;
            case RE::ActorValue::kDestruction: return MagicSchool::Destruction;
            case RE::ActorValue::kIllusion:    return MagicSchool::Illusion;
            case RE::ActorValue::kRestoration: return MagicSchool::Restoration;
            default:                           return MagicSchool::None;
            }
        }

        // Walk a SpellItem to its costliest effect's base setting and read the
        // associated magic skill. The costliest effect is what Skyrim itself
        // uses to classify a spell's school in the UI.
        MagicSchool ResolveSpellSchool(const RE::TESForm* a_form)
        {
            if (!a_form) return MagicSchool::None;
            const auto* spell = a_form->As<RE::SpellItem>();
            if (!spell) return MagicSchool::None;
            auto* effect = spell->GetCostliestEffectItem();
            if (!effect || !effect->baseEffect) return MagicSchool::None;
            return ActorValueToSchool(effect->baseEffect->GetMagickSkill());
        }

        // Pick the player's "primary" spell school. Right hand wins; if right
        // hand has no spell, fall back to left.
        MagicSchool DetectPlayerSpellSchool(RE::PlayerCharacter* a_player)
        {
            if (!a_player) return MagicSchool::None;
            auto right = ResolveSpellSchool(a_player->GetEquippedObject(false));
            if (right != MagicSchool::None) return right;
            return ResolveSpellSchool(a_player->GetEquippedObject(true));
        }

        const char* SchoolName(MagicSchool s)
        {
            switch (s) {
            case MagicSchool::Alteration:  return "Alteration";
            case MagicSchool::Conjuration: return "Conjuration";
            case MagicSchool::Destruction: return "Destruction";
            case MagicSchool::Illusion:    return "Illusion";
            case MagicSchool::Restoration: return "Restoration";
            default:                       return "None";
            }
        }

        const char* CastTypeName(CastType c)
        {
            switch (c) {
            case CastType::Concentration: return "Concentration";
            case CastType::FireAndForget: return "FireAndForget";
            case CastType::Ritual:        return "Ritual";
            default:                      return "None";
            }
        }

        // Case-insensitive substring search for animation tag matching.
        // BSFixedString tags don't have a guaranteed case convention.
        bool ContainsCI(std::string_view a_haystack, std::string_view a_needle)
        {
            if (a_needle.empty() || a_needle.size() > a_haystack.size()) return false;
            for (size_t i = 0; i + a_needle.size() <= a_haystack.size(); ++i) {
                bool match = true;
                for (size_t j = 0; j < a_needle.size(); ++j) {
                    if (std::tolower(static_cast<unsigned char>(a_haystack[i + j])) !=
                        std::tolower(static_cast<unsigned char>(a_needle[j]))) {
                        match = false;
                        break;
                    }
                }
                if (match) return true;
            }
            return false;
        }

        // Resolve school from an EnchantmentItem (staff fires). Same approach
        // as ResolveSpellSchool but accepts the base MagicItem so it works
        // with enchantments that fail As<SpellItem>().
        MagicSchool ResolveEnchantmentSchool(const RE::MagicItem* a_magic)
        {
            if (!a_magic) return MagicSchool::None;
            auto* effect = a_magic->GetCostliestEffectItem();
            if (!effect || !effect->baseEffect) return MagicSchool::None;
            return ActorValueToSchool(effect->baseEffect->GetMagickSkill());
        }

        // Mod-added staves can carry the same ritual effects as hand spells.
        CastType ResolveEnchantmentCastType(const RE::MagicItem* a_magic)
        {
            return ResolveMagicCastType(a_magic, MagicCastSource::Staff);
        }

        // True if the spell is a Ward (Restoration Concentration spell whose
        // effects modify the WardPower actor value). The engine has no
        // dedicated ArchetypeID::kWard, so we identify wards by walking the
        // spell's effects and checking each base effect's primary AV against
        // ActorValue::kWardPower.
        bool IsWardSpell(const RE::SpellItem* a_spell)
        {
            if (!a_spell) return false;
            for (const auto* effect : a_spell->effects) {
                if (!effect || !effect->baseEffect) continue;
                const auto* base = effect->baseEffect;
                // PrimaryAV / SecondaryAV — the AV slots on the base effect.
                // Wards modify kWardPower as their primary AV.
                if (base->data.primaryAV == RE::ActorValue::kWardPower) return true;
                if (base->data.secondaryAV == RE::ActorValue::kWardPower) return true;
            }
            return false;
        }

        // Does this magic item actually LAUNCH something?
        //
        // Attack Lag ("Projectile Lag") exists to let the camera ride out a
        // shot that is still in flight. It was arming for every fire-and-
        // forget cast, including Oakflesh — a self-buff with nothing in
        // flight at all (user report 2026-08-16). The engine already knows
        // the answer: an effect that spawns a projectile carries one on its
        // base record, and EffectSetting::data.projectileBase is null for
        // every effect that spawns none.
        //
        // Three gates, all required:
        //   1. NOT Concentration — Flames/Sparks are streams. They do carry a
        //      projectile record, but a stream has no single shot to ride out
        //      and the cast state already covers it. (Concentration is
        //      excluded upstream too; kept here so this helper is honest
        //      standing alone.)
        //   2. Delivery is kAimed or kTargetLocation — the only two that
        //      travel. kSelf (Oakflesh, Healing, Bound Sword, Dragonhide),
        //      kTouch (Vampiric Drain, Flame Cloak) and kTargetActor never
        //      put anything between the player and the world.
        //   3. Some effect carries a projectileBase.
        //
        // Works for staff enchantments as well as spells — both are
        // MagicItems, and a Staff of Fireballs' enchantment carries the
        // fireball projectile exactly like the spell does.
        bool MagicItemLaunchesProjectile(const RE::MagicItem* a_item)
        {
            if (!a_item) return false;
            if (a_item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
                return false;
            const auto delivery = a_item->GetDelivery();
            if (delivery != RE::MagicSystem::Delivery::kAimed &&
                delivery != RE::MagicSystem::Delivery::kTargetLocation)
                return false;
            for (const auto* effect : a_item->effects) {
                if (effect && effect->baseEffect && effect->baseEffect->data.projectileBase)
                    return true;
            }
            return false;
        }

        // Same question, asked of a caster's live spell.
        bool CasterLaunchesProjectile(const RE::MagicCaster* a_caster)
        {
            return a_caster && MagicItemLaunchesProjectile(a_caster->currentSpell);
        }

        // Map a SpellItem's CastingType to our CastType enum. The engine has
        // no native "Ritual" type — every vanilla ritual spell is technically
        // kFireAndForget. We identify rituals via four combined signals on
        // the costliest effect:
        //   1. CastingType == kFireAndForget   (Lightning Storm-style
        //      concentration master spells stay Concentration)
        //   2. Delivery   == kSelf             (excludes Icy Spear, Flame
        //      Thrall, etc. which are kAimed)
        //   3. Min skill level >= 100          (master tier; excludes
        //      adept/expert wall spells)
        //   4. Effect area > 0                 (excludes self-buffs like
        //      Dragonhide which match the first three)
        // This catches Fire Storm, Bane of the Undead, Mass Paralysis,
        // Call to Arms, Mayhem, Hysteria, Harmony, etc.
        // kConstantEffect and kScroll fall through to None.
        CastType ResolveCastType(const RE::SpellItem* a_spell)
        {
            return ResolveMagicCastType(a_spell);
        }

        // Live per-hand cast detection. Walks the player's left and right
        // hand magic casters, classifies whatever each is currently casting
        // (via currentSpell), and returns the priority-resolved cast type
        // for the frame.
        //
        // Priority: Ritual > FireAndForget > Concentration > None
        //
        // Why polling instead of the SKSE kSpellCast event: the event only
        // fires on cast START. If you hold a Concentration spell in one hand
        // and a FAF interrupts it from the other, the cached cast type stays
        // FAF after the FAF ends until the next cast event — even though the
        // Concentration is still going. Polling currentSpell every frame
        // gives us the live picture and lets the cache revert to
        // Concentration when the FAF ends.
        //
        // kOther (shouts) and kInstant (scrolls) are intentionally excluded
        // — neither should drive the spell-cast camera state.
        struct HandCastInfo
        {
            bool        active = false;
            // Spell cast (SpellItem) — drives Magic state
            CastType    type   = CastType::None;
            MagicSchool school = MagicSchool::None;
            // Staff fire (EnchantmentItem) — drives Staves state
            CastType    staffType   = CastType::None;
            bool        staffConcentration = false;
            MagicSchool staffSchool = MagicSchool::None;
            // Camera-lockdown signal — true if any hand caster is in a
            // state where vanilla would suppress free-look.
            bool        locked = false;
            // Per-hand "actively casting an F&F (or ritual) spell" flags.
            // Distinct from the combined `type` field, which collapses
            // both hands into one priority-resolved cast classification.
            // The combined view hides per-hand release events when both
            // hands are casting F&F at once: the cachedCastType stays at
            // F&F because the other hand keeps it lit, and the standard
            // "cachedCastType went non-F&F" trigger never fires. These
            // per-hand flags let the linger trigger recognize a
            // dual-cast as two release events for session escalation.
            bool        leftFF  = false;
            bool        rightFF = false;
            // Per-hand "reached a firing state this frame" flags. True
            // when an F&F caster's state == kReady (held charged) or
            // kCasting (firing). Used to distinguish real casts from
            // tap-cancels: a tap that doesn't charge enough goes
            // kCharging → kNone without ever flipping these.
            bool        leftFiring  = false;
            bool        rightFiring = false;
            // True when any hand caster is actively casting a ward spell.
            // Routed by the Block resolver (ward casts win over base weapon
            // routing, but yield to non-ward casts and to bashes).
            bool        ward = false;
            // Hand(s) whose classification matches the winning (type,
            // school) pair — drives the per-hand magic overrides. Engine
            // dual-casting forces Both.
            CastingHand hand = CastingHand::None;
            // Raw "the engine says this is a dual cast" reading for THIS
            // frame, from either engine source. Published because both
            // sources are transient (see PollHandCaster's dual latch).
            bool        dualNow = false;
        };

        // Vanilla Lightning Storm (Skyrim.esm). Concentration master spell with
        // a unique windup during which vanilla locks the camera; free-look
        // resumes once the charge completes.
        constexpr RE::FormID kLightningStormFormID = 0x0001CB05;

        HandCastInfo ResolveActiveHandCast(RE::PlayerCharacter* a_player)
        {
            HandCastInfo result;
            if (!a_player) return result;

            constexpr std::array sources = {
                RE::MagicSystem::CastingSource::kLeftHand,
                RE::MagicSystem::CastingSource::kRightHand,
            };

            // Per-hand spell classification, kept alongside the combined
            // priority resolution so the winner can be attributed back to
            // the hand(s) that produced it after the walk.
            CastType    leftType = CastType::None,       rightType = CastType::None;
            MagicSchool leftSchool = MagicSchool::None,  rightSchool = MagicSchool::None;
            // Per-caster dual-cast flag (ActorMagicCaster::GetIsDualCasting,
            // literally `flags & 1` on the caster). See the attribution block
            // below for why this is read alongside Actor::IsDualCasting().
            bool        leftDual = false,                rightDual = false;

            for (auto src : sources) {
                auto* caster = a_player->GetMagicCaster(src);
                if (!caster) continue;
                // "Active" = anything except kNone (0). kReady (3) is the
                // held-charged state for staves and matters here too.
                if (caster->state.get() == RE::MagicCaster::State::kNone) continue;
                result.active = true;
                if (caster->GetIsDualCasting()) {
                    if (src == RE::MagicSystem::CastingSource::kLeftHand) leftDual = true;
                    else                                                  rightDual = true;
                }

                // Classify the magic currently in this caster.
                auto* magicItem = caster->currentSpell;
                if (!magicItem) continue;

                auto* spell = magicItem->As<RE::SpellItem>();
                if (spell) {
                    // Detect ward casts — orthogonal to type/school and used
                    // by the Block resolver. A ward is still classified
                    // through the usual type/school fields below; the ward
                    // flag just adds a route to Blocking when no higher-
                    // priority signal (attack, non-ward cast) is active.
                    if (IsWardSpell(spell)) {
                        result.ward = true;
                    }
                    // Spell cast — drives Magic camera state
                    const auto type       = ResolveCastType(spell);
                    const auto thisSchool = ResolveSpellSchool(spell);

                    if (src == RE::MagicSystem::CastingSource::kLeftHand) {
                        leftType = type;  leftSchool = thisSchool;
                    } else {
                        rightType = type; rightSchool = thisSchool;
                    }

                    if (type == CastType::FireAndForget || type == CastType::Ritual) {
                        const auto cs = caster->state.get();
                        const bool firing = (cs == RE::MagicCaster::State::kReady) ||
                                            (cs == RE::MagicCaster::State::kCasting);
                        if (src == RE::MagicSystem::CastingSource::kLeftHand) {
                            result.leftFF     = true;
                            result.leftFiring = firing;
                        } else if (src == RE::MagicSystem::CastingSource::kRightHand) {
                            result.rightFF     = true;
                            result.rightFiring = firing;
                        }
                    }

                    if (type == CastType::Ritual) {
                        result.type   = CastType::Ritual;
                        result.school = thisSchool;
                    } else if (type == CastType::FireAndForget && result.type != CastType::Ritual) {
                        result.type   = CastType::FireAndForget;
                        result.school = thisSchool;
                    } else if (type == CastType::Concentration && result.type == CastType::None) {
                        result.type   = CastType::Concentration;
                        result.school = thisSchool;
                    }

                    // Camera-lockdown rules:
                    //   - Ritual casts lock the camera for the entire windup
                    //     (vanilla locks free-look on master-tier AoE casts).
                    //   - Lightning Storm (kConcentration master) locks only
                    //     while the caster is in kCharging; once the windup
                    //     completes and the stream starts, vanilla releases
                    //     free-look.
                    const auto casterState = caster->state.get();
                    if (type == CastType::Ritual) {
                        result.locked = true;
                    } else if (spell->GetFormID() == kLightningStormFormID &&
                               casterState == RE::MagicCaster::State::kCharging) {
                        result.locked = true;
                    }
                } else {
                    // Not a SpellItem — likely an EnchantmentItem from a staff fire.
                    // Detect school and cast type from the enchantment.
                    const auto sType    = ResolveEnchantmentCastType(magicItem);
                    const auto sSchool  = ResolveEnchantmentSchool(magicItem);
                    if (sSchool != MagicSchool::None && PreferMagicCast(sType, result.staffType)) {
                        result.staffType   = sType;
                        result.staffConcentration = magicItem->GetCastingType() ==
                            RE::MagicSystem::CastingType::kConcentration;
                        result.staffSchool = sSchool;
                    }
                }
            }

            // Attribute the winning (type, school) back to the hand(s) that
            // produced it. Only hands whose own classification equals the
            // winner count — a left Flames stream under a right Firebolt
            // charge attributes the F&F winner to the RIGHT hand only.
            // Engine dual-casting (perk) forces Both: the engine may drive
            // the merged cast through a single caster, so the mask alone
            // can't be trusted to read it as two hands.
            if (result.type != CastType::None) {
                const bool leftMatch  = (leftType == result.type && leftSchool == result.school);
                const bool rightMatch = (rightType == result.type && rightSchool == result.school);
                // Dual-cast detection reads BOTH engine sources, because
                // NEITHER survives the cast. Measured with [TLHAND]:
                //   dual=atrue/ltrue/rfalse   at 11:28:21.824
                //   dual=afalse/ltrue/rfalse  at 11:28:21.867  (+43 ms)
                //   dual=afalse/lfalse/rfalse at 11:28:22.038  (+214 ms)
                // Actor::IsDualCasting() reads HighProcessData::isDualCasting
                // and clears within a few frames of the cast being initiated;
                // ActorMagicCaster::GetIsDualCasting() (`flags & 1` on the
                // caster) lasts a few frames longer and then clears too. Once
                // both are gone the merged concentration stream is running
                // through a single caster (the left, in vanilla), so
                // `leftMatch && rightMatch` no longer holds either and the
                // attribution collapses to Left mid-cast.
                //
                // So this frame's reading is only ever an EDGE. The latch that
                // turns it into a stable answer for the whole cast lives in
                // PollHandCaster — see the dual latch there.
                //
                // Per-caster flag only trusted on a hand whose own spell
                // matches the winning (type, school), so an unrelated dual cast
                // in the other hand can't claim the attribution.
                const bool dualCaster = (leftMatch && leftDual) || (rightMatch && rightDual);
                result.dualNow = a_player->IsDualCasting() || dualCaster;
                if (result.dualNow || (leftMatch && rightMatch)) {
                    result.hand = CastingHand::Both;
                } else if (leftMatch) {
                    result.hand = CastingHand::Left;
                } else if (rightMatch) {
                    result.hand = CastingHand::Right;
                }
            }

            return result;
        }

    }

    StateResolver& StateResolver::GetSingleton()
    {
        static StateResolver instance;
        return instance;
    }

    void StateResolver::Register()
    {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("StateResolver: ScriptEventSourceHolder unavailable, race events will not fire");
            return;
        }
        holder->AddEventSink<RE::TESSwitchRaceCompleteEvent>(this);
        spdlog::info("StateResolver: registered for TESSwitchRaceCompleteEvent");

        holder->AddEventSink<RE::TESEquipEvent>(this);
        spdlog::info("StateResolver: registered for TESEquipEvent");

        if (auto* actionSource = SKSE::GetActionEventSource()) {
            actionSource->AddEventSink<SKSE::ActionEvent>(this);
            spdlog::info("StateResolver: registered for SKSE::ActionEvent");
        } else {
            spdlog::warn("StateResolver: SKSE action event source unavailable");
        }

        // Seed initial state from current player race/loadout in case we load
        // mid-transformation or with an already-equipped weapon.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto* race = player->GetRace();
            if (IsWerewolfRace(race)) {
                isWerewolf = true;
                state = CameraState::Werewolf;
                spdlog::info("StateResolver: initial race is Werewolf");
            } else if (IsVampireLordRace(race)) {
                isVampireLord = true;
                // Don't seed `state` here — ResolveVampireLord will compute
                // the correct sub-state on the first Update() pass.
                spdlog::info("StateResolver: initial race is VampireLord");
            }
            isMeleeEquipped     = PlayerHasMeleeEquipped(player);
            isBowEquipped       = PlayerHasBowEquipped(player);
            isCrossbowEquipped  = PlayerHasCrossbowEquipped(player);
            isStaffEquipped     = PlayerHasStaffEquipped(player);
            isSpellEquipped     = PlayerHasSpellEquipped(player);
            isShieldEquipped    = PlayerHasShieldEquipped(player);
            isOneHandedEquipped = PlayerHasOneHandedEquipped(player);
            isTwoHandedEquipped = PlayerHasTwoHandedEquipped(player);
            spellSchool         = DetectPlayerSpellSchool(player);
            spdlog::info("StateResolver: initial melee={}, bow={}, xbow={}, staff={}, spell={} school={}, shield={}, 1H={}, 2H={}",
                         isMeleeEquipped, isBowEquipped, isCrossbowEquipped, isStaffEquipped, isSpellEquipped,
                         SchoolName(spellSchool), isShieldEquipped, isOneHandedEquipped, isTwoHandedEquipped);
        }
    }

    RE::BSEventNotifyControl StateResolver::ProcessEvent(
        const RE::TESSwitchRaceCompleteEvent* a_event,
        RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*)
    {
        if (!a_event || !a_event->subject) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (a_event->subject.get() != player) {
            // NPC noise retains the previous form so entering and reverting
            // use the corresponding transformation settings.
            if (auto* npc = a_event->subject.get()->As<RE::Actor>()) {
                CameraNoiseController::NotifyNpcRaceChange(npc);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* race = player->GetRace();
        const bool nowWerewolf    = IsWerewolfRace(race);
        const bool nowVampireLord = IsVampireLordRace(race);

        if (nowWerewolf != isWerewolf) {
            isWerewolf = nowWerewolf;
            if (nowWerewolf) {
                spdlog::debug("StateResolver: Werewolf transformation detected");
            } else {
                spdlog::debug("StateResolver: Werewolf revert detected");
            }
        }
        if (nowVampireLord != isVampireLord) {
            isVampireLord = nowVampireLord;
            if (nowVampireLord) {
                spdlog::debug("StateResolver: VampireLord transformation detected");
            } else {
                spdlog::debug("StateResolver: VampireLord revert detected");
            }
        }

        // Mark dirty so the next Update() walks the priority stack. The
        // resolver no longer writes the public state field directly here —
        // resolution is centralised.
        stateChanged = true;

        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl StateResolver::ProcessEvent(
        const RE::TESEquipEvent* a_event,
        RE::BSTEventSource<RE::TESEquipEvent>*)
    {
        if (!a_event || !a_event->actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (a_event->actor.get() != player) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Recompute from the player's current loadout — cheaper than decoding
        // which slot was affected, and handles the "unequip right → unarmed"
        // case that TESEquipEvent alone wouldn't cover.
        const bool nowMelee    = PlayerHasMeleeEquipped(player);
        const bool nowBow      = PlayerHasBowEquipped(player);
        const bool nowCrossbow = PlayerHasCrossbowEquipped(player);
        const bool nowStaff    = PlayerHasStaffEquipped(player);
        const bool nowSpell    = PlayerHasSpellEquipped(player);
        const bool nowShield   = PlayerHasShieldEquipped(player);
        const bool nowOneH     = PlayerHasOneHandedEquipped(player);
        const bool nowTwoH     = PlayerHasTwoHandedEquipped(player);
        bool dirty = false;
        if (nowMelee != isMeleeEquipped) {
            isMeleeEquipped = nowMelee;
            spdlog::debug("StateResolver: melee equipped = {}", nowMelee);
            dirty = true;
        }
        if (nowBow != isBowEquipped) {
            isBowEquipped = nowBow;
            spdlog::debug("StateResolver: bow equipped = {}", nowBow);
            dirty = true;
        }
        if (nowCrossbow != isCrossbowEquipped) {
            isCrossbowEquipped = nowCrossbow;
            spdlog::debug("StateResolver: crossbow equipped = {}", nowCrossbow);
            dirty = true;
        }
        if (nowStaff != isStaffEquipped) {
            isStaffEquipped = nowStaff;
            spdlog::debug("StateResolver: staff equipped = {}", nowStaff);
            dirty = true;
        }
        if (nowSpell != isSpellEquipped) {
            isSpellEquipped = nowSpell;
            spdlog::debug("StateResolver: spell equipped = {}", nowSpell);
            dirty = true;
        }
        if (nowShield != isShieldEquipped) {
            isShieldEquipped = nowShield;
            spdlog::debug("StateResolver: shield equipped = {}", nowShield);
            dirty = true;
        }
        if (nowOneH != isOneHandedEquipped) {
            isOneHandedEquipped = nowOneH;
            spdlog::debug("StateResolver: 1H melee equipped = {}", nowOneH);
            dirty = true;
        }
        if (nowTwoH != isTwoHandedEquipped) {
            isTwoHandedEquipped = nowTwoH;
            spdlog::debug("StateResolver: 2H melee equipped = {}", nowTwoH);
            dirty = true;
        }
        const auto nowSchool = DetectPlayerSpellSchool(player);
        if (nowSchool != spellSchool) {
            spellSchool = nowSchool;
            spdlog::debug("StateResolver: spell school = {}", SchoolName(nowSchool));
            dirty = true;
        }
        if (dirty) stateChanged = true;
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl StateResolver::ProcessEvent(
        const SKSE::ActionEvent* a_event,
        RE::BSTEventSource<SKSE::ActionEvent>*)
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;

        // Shout detection — kVoiceCast fires when the player begins a shout
        // and ought to carry the TESShout form as sourceForm. Extra logging
        // below traces every branch since we're debugging why detection
        // isn't firing.
        if (a_event->type.get() == SKSE::ActionEvent::Type::kVoiceCast) {
            const bool actorIsPlayer = (a_event->actor == RE::PlayerCharacter::GetSingleton());
            if (!actorIsPlayer) {
                spdlog::debug("StateResolver: kVoiceCast received but actor is not player (skipping)");
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!a_event->sourceForm) {
                spdlog::debug("StateResolver: kVoiceCast received but sourceForm was null");
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* shout = a_event->sourceForm->As<RE::TESShout>();
            if (!shout) {
                spdlog::debug("StateResolver: kVoiceCast sourceForm ({:08X}) is not a TESShout (formType={})",
                             a_event->sourceForm->formID,
                             static_cast<int>(a_event->sourceForm->formType.get()));
                return RE::BSEventNotifyControl::kContinue;
            }

            auto id = ShoutRegistry::GetSingleton().Find(shout);
            if (id) {
                spdlog::debug("StateResolver: kVoiceCast -> shout id={} ({:08X})",
                             static_cast<int>(*id), shout->formID);
            } else {
                const char* name = shout->GetFullName();
                spdlog::debug("StateResolver: kVoiceCast -> shout not in registry (form {:08X} name='{}')",
                             shout->formID, name ? name : "?");
            }
            if (id != activeShoutId) {
                activeShoutId = id;
                stateChanged = true;
            }
            // Mod-added shout identity — captured only on a registry miss
            // so the Shout weapon-binding layer can match this cast by
            // form. Known shouts clear it (mutually exclusive).
            activeModShoutFormID = 0;
            activeModShoutPlugin.clear();
            if (!id) {
                if (auto* file = shout->GetFile(0)) {
                    activeModShoutFormID = shout->GetLocalFormID();
                    activeModShoutPlugin = file->fileName;
                }
            }

            // Werewolf howls (Howl of Terror / Pack / Savage Feeding) fire
            // kVoiceCast on a TESShout but the werewolf behavior graph
            // doesn't emit BeginCastVoice / shoutStop tags. We can't ride
            // the regular animation-tag path so isShouting has to come
            // from kVoiceCast.
            //
            // The howl's kVoiceCast fires at the start of the wind-up
            // animation. Setting isShouting immediately runs the camera
            // transition ahead of the visible cue, but too long a delay drops
            // it entirely behind the roar's emphasis. 150ms was still early by
            // about a tenth of a second by eye; 300ms lands the transition ON
            // the roar rather than leading it.
            constexpr int kRoarStartDelayMs = 500;
            if (isWerewolf) {
                werewolfRoarStartTime = CameraEffectClock::Now() +
                                        std::chrono::milliseconds(kRoarStartDelayMs);
                werewolfRoarScheduled = true;
                spdlog::debug("StateResolver: werewolf howl/roar scheduled (form {:08X}, +{}ms)",
                             shout->formID, kRoarStartDelayMs);
            }
        }

        // kVoiceFire — the moment the shout actually GOES OFF, as opposed to
        // kVoiceCast which opens the wind-up. This matters because the
        // animation-graph release tag (Voice_SpellFire_Event) is emitted by
        // vanilla behaviour but NOT by every framework — this load order's
        // graph has never once emitted it, which left the noise envelopes
        // with no release edge and no way to tell a 1-word tap from a
        // 3-word hold. kVoiceFire comes from SKSE, not the graph, so it is
        // there regardless of what behaviour mods are installed.
        if (a_event->type.get() == SKSE::ActionEvent::Type::kVoiceFire &&
            a_event->actor == RE::PlayerCharacter::GetSingleton()) {
            const auto now = CameraEffectClock::Now();
            // Only stamp a fire time that belongs to the shout currently
            // being tracked, so a stale event can't back-date the envelope.
            if (shoutFireTime < shoutStartTime) {
                shoutFireTime = now;
            }
            CaptureShoutWordCount("kVoiceFire");
        }

        // Projectile launches, for the Attack Lag gate. These come from SKSE
        // rather than the animation graph on purpose: the graph tags
        // (MLh/MRh_SpellFire_Event, arrowRelease/arrowDetach) are vanilla
        // vocabulary and usually survive a behaviour patch, but "usually" is
        // not a compatibility guarantee, and a graph that renamed them would
        // silently disable the feature with no way for the user to tell.
        // kSpellFire / kBowRelease fire from the engine's own action
        // dispatch, so they are there whatever animations are installed. The
        // graph tags stay wired up as well — either source arming the latch
        // is correct, since it is a latch and not a counter.
        if (a_event->actor == RE::PlayerCharacter::GetSingleton()) {
            switch (a_event->type.get()) {
            case SKSE::ActionEvent::Type::kSpellFire: {
                // sourceForm IS the spell that just fired, so the projectile
                // question gets its most precise possible answer here: no
                // guessing which hand, no reading a caster that may already
                // have moved on. Oakflesh and friends stop here.
                const auto* item = a_event->sourceForm
                                       ? a_event->sourceForm->As<RE::MagicItem>()
                                       : nullptr;
                if (MagicItemLaunchesProjectile(item)) {
                    projectileMagicCounter.fetch_add(1, std::memory_order_relaxed);
                } else if (item) {
                    spdlog::debug("StateResolver: spell fire '{}' launches nothing "
                                  "- no attack lag",
                                  a_event->sourceForm->GetName());
                }
                break;
            }
            case SKSE::ActionEvent::Type::kBowRelease:
                projectileArrowCounter.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    void StateResolver::CaptureShoutWordCount(const char* a_via)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto& rt = player->GetActorRuntimeData();
        auto* process = rt.currentProcess;
        auto* high = process ? process->high : nullptr;
        if (!high) return;

        int words = 0;
        switch (high->currentShoutVariation) {
        case RE::TESShout::VariationIDs::kOne:   words = 1; break;
        case RE::TESShout::VariationIDs::kTwo:   words = 2; break;
        case RE::TESShout::VariationIDs::kThree: words = 3; break;
        default: break;   // kNone — the engine hasn't committed a variation yet
        }
        if (words <= 0 || words == shoutWordCount) return;

        shoutWordCount = words;
        spdlog::debug("StateResolver: shout word count = {} (via {})", words, a_via ? a_via : "?");
    }

    void StateResolver::PollShoutWords(RE::PlayerCharacter* a_player)
    {
        // Safety net around the kVoiceFire capture above. The engine may
        // publish currentShoutVariation a frame after the action event, and
        // a setup where kVoiceFire never arrives would otherwise never learn
        // the word count at all — so keep sampling for as long as the shout
        // is live and latch the first real value.
        if (!a_player) return;

        if (!isShouting) {
            // The engine leaves the last variation behind between shouts.
            // Clearing on the way out stops a stale count from colouring the
            // NEXT shout's wind-up before its own count is known.
            shoutWordCount = 0;
            return;
        }
        if (shoutWordCount == 0) {
            CaptureShoutWordCount("poll");
        }
    }

    void StateResolver::RecordGraphEventForFreeze(std::string_view a_tag)
    {
        // PER-FRAME POLLING DUMMIES NEVER ENTER THE RING.
        //
        // The ring is 24 slots and exists for ONE job: on an "ACTOR STUCK"
        // freeze verdict, show the animation state that was entered and never
        // exited. The 2026-09-07 21:23 capture — the cleanest freeze ever
        // caught, three episodes, every DDC block clear — printed this:
        //
        //   last 24 graph events: SCAR_UpdateDummy -> SCAR_UpdateDummy -> ...
        //
        // all twenty-four. SCAR (Skyrim Combos AI Revolution) emits
        // SCAR_UpdateDummy every frame as a poll, so it had flushed the entire
        // history before the freeze even started, and the one dump that
        // mattered carried no information at all.
        //
        // A per-frame event is noise by definition here: 24 frames of history
        // is a third of a second and nothing diagnostic happens in it. Drop
        // them at the writer rather than filtering at the reader, so the 24
        // slots hold 24 REAL transitions however long ago they happened.
        //
        // Exact matches only, and a short deny-list rather than a heuristic:
        // an event this filter hides can never be recovered from a log, so it
        // has to be something known to fire unconditionally every frame.
        if (a_tag == "SCAR_UpdateDummy") return;

        std::unique_lock lock(graphRingMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            graphRingDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& dst = graphRing[graphRingN % kGraphRing];
        const std::size_t len = (std::min)(a_tag.size(), kGraphTagLen - 1);
        std::memcpy(dst.tag.data(), a_tag.data(), len);
        dst.tag[len] = '\0';
        dst.time = std::chrono::steady_clock::now();
        ++graphRingN;
    }

    std::size_t StateResolver::GetRecentGraphEvents(
        std::array<std::string, kGraphRing>& a_out, std::size_t& a_dropped) const
    {
        std::array<FreezeGraphEvent, kGraphRing> snapshot{};
        std::size_t have = 0;
        {
            std::lock_guard lock(graphRingMutex);
            have = (std::min)(graphRingN, kGraphRing);
            const std::size_t first = graphRingN - have;
            for (std::size_t index = 0; index < have; ++index) {
                snapshot[index] = graphRing[(first + index) % kGraphRing];
            }
        }
        a_dropped = graphRingDropped.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < have; ++index) {
            const auto& entry = snapshot[index];
            const float age = std::chrono::duration<float>(now - entry.time).count();
            a_out[index] = fmt::format("{} ({:.3f}s ago)", entry.tag.data(), age);
        }
        return have;
    }

    RE::BSEventNotifyControl StateResolver::ProcessEvent(
        const RE::BSAnimationGraphEvent* a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;
        // Defensive: we only ever subscribe to the player's graph(s), but the
        // holder is exposed on the event so verify in case other actors share
        // a graph somehow.
        if (a_event->holder != RE::PlayerCharacter::GetSingleton()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        const std::string_view tag(a_event->tag.c_str() ? a_event->tag.c_str() : "");
        if (tag.empty()) return RE::BSEventNotifyControl::kContinue;

        // FREEZE FORENSICS. The 2026-08-23 log caught a real episode where the
        // engine HAD the full movement input (eng=1.03) and the actor still
        // would not move, with asMove=0x0 — i.e. the animation graph was not
        // letting go. The only way to see that after the fact is to know what
        // the graph was doing just before, so every event the player's graph
        // emits lands in a small ring the freeze watchdog dumps on onset.
        // Names only, no allocation on the hot path beyond the copy.
        RecordGraphEventForFreeze(tag);

        // Vanilla shout animation events seen in the player's behavior graph
        // (confirmed via diagnostic logging):
        //   BeginCastVoice         — fires at shout charge start (~aligns with
        //                            MagicCaster::kOther entering kCharging)
        //   Voice_SpellFire_Event  — fires at the release moment
        //   shoutStop              — fires when the follow-through animation
        //                            completes, ~1s after the caster returns
        //                            to idle. This is the "true" end of the
        //                            shout for camera purposes.
        // Match case-insensitively. Any other tag containing "shout" or
        // "voice" gets logged as a diagnostic so we can adjust if a mod uses
        // different naming.
        // --- Shout detection ---
        // Vanilla emits "BeginCastVoice"; behaviour frameworks (the player's
        // graph here fires "SBF_ShoutStart" / "SBF_ShoutStop") rename the
        // bracket. Match the generic "shoutstart" substring too so a renamed
        // start tag still flips isShouting. ("SBF_ShoutStop" already matches
        // the existing "shoutstop" substring, so the stop side needs nothing.)
        const bool isShoutStart = ContainsCI(tag, "begincastvoice") ||
                                  ContainsCI(tag, "shoutstart");
        const bool isShoutStop  = ContainsCI(tag, "shoutstop");
        if (isShoutStart) {
            // Always update the start timestamp so the 1p shouts noise
            // envelope can detect back-to-back shouts (where isShouting
            // never falls because the linger is cancelled by the new
            // shout) and restart its build-from-zero curve.
            shoutStartTime = CameraEffectClock::Now();
            shoutFireTime  = {};  // cleared until Voice_SpellFire_Event / kVoiceFire
            shoutPausedAccum = {};  // fresh shout, fresh safety window (see PollShoutLinger)
            shoutPauseTick   = {};
            shoutWordCount = 0;   // unknown until the engine commits a variation
            // Fresh shout, fresh Lag: CameraController republishes the
            // resolved entry's value on the next 3p frame; a shout cast
            // without one (pure first person) holds no stale lag.
            activeShoutLagSec = 0.0f;
            if (!isShouting) {
                isShouting = true;
                spdlog::debug("StateResolver: shout started (tag={})", tag);
                stateChanged = true;
            }
            // If the linger was active (previous shout just ended), cancel
            // it — this shout takes over. isShouting is already true so
            // no additional transition fires.
            shoutLingering  = false;
            shoutLagHolding = false;   // a new shout is live, not lag-held

            // Shout-id resolution moved to the SKSE ActionEvent handler
            // (kVoiceCast fires with sourceForm pointing straight at the
            // TESShout — reliable, unlike GetCurrentShout which returned
            // null on most casts because high->currentShout isn't set yet
            // when BeginCastVoice fires).
        } else if (isShoutStop) {
            if (isShouting && !shoutLingering) {
                // Don't flip isShouting yet — start the exit linger so
                // the Shout sub-state profile gets a cinematic tail. The
                // expiry check in PollShoutLinger commits the end.
                shoutEndTime   = CameraEffectClock::Now();
                shoutLingering = true;
                spdlog::debug("StateResolver: shout stop received — linger started (tag={})", tag);
            }
        }
        // Voice_SpellFire_Event fires at the actual release moment between
        // BeginCastVoice and shoutStop. We have no use for it (it'd just
        // duplicate the start signal) so it's intentionally a no-op. Any
        // *other* unrecognised shout/voice tag still gets logged so we'd
        // notice if a mod introduced new naming.
        else if (ContainsCI(tag, "voice_spellfire")) {
            // Voice line fires here — the moment the actual "FUS" plays.
            // Recorded so the 1p shouts noise envelope can taper down
            // approaching this moment regardless of 1/2/3-word hold time.
            shoutFireTime = CameraEffectClock::Now();
        } else if (ContainsCI(tag, "shout") || ContainsCI(tag, "voice")) {
            spdlog::warn("StateResolver: unhandled shout/voice tag={}", tag);
        }

        // --- Skyrim's Paraglider (mod support) ---
        // Detection is the paraglide ANIMATIONS' own annotations, not the
        // plugin's NotifyAnimationGraph("StartPara"/"EndPara") inputs — those
        // are events sent INTO the graph and were LOG-PROVEN never to surface
        // in this sink (21:33 log: three glides, zero StartPara/EndPara). What
        // the graph EMITS during a glide, from the same log:
        //   deploy: 'SoundPlay.ParaglideEquip', 'Parachute', 'Para_EquipOut'
        //           (the SoundPlay tag did NOT fire on every deploy — glide 3
        //           announced itself only via Para_EquipOut — so all three
        //           latch, idempotently)
        //   end:    'SoundPlay.ParaglideUnequip', 'Para_UnequipOut', and
        //           'ParaDummy' (the mid-air close fired ONLY ParaDummy)
        // The grounded/swimming net in PollParaglide covers any exit that
        // emits nothing at all. Start latches only while genuinely airborne —
        // a glider cannot deploy on the ground, so a stray tag can't stick
        // the camera in glide framing.
        // NOTE the substring traps: "paraglideunequip" CONTAINS neither
        // "paraglideequip" (the "un" splits it) nor vice versa, and
        // "para_unequipout" does not contain "para_equipout" — checked.
        const bool paraStart = ContainsCI(tag, "paraglideequip") ||
                               ContainsCI(tag, "parachute")      ||
                               ContainsCI(tag, "para_equipout");
        const bool paraEnd   = ContainsCI(tag, "paraglideunequip") ||
                               ContainsCI(tag, "para_unequipout")  ||
                               ContainsCI(tag, "paradummy");
        if (paraStart && !isParagliding) {
            bool airborne = false;
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                airborne = p->IsInMidair();
                if (!airborne) {
                    if (auto* cc = p->GetCharController()) {
                        const auto st = cc->context.currentState;
                        airborne = st == RE::hkpCharacterStateType::kInAir ||
                                   st == RE::hkpCharacterStateType::kJumping;
                    }
                }
            }
            if (airborne) {
                isParagliding = true;
                stateChanged  = true;
                spdlog::debug("StateResolver: paraglide started (tag={})", tag);
            }
        } else if (paraEnd && isParagliding) {
            isParagliding = false;
            stateChanged  = true;
            spdlog::debug("StateResolver: paraglide ended (tag={})", tag);
        }

        // --- Attack animation bracket ---
        // Power attack CLASSIFICATION (normal vs power) still comes from the
        // AIProcess attack data in PollAttack — animation tags vary too much
        // to trust for that. What the tags ARE good for is the thing the
        // graph variable is bad at: the true extent of the animation.
        //
        // COMPATIBILITY CONTRACT. This has to hold up on a graph we have
        // never seen, so it is built to earn its authority and to give it
        // back. Three rules:
        //   1. Substrings, not a name table — one match covers every
        //      convention that reuses the vanilla vocabulary, which every
        //      Nemesis/Pandora patch (MCO, BFCO, SkySA, ADXP, SBF …) does,
        //      because they ANNOTATE the vanilla graph rather than replace
        //      its events.
        //        "attackstart"    -> vanilla attackStart, SBF_NormalAttackStart,
        //                            SBF_PowerAttackStart, Collision_AttackStart
        //        "powerstart"     -> vanilla attackPowerStartInPlace / Forward /
        //                            Backward / Left / Right
        //        "attackinitiate" -> MCO_AttackInitiate, MCO_PowerAttackInitiate,
        //                            MCO_SprintPowerAttackInitiate
        //        "attackstop"     -> vanilla attackStop, SBF_NormalAttackStop,
        //                            SBF_PowerAttackStop
        //   2. Nothing is trusted until a full open→close cycle has been
        //      observed (attackAnimBracketProven). A graph that only ever
        //      emits one half of the pair changes nothing.
        //   3. If the watchdog in PollAttack ever has to force a bracket
        //      shut, brackets are abandoned for good on this graph
        //      (attackAnimDistrust) and we fall back to the old graph-var +
        //      linger path. Worst case on an unknown graph is one overlong
        //      attack state, once — not a permanent regression.
        //
        // Deliberately NOT matched as an end: MCO_WinClose /
        // MCO_TransitionClose / MCO_Recovery / SkySA_AttackWinEnd. Those mark
        // the end of the attack WINDOW inside a combo, and a chained swing
        // fires them between hits — treating them as the animation end would
        // drop the Attack sub-state mid-combo. Rule 3 is what covers a graph
        // whose only close tag is one of those.
        if (ContainsCI(tag, "attackstart") || ContainsCI(tag, "attackinitiate") ||
            ContainsCI(tag, "powerstart"))
        {
            // NOT while the voice is active: shout/roar animations run
            // through the attack graph on some rigs (the werewolf howl emits
            // a power-start), and a weapon swing genuinely can't begin
            // mid-shout — so a start tag inside the voice window is the
            // shout's own animation, not an attack. Without this the roar
            // opened an attack bracket, its shout entry fell back onto the
            // ATTACK profile, and the post-roar ease read as a zoom snap.
            if (isShouting) {
                static bool sLoggedShoutSuppress = false;
                if (!sLoggedShoutSuppress) {
                    sLoggedShoutSuppress = true;
                    spdlog::debug("StateResolver: attack-bracket open suppressed during voice (tag={})", tag);
                }
            } else {
                attackAnimSawStart.store(true, std::memory_order_relaxed);
                attackAnimBracketOpen.store(true, std::memory_order_relaxed);
                attackAnimStartCounter.fetch_add(1, std::memory_order_relaxed);
                // The tag says which KIND of swing this is. "power" covers
                // SBF_PowerAttackStart, MCO_PowerAttackInitiate,
                // MCO_SprintPowerAttackInitiate and vanilla
                // attackPowerStart*; anything else is a normal swing.
                attackAnimPowerHint.store(ContainsCI(tag, "power"),
                                          std::memory_order_relaxed);
                attackAnimPowerCounter.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (ContainsCI(tag, "attackstop")) {
            // Only a stop that actually CLOSES something proves the pair.
            if (attackAnimBracketOpen.exchange(false, std::memory_order_relaxed) &&
                attackAnimSawStart.load(std::memory_order_relaxed))
            {
                attackAnimBracketProven.store(true, std::memory_order_relaxed);
            }
        }

        // --- Projectile launch counters (Attack Lag gating) ---
        // "spellfire" is substring-matched so a transformed or modded graph
        // participates without a name table; "voice" is excluded because
        // Voice_SpellFire_Event is the shout release, not a hand cast.
        if (ContainsCI(tag, "spellfire") && !ContainsCI(tag, "voice")) {
            // Projectile-only, same rule as the SKSE path. The tag NAMES the
            // hand (MRh_/MLh_SpellFire_Event), so ask that hand's caster
            // rather than "does either hand hold a projectile spell" — with
            // Firebolt in the off hand, the looser test would license a lag
            // for an Oakflesh cast and put us right back where we started.
            // Unlabelled tags (a modded graph that dropped the prefix) fall
            // back to any caster, which is still strictly better than the
            // old unconditional bump.
            using Src = RE::MagicSystem::CastingSource;
            bool launches = false;
            if (auto* ply = RE::PlayerCharacter::GetSingleton()) {
                const bool rh = ContainsCI(tag, "mrh");
                const bool lh = ContainsCI(tag, "mlh");
                if (rh || lh) {
                    launches = CasterLaunchesProjectile(
                        ply->GetMagicCaster(rh ? Src::kRightHand : Src::kLeftHand));
                } else {
                    for (auto src : { Src::kRightHand, Src::kLeftHand, Src::kInstant }) {
                        if (CasterLaunchesProjectile(ply->GetMagicCaster(src))) {
                            launches = true;
                            break;
                        }
                    }
                }
            }
            if (launches) projectileMagicCounter.fetch_add(1, std::memory_order_relaxed);
        }
        // Both spellings: vanilla behaviour emits arrowRelease when the
        // string releases and arrowDetach when the arrow actually leaves.
        // Graphs vary on which they annotate — this one only ever emits
        // arrowDetach, so matching arrowRelease alone finds nothing.
        if (ContainsCI(tag, "arrowrelease") || ContainsCI(tag, "arrowdetach")) {
            projectileArrowCounter.fetch_add(1, std::memory_order_relaxed);
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    void StateResolver::EnsureAnimGraphSubscription(RE::PlayerCharacter* a_player)
    {
        if (!a_player) return;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!a_player->GetAnimationGraphManager(manager) || !manager) return;

        // Re-subscribe whenever the manager pointer changes rather than
        // latching once at load. The original latch meant that after a
        // transformation OR a behaviour-framework graph reload swapped the
        // player's manager, our sink dangled on the dead manager and shout
        // tags (BeginCastVoice / shoutStop / SBF_ShoutStart…) silently
        // stopped arriving — isShouting never flipped, so the Quick Tune
        // Shout box and first-person shout camera noise never engaged. The
        // Camera Noise graph sink already does exactly this re-subscribe,
        // which is why it kept logging shout events while we went deaf.
        void* curMgr = static_cast<void*>(manager.get());
        if (curMgr == lastAnimGraphMgr) return;

        // A new manager means a new animation set — werewolf, Vampire Lord, a
        // behaviour-framework reload. Whatever the previous graph taught us
        // about attack bracketing does not transfer: its tags may be absent
        // here, and a graph we distrusted deserves a clean hearing. Re-learn
        // from scratch (costs at most one attack on the new graph, during
        // which the plain IsAttacking + linger path is in charge).
        attackAnimBracketOpen.store(false, std::memory_order_relaxed);
        attackAnimSawStart.store(false, std::memory_order_relaxed);
        attackAnimBracketProven.store(false, std::memory_order_relaxed);
        attackAnimDistrust.store(false, std::memory_order_relaxed);
        attackAnimOpenObserved = false;

        // The player's manager typically has one main graph but the
        // BSTSmallArray can hold several (e.g. werewolf swap brings in extras).
        // Subscribe to all of them; the BShkbAnimationGraph base inherits
        // BSTEventSource<BSAnimationGraphEvent> so AddEventSink is on the
        // graph itself. AddEventSink de-dupes, so re-adding to a graph we're
        // already on is a harmless no-op.
        bool subscribedAny = false;
        for (auto& graphPtr : manager->graphs) {
            if (auto* graph = graphPtr.get()) {
                graph->AddEventSink<RE::BSAnimationGraphEvent>(this);
                subscribedAny = true;
            }
        }
        if (subscribedAny) {
            lastAnimGraphMgr = curMgr;
            spdlog::info("StateResolver: (re)subscribed to player BSAnimationGraphEvent ({} graph(s), mgr=0x{:x})",
                         manager->graphs.size(), reinterpret_cast<std::uintptr_t>(curMgr));
        }
    }

    void StateResolver::Update(RE::PlayerCharacter* a_player)
    {
        CameraEffectClock::Sync();
        if (CameraEffectClock::IsPaused()) {
            RequestMenuStateHold();
            UpdateMenuStateHold();
            return;
        }
        // ===== Per-frame polls =====
        // These run unconditionally so the cached state stays current; each
        // sets stateChanged on transition. Polls have no side effects beyond
        // updating cached members and the dirty flag.
        PollDragonRiding();
        // Bow zoom — one bool read off PlayerCamera, no hook and no perk
        // lookup. Cheap enough to do unconditionally; the pickers only
        // consult it in the Bow / Crossbow branches.
        {
            auto* pcZoom = RE::PlayerCamera::GetSingleton();
            const bool nowZoom = pcZoom && pcZoom->bowZoomedIn;
            if (nowZoom != isBowZoomed) {
                isBowZoomed  = nowZoom;
                stateChanged = true;
                spdlog::debug("StateResolver: bow zoom {}", nowZoom ? "engaged" : "released");
            }
        }
        // Mounted melee swing side. Latched on the swing's first frame; see
        // GetMountAttackSide for why the signal is freeRotation.x and not the
        // player-vs-horse yaw (that reads 0.0 always — the rider's body angle
        // is locked to the horse).
        {
            auto* pcSide = RE::PlayerCamera::GetSingleton();
            bool swinging = false;
            if (a_player && isHorseback && isWeaponDrawn && isMeleeEquipped) {
                if (auto* asSide = a_player->AsActorState()) {
                    swinging = asSide->actorState1.meleeAttackState !=
                               RE::ATTACK_STATE_ENUM::kNone;
                }
            }
            // RE-LATCH ON A NEW SWING, NOT ONLY ON A NEW ATTACK SESSION.
            //
            // The first version cleared the latch only when meleeAttackState
            // fell back to kNone. In a CHAIN of attacks it never does — the
            // state walks kDraw -> kSwing -> kHit -> kNextAttack -> kSwing
            // without passing through kNone — so the side latched by the first
            // swing held for the whole chain. User: "after i use a right
            // attack, it won't switch to left if i do left attacks until i
            // stop attacking and begin again with a left attack."
            //
            // The attack RECORD changes per swing ('attackStart_MC_1HMLeft' vs
            // '..._1HMRight'), so keying the latch on the event name re-reads
            // the side for every swing while still holding it steady THROUGH
            // each one — which is the whole point of latching.
            static std::string sLatchedAtkEvent;
            if (!swinging) {
                mountAttackSide = MountAttackSide::None;
                sLatchedAtkEvent.clear();
            } else if (a_player) {
                const char* curEvPtr = nullptr;
                if (auto* procEv = a_player->GetActorRuntimeData().currentProcess) {
                    if (auto* highEv = procEv->high) {
                        if (auto atkEv = highEv->attackData) curEvPtr = atkEv->event.c_str();
                    }
                }
                const std::string_view curEv(curEvPtr ? curEvPtr : "");
                if (!curEv.empty() && curEv != sLatchedAtkEvent) {
                    sLatchedAtkEvent.assign(curEv);
                    mountAttackSide = MountAttackSide::None;   // force the re-read below
                }
            }

            if (!swinging) {
                // handled above
            } else if (mountAttackSide == MountAttackSide::None && a_player) {
                // First frame of this swing — decide once and hold.
                //
                // THE SIGNAL IS THE ATTACK HAND, from the engine's own attack
                // data. User: "on horseback with a two handed weapon, pressing
                // the right trigger swings the weapon on the right side and
                // the left trigger the left side." So the side is chosen by
                // WHICH ATTACK BUTTON was pressed, not by where the camera is
                // pointing — which is why both aim-based probes failed:
                // player-vs-horse yaw read -0.0 deg on every swing, and
                // freeRotation.x read under one degree on every swing (so the
                // sign test called everything LEFT).
                //
                // BGSAttackData::IsLeftAttack() is an ENGINE read, not an
                // animation tag, so it survives MCO/BFCO and any animation
                // replacer — [[compatibility-first-design]]. DDC already reads
                // the same attackData for the power-attack flag.
                auto& rtSide = a_player->GetActorRuntimeData();
                if (auto* procSide = rtSide.currentProcess) {
                    if (auto* highSide = procSide->high) {
                        if (auto atkSide = highSide->attackData) {
                            // IsLeftAttack() RETURNS FALSE FOR BOTH mounted
                            // attacks — measured: it called
                            // 'attackStart_MC_1HMLeft' a RIGHT attack. The
                            // flag it reads is not set on the mounted-combat
                            // attack records.
                            //
                            // The ATKE event name IS set, and vanilla names
                            // these explicitly: attackStart_MC_1HMLeft /
                            // attackStart_MC_1HMRight ("MC" = mounted combat).
                            // That string is engine DATA off the race's attack
                            // record, not an animation annotation, so it is
                            // still mod-agnostic in the way that matters.
                            // IsLeftAttack() stays as the fallback for any
                            // record that does not carry the suffix.
                            const char*      evPtr = atkSide->event.c_str();
                            std::string_view ev(evPtr ? evPtr : "");
                            const auto endsWithCI = [](std::string_view s, std::string_view suf) {
                                if (s.size() < suf.size()) return false;
                                s.remove_prefix(s.size() - suf.size());
                                for (std::size_t i = 0; i < suf.size(); ++i) {
                                    if (std::tolower(static_cast<unsigned char>(s[i])) !=
                                        std::tolower(static_cast<unsigned char>(suf[i]))) {
                                        return false;
                                    }
                                }
                                return true;
                            };
                            bool leftSwing;
                            const char* how;
                            if (endsWithCI(ev, "left"))       { leftSwing = true;  how = "event suffix"; }
                            else if (endsWithCI(ev, "right")) { leftSwing = false; how = "event suffix"; }
                            else { leftSwing = atkSide->IsLeftAttack(); how = "IsLeftAttack fallback"; }

                            mountAttackSide = leftSwing ? MountAttackSide::Left
                                                        : MountAttackSide::Right;
                            spdlog::debug("[MOUNTATK] mounted swing hand = {} via {} "
                                         "(attack event '{}')",
                                         leftSwing ? "LEFT" : "RIGHT", how, ev);
                            stateChanged = true;
                        }
                    }
                }
            }
            (void)pcSide;
        }

        if (a_player) {
            // AFTER PollDragonRiding — it reads isDragonRiding, and must see
            // this frame's value so dismounting parks it the same frame.
            PollDragonAction(a_player);
            PollRace(a_player);
            PollEquipment(a_player);
            PollHorseback(a_player);
            PollWeaponDrawn(a_player);
            PollHandCaster(a_player);
            PollSneak(a_player);
            PollSprint(a_player);
            PollSwimming(a_player);
            PollParaglide(a_player);
            PollWerewolfFeed(a_player);
            PollAttack(a_player);
            PollBlocking(a_player);
            PollShoutWords(a_player);
            PollShoutLinger();
            PollTargetLock();
            EnsureAnimGraphSubscription(a_player);
            // [CLIPCAM] same cadence, same manager-change trick: keep the
            // clip hook's player scope fresh in 1p AND 3p.
            AnimationCameraController::GetSingleton().EnsurePlayerScope(a_player);
        }

        // Quick Tune shout hold — runs every frame (independent of the
        // resolution gate) so the held shout survives the linger expiry.
        UpdateMenuStateHold();

        // ===== Resolution gate =====
        // Skip the priority stack walk entirely if no input has changed since
        // the last resolved frame. The vast majority of frames hit this path.
        if (stateChanged) {
            stateChanged = false;

            // Reset blockKind every resolution pass — only ResolveBlocking
            // sets it to non-None. The other Resolve* don't need to track it,
            // so a single clear here keeps the per-resolver bodies tidy.
            blockKind = BlockKind::None;

            // ===== Priority stack =====
            //   Transformation > VampireLord > Mount > Blocking > Weapon > Sheathed
            // Blocking sits above the regular Weapon branch because it's a
            // dedicated camera category — whenever the player is actively
            // blocking (or mid-bash from a blocked stance), the Blocking
            // profile wins regardless of which weapon is equipped.
            [&] {
                if (ResolveTransformation()) return;
                if (ResolveVampireLord())    return;
                if (ResolveMount())          return;
                if (ResolveBlocking())       return;
                if (a_player && ResolveWeapon()) return;
                ResolveSheathed();
            }();
        }

        // ===== Attack Lag (Cinematic Effects) =====
        // Runs every frame (holds are time-driven, not edge-driven), after the
        // resolution gate so it always compares against this frame's live
        // committed outputs.
        UpdateAttackLag();
    }

    void StateResolver::UpdateAttackLag()
    {
        const auto& s = SettingsManager::GetSingleton();

        // This frame's live resolution outputs (what the getters would report
        // with no hold active).
        const AttackLagSnap fresh{ state, subState, school, castType,
                                   cachedCastHand, isPowerAttack, powerAttackDir,
                                   isBowZoomed, IsLiveConcentrationCast() };

        // Third person only. In 1p the hold neither starts nor survives, so
        // 1p FOV/noise routing always sees live values; a POV switch mid-hold
        // commits the pending base transition immediately.
        auto* pc = RE::PlayerCamera::GetSingleton();
        const bool in3p = pc && !pc->IsInFirstPerson();

        // "Plain base of the given state": same top-level state, no sub-state,
        // no cast, no power attack, not blocking. This is the ONLY fresh
        // resolution a hold is allowed to mask — anything else (sprint, sneak,
        // shout, block, sheathe, state change, new attack) must show through
        // instantly, per the feature contract.
        auto isPlainBaseOf = [&](CameraState a_family) {
            return fresh.state == a_family &&
                   fresh.sub   == CameraSubState::None &&
                   fresh.cast  == CastType::None &&
                   !fresh.pa &&
                   blockKind == BlockKind::None;
        };

        // Category of an effective output, or no category when it isn't an
        // attack condition.
        //
        // SCOPE: projectile attacks only. Melee used to be a category here
        // and no longer is — the lag exists to let the camera ride out a
        // shot, and a sword swing has nothing in flight to ride out.
        //   Archery — Bow / Crossbow with the Attack sub-state.
        //   Magic   — the Magic AND Staves states (a staff shot is a magic
        //             shot; it just comes out of a stick), any school /
        //             cast hand, riding on top of the internal cast linger.
        //             Concentration is excluded below: a sustained stream
        //             isn't a launch, and holding the cast framing after one
        //             ends is the behaviour that made a mis-tuned per-hand
        //             cell read as a random sticky zoom-out.
        enum class Cat { None, Magic, Archery };
        auto classify = [](const AttackLagSnap& a) {
            switch (a.state) {
            case CameraState::Bow:
            case CameraState::Crossbow:
                return (a.sub == CameraSubState::Attack) ? Cat::Archery : Cat::None;
            case CameraState::Magic:
            case CameraState::Staves:
                return (a.cast != CastType::None && !a.concentration)
                           ? Cat::Magic : Cat::None;
            default:
                return Cat::None;
            }
        };

        // Consume this frame's projectile-launch events. A hold may only arm
        // if something was actually FIRED during the attack that just ended —
        // that is what "projectiles only" means in practice. Draw-cancels, dry
        // swings and interrupted casts all end an attack state without ever
        // bumping these, and correctly produce no lag.
        {
            const auto magicCtr = projectileMagicCounter.load(std::memory_order_relaxed);
            const auto arrowCtr = projectileArrowCounter.load(std::memory_order_relaxed);
            if (magicCtr != projectileMagicSeen) {
                projectileMagicSeen  = magicCtr;
                attackLagMagicFired  = true;
            }
            if (arrowCtr != projectileArrowSeen) {
                projectileArrowSeen   = arrowCtr;
                attackLagArcheryFired = true;
            }
        }

        const auto now = CameraEffectClock::Now();

        if (attackLagHolding) {
            // A hold only survives while the fresh resolution is still the
            // held state's plain base, we're still in 3p, and time remains.
            if (!in3p || !isPlainBaseOf(attackLagHeld.state) || now >= attackLagUntil) {
                attackLagHolding = false;
            }
        } else if (in3p) {
            // Attack→base edge: what consumers SAW last frame was an attack
            // condition, and this frame's live resolution is that state's
            // plain base. Arm the hold with the category's configured lag.
            const Cat  cat   = classify(attackLagPrevEff);
            const bool ended = (cat != Cat::None) && isPlainBaseOf(attackLagPrevEff.state);
            if (ended) {
                float lagSec = 0.0f;
                bool  fired  = false;
                switch (cat) {
                case Cat::Magic:   lagSec = s.attackLagMagic;   fired = attackLagMagicFired;   break;
                case Cat::Archery: lagSec = s.attackLagArchery; fired = attackLagArcheryFired; break;
                default: break;
                }
                if (lagSec > 0.001f && fired) {
                    attackLagHeld    = attackLagPrevEff;
                    attackLagUntil   = now + std::chrono::milliseconds(
                                           static_cast<int>(lagSec * 1000.0f));
                    attackLagHolding = true;
                    spdlog::debug("StateResolver: attack lag hold started ({}s, {})",
                                 lagSec, cat == Cat::Magic ? "magic" : "archery");
                }
                // Consume the launch latches on the edge whether or not a
                // hold armed (slider at 0, nothing fired). A launch only
                // authorises the attack it belonged to — leaving one set
                // would let an old shot license a later, unrelated hold.
                attackLagMagicFired   = false;
                attackLagArcheryFired = false;
            }
        }

        // Safety drain. The arm path above is 3p-only and edge-only, so a
        // launch during a cast the player then walks out of — or any launch
        // at all while in first person — would otherwise leave a latch set
        // indefinitely. Once no attack condition is live and nothing is
        // held, there is no attack for a pending launch to belong to.
        if (!attackLagHolding && classify(fresh) == Cat::None) {
            attackLagMagicFired   = false;
            attackLagArcheryFired = false;
        }

        // Record what consumers actually see this frame for next frame's
        // edge detection.
        attackLagPrevEff = attackLagHolding ? attackLagHeld : fresh;
    }

    // ----- Per-frame polls -----

    void StateResolver::PollRace(RE::PlayerCharacter* a_player)
    {
        // Belt-and-suspenders: TESSwitchRaceCompleteEvent is the primary
        // signal for transformations, but it can fail to fire on revert in
        // some setups (auto-revert from werewolf timer, mod conflicts, save
        // reload mid-form). Re-deriving the cached flags from the player's
        // current race every frame guarantees we self-heal on the next
        // frame after any missed event.
        auto* race = a_player->GetRace();
        const bool nowWerewolf    = IsWerewolfRace(race);
        const bool nowVampireLord = IsVampireLordRace(race);
        if (nowWerewolf != isWerewolf) {
            isWerewolf = nowWerewolf;
            spdlog::debug("StateResolver: Werewolf {} (poll)",
                         nowWerewolf ? "transformation detected" : "revert detected");
            stateChanged = true;
        }
        if (nowVampireLord != isVampireLord) {
            isVampireLord = nowVampireLord;
            spdlog::debug("StateResolver: VampireLord {} (poll)",
                         nowVampireLord ? "transformation detected" : "revert detected");
            stateChanged = true;
        }
    }

    void StateResolver::PollEquipment(RE::PlayerCharacter* a_player)
    {
        // Belt-and-suspenders: TESEquipEvent is the primary signal but it
        // doesn't fire on save load — the cached loadout flags can carry
        // stale state from the previous in-memory session into a fresh save
        // (especially mid-werewolf-form scenarios). Recomputing every frame
        // is cheap (a few GetEquippedObject calls) and self-heals on the
        // next frame after any missed equip event.
        const bool nowMelee    = PlayerHasMeleeEquipped(a_player);
        const bool nowBow      = PlayerHasBowEquipped(a_player);
        const bool nowCrossbow = PlayerHasCrossbowEquipped(a_player);
        const bool nowStaff    = PlayerHasStaffEquipped(a_player);
        const bool nowSpell    = PlayerHasSpellEquipped(a_player);
        const bool nowShield   = PlayerHasShieldEquipped(a_player);
        const bool nowOneH     = PlayerHasOneHandedEquipped(a_player);
        const bool nowTwoH     = PlayerHasTwoHandedEquipped(a_player);
        if (nowMelee != isMeleeEquipped) {
            isMeleeEquipped = nowMelee;
            spdlog::debug("StateResolver: melee equipped = {} (poll)", nowMelee);
            stateChanged = true;
        }
        if (nowBow != isBowEquipped) {
            isBowEquipped = nowBow;
            spdlog::debug("StateResolver: bow equipped = {} (poll)", nowBow);
            stateChanged = true;
        }
        if (nowCrossbow != isCrossbowEquipped) {
            isCrossbowEquipped = nowCrossbow;
            spdlog::debug("StateResolver: crossbow equipped = {} (poll)", nowCrossbow);
            stateChanged = true;
        }
        if (nowStaff != isStaffEquipped) {
            isStaffEquipped = nowStaff;
            spdlog::debug("StateResolver: staff equipped = {} (poll)", nowStaff);
            stateChanged = true;
        }
        if (nowSpell != isSpellEquipped) {
            isSpellEquipped = nowSpell;
            spdlog::debug("StateResolver: spell equipped = {} (poll)", nowSpell);
            stateChanged = true;
        }
        if (nowShield != isShieldEquipped) {
            isShieldEquipped = nowShield;
            spdlog::debug("StateResolver: shield equipped = {} (poll)", nowShield);
            stateChanged = true;
        }
        if (nowOneH != isOneHandedEquipped) {
            isOneHandedEquipped = nowOneH;
            spdlog::debug("StateResolver: 1H melee equipped = {} (poll)", nowOneH);
            stateChanged = true;
        }
        if (nowTwoH != isTwoHandedEquipped) {
            isTwoHandedEquipped = nowTwoH;
            spdlog::debug("StateResolver: 2H melee equipped = {} (poll)", nowTwoH);
            stateChanged = true;
        }
        const auto nowSchool = DetectPlayerSpellSchool(a_player);
        if (nowSchool != spellSchool) {
            spellSchool = nowSchool;
            spdlog::debug("StateResolver: spell school = {} (poll)", SchoolName(nowSchool));
            stateChanged = true;
        }
    }

    void StateResolver::PollDragonRiding()
    {
        auto* playerCam = RE::PlayerCamera::GetSingleton();
        const bool now = playerCam &&
            playerCam->currentState.get() ==
            playerCam->cameraStates[RE::CameraState::kDragon].get();
        if (now == isDragonRiding) return;
        isDragonRiding = now;
        spdlog::debug("StateResolver: dragon riding {}", now ? "detected" : "ended");
        stateChanged = true;
    }

    // What the dragon you are SITTING ON is doing.
    //
    // Deliberately NOT shared with the Cinematic Effects dragon scan in
    // CameraNoiseController, which answers a different question: that one
    // walks every dragon in range, ranks them by loudness against distance,
    // and shapes short IMPULSES with decay tails. This one has exactly one
    // actor, no distance, and needs a STEADY state with hysteresis, because
    // its output picks a camera profile rather than a shake. The two would
    // fight if forced together; what they do share is the reading of
    // flyState / meleeAttackState / caster state, which is engine truth and
    // cannot drift.
    void StateResolver::PollDragonAction(RE::PlayerCharacter* a_player)
    {
        using FState = RE::FLY_STATE;
        using AState = RE::ATTACK_STATE_ENUM;
        using Src    = RE::MagicSystem::CastingSource;
        using CType  = RE::MagicSystem::CastingType;
        using CState = RE::MagicCaster::State;

        // Not riding: park at Cruising so a stale action can't leak into the
        // next mount, and skip the work entirely.
        if (!isDragonRiding || !a_player) {
            if (dragonAction != DragonAction::Cruising) {
                dragonAction        = DragonAction::Cruising;
                dragonActionPending = DragonAction::Cruising;
                dragonActionHeldFor = 0.0f;
                stateChanged        = true;
            }
            return;
        }

        RE::NiPointer<RE::Actor> mountPtr;
        if (!a_player->GetMount(mountPtr) || !mountPtr) return;
        auto* mount = mountPtr.get();

        // ---- Axis 1: POSTURE -------------------------------------------
        // Where the dragon is. Drives the idle rows directly, and picks which
        // column of the action matrix an action lands in.
        enum class Posture { Flying, Hovering, Grounded, Takeoff, Landing };
        Posture posture = Posture::Flying;
        bool    acting  = false;   // melee bracket, for axis 2

        if (auto* as = mount->AsActorState()) {
            // EVERY enumerator spelled out. FLY_STATE also carries kAction
            // (6), which a dragon uses for a scripted AIRBORNE move — a
            // catch-all default would have called that "grounded" and put a
            // ground framing on a dragon in mid-air.
            switch (as->actorState1.flyState) {
            case FState::kNone:     posture = Posture::Grounded; break;  // on its feet
            case FState::kPerching: posture = Posture::Grounded; break;
            case FState::kHovering: posture = Posture::Hovering; break;
            case FState::kTakeOff:  posture = Posture::Takeoff;  break;
            case FState::kLanding:  posture = Posture::Landing;  break;
            case FState::kCruising: posture = Posture::Flying;   break;
            case FState::kAction:   posture = Posture::Flying;   break;  // airborne, unclassified
            }

            // Melee — bite, tail and wing all land here (see the DragonAction
            // comment for why they are one). kDraw is the wind-up and counts:
            // the camera wants to be in position BEFORE the impact, which is
            // the opposite of the shake system's needs.
            const auto m = as->actorState1.meleeAttackState;
            acting = (m == AState::kDraw || m == AState::kSwing ||
                      m == AState::kHit  || m == AState::kFollowThrough);
        }

        // ---- Axis 2: ACT -----------------------------------------------
        // Breath vs projectile, straight off the caster — the same test the
        // Cinematic Effects sources use. Concentration is the sustained
        // breath; a fire-and-forget lob frames like an attack, not a breath.
        enum class Act { None, Attack, Breath };
        Act act = acting ? Act::Attack : Act::None;
        for (auto cs : { Src::kLeftHand, Src::kRightHand, Src::kOther, Src::kInstant }) {
            auto* caster = mount->GetMagicCaster(cs);
            if (!caster || caster->state.get() != CState::kCasting) continue;
            auto* spell = caster->currentSpell ? caster->currentSpell->As<RE::SpellItem>() : nullptr;
            if (!spell) continue;
            if (spell->GetCastingType() == CType::kConcentration) { act = Act::Breath; break; }
            if (act == Act::None) act = Act::Attack;
        }

        // ---- Cross the two ----------------------------------------------
        // Takeoff and Landing have no action row of their own; a dragon that
        // attacks mid-takeoff is attacking from the air, so both fold into
        // the Flying column.
        DragonAction want;
        switch (act) {
        case Act::Breath:
            want = posture == Posture::Grounded ? DragonAction::BreathGrounded
                 : posture == Posture::Hovering ? DragonAction::BreathHovering
                                                : DragonAction::BreathFlying;
            break;
        case Act::Attack:
            want = posture == Posture::Grounded ? DragonAction::AttackGrounded
                 : posture == Posture::Hovering ? DragonAction::AttackHovering
                                                : DragonAction::AttackFlying;
            break;
        default:
            switch (posture) {
            case Posture::Grounded: want = DragonAction::Perched;  break;
            case Posture::Hovering: want = DragonAction::Hovering; break;
            case Posture::Takeoff:  want = DragonAction::Takeoff;  break;
            case Posture::Landing:  want = DragonAction::Landing;  break;
            default:                want = DragonAction::Cruising; break;
            }
            break;
        }

        // ---- Hysteresis --------------------------------------------------
        // A HIGHER priority preempts immediately: a breath starting mid-hover
        // must not wait out the hover's dwell. A LOWER one has to outlast the
        // dwell, which is what stops a one-frame flyState wobble (or the gap
        // between kSwing and kFollowThrough) strobing the camera between two
        // framings.
        constexpr float kDragonActionDwell = 0.35f;
        // Own clock, like the other timed polls in this file — the resolver
        // has no shared frame delta. Clamped so an alt-tab or a loading
        // screen can't retire a dwell in one frame.
        static auto  sDragTp = CameraEffectClock::Now();
        const auto   nowDrag = CameraEffectClock::Now();
        const float  dragDt  = std::clamp(
            std::chrono::duration<float>(nowDrag - sDragTp).count(), 0.0f, 0.10f);
        sDragTp = nowDrag;

        if (want > dragonAction) {
            dragonActionPending = want;
            dragonActionHeldFor = 0.0f;
        } else if (want != dragonAction) {
            if (want != dragonActionPending) {
                dragonActionPending = want;
                dragonActionHeldFor = 0.0f;
            }
            dragonActionHeldFor += dragDt;
            if (dragonActionHeldFor < kDragonActionDwell) return;
        } else {
            // Settled on what we already report — cancel any pending fall.
            dragonActionPending = want;
            dragonActionHeldFor = 0.0f;
            return;
        }

        if (dragonActionPending == dragonAction) return;
        static constexpr const char* kNames[] = {
            "Cruising", "Perched", "Hovering", "Takeoff", "Landing",
            "Attack (Grounded)", "Attack (Hovering)", "Attack (Flying)",
            "Breath (Grounded)", "Breath (Hovering)", "Breath (Flying)"
        };
        static_assert(sizeof(kNames) / sizeof(kNames[0]) == kDragonActionCount,
                      "DragonAction names must cover every enumerator");
        const auto prev = dragonAction;
        dragonAction        = dragonActionPending;
        dragonActionHeldFor = 0.0f;
        stateChanged        = true;
        // `locked` settles the open question about the TL tree: TDM is not
        // supposed to be able to lock while dragon riding without the Lock-On
        // Extension Patch, which is why dragon riding has ONE TL slot instead
        // of eleven. If this ever prints true on a install without the patch,
        // that assumption is wrong and the single slot is worth revisiting.
        spdlog::debug("[DRAGONSUB] {} -> {} | locked={}",
                     kNames[static_cast<std::size_t>(prev)],
                     kNames[static_cast<std::size_t>(dragonAction)],
                     isTargetLocked);
    }

    void StateResolver::PollHorseback(RE::PlayerCharacter* a_player)
    {
        const bool now = a_player->IsOnMount();
        if (now == isHorseback) return;
        isHorseback = now;
        spdlog::debug("StateResolver: {}", now ? "mount detected (Horseback)" : "dismount detected");
        stateChanged = true;
    }

    void StateResolver::PollWeaponDrawn(RE::PlayerCharacter* a_player)
    {
        bool now = false;
        if (auto* actorState = a_player->AsActorState()) {
            now = actorState->IsWeaponDrawn();
        }
        if (now == isWeaponDrawn) return;
        isWeaponDrawn = now;
        spdlog::debug("StateResolver: weapon {}", now ? "drawn" : "sheathed");
        stateChanged = true;
    }

    void StateResolver::PollHandCaster(RE::PlayerCharacter* a_player)
    {
        // Live poll: walks both hand casters, classifies what each is
        // currently casting, priority-resolves into a single active cast
        // type + school for the frame. Updates isHandCasterActive,
        // cachedCastType, and cachedCastSchool together — they share the
        // same input and have to stay consistent.
        auto info = ResolveActiveHandCast(a_player);

        // ---- Dual-cast latch ------------------------------------------------
        // Both engine dual-cast flags are edges, not states (see the comment in
        // ResolveActiveHandCast for the measurements). A dual cast therefore
        // announces itself for a couple of hundred milliseconds and then looks
        // exactly like a left-hand cast for the remaining second or more, which
        // is what made the Both Hands per-hand override apply for three frames
        // and then hand the camera to the Left Hand profile.
        //
        // Latch it for the life of the cast instead: once any frame of THIS
        // cast says dual, the cast IS dual until it ends. Safe because vanilla
        // has no way to half-release a dual cast — the merged spell is one
        // cast, so there is no state where "it started dual but is now one
        // hand" is the truth. A single-hand cast never sets either flag, so it
        // never latches and its behaviour is unchanged.
        //
        // Released when the cast ends (type None), which also covers dying,
        // sheathing and menu interrupts since they all drop the caster.
        if (info.type == CastType::None) {
            if (castDualLatched) {
                castDualLatched = false;
                spdlog::debug("StateResolver: dual-cast latch released");
            }
        } else if (info.dualNow && !castDualLatched) {
            castDualLatched = true;
            spdlog::debug("StateResolver: dual-cast latch engaged (hand -> Both for this cast)");
        }
        if (castDualLatched) info.hand = CastingHand::Both;

        // Per-hand release detection — runs ahead of the combined linger
        // logic so dual-cast (right F&F + left F&F) registers as TWO
        // release events for session escalation, not one. A "release"
        // is a falling edge of the per-hand F&F flag, gated on whether
        // the cycle ever reached a firing state — taps that
        // never charge enough (kCharging → kNone) are ignored so the
        // camera doesn't latch into the F&F linger profile from a
        // partial press. On each real release, if the previous one
        // happened within the session window, mark a session as
        // pending; the linger consumes it when it eventually starts.
        constexpr int kCastLingerSessionMs = 1000;
        const auto nowTp = CameraEffectClock::Now();

        // Sticky "saw firing state" — set whenever the per-hand caster
        // state hit kReady/kCasting during the current F&F cycle.
        if (info.leftFiring)  leftSawFiring  = true;
        if (info.rightFiring) rightSawFiring = true;

        const bool leftFallEdge   = lastLeftHandFF  && !info.leftFF;
        const bool rightFallEdge  = lastRightHandFF && !info.rightFF;
        const bool leftRealRelease  = leftFallEdge  && leftSawFiring;
        const bool rightRealRelease = rightFallEdge && rightSawFiring;
        const bool anyRealRelease   = leftRealRelease || rightRealRelease;

        // Reset the sticky flag on falling edges so the next cycle
        // starts clean. Tap-cancels reset without ever counting.
        if (leftFallEdge)  leftSawFiring  = false;
        if (rightFallEdge) rightSawFiring = false;

        if (anyRealRelease) {
            const bool hadPriorRelease = (lastFFReleaseTime.time_since_epoch().count() != 0);
            if (hadPriorRelease) {
                const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         nowTp - lastFFReleaseTime).count();
                castSessionPending = (sinceMs <= kCastLingerSessionMs);
            } else {
                castSessionPending = false;
            }
            lastFFReleaseTime = nowTp;
        }
        lastLeftHandFF  = info.leftFF;
        lastRightHandFF = info.rightFF;

        // Cast-end linger: when the live cast drops to None but we had an
        // active cast the previous frame, hold the cached values for a
        // short window before reverting. Gives the camera spring a beat
        // on the cast profile after the trigger is released instead of
        // snapping straight back. The three fields (isHandCasterActive,
        // cachedCastType, cachedCastSchool) linger together because
        // consumers gate selection on isHandCasterActive and expect the
        // type/school to be coherent with it.
        //
        // Adaptive window:
        //   - kCastLingerBaseMs is used for a cold cast end. Short so a
        //     one-off cast returns to base promptly.
        //   - kCastLingerSessionMs is used once the player proves they're
        //     rapid-firing: if a new cast starts *during* a linger, we
        //     flip castSessionActive on. The next cast end (and any
        //     further chained ones) gets the longer window so inter-cast
        //     gaps stay parked on the cast profile instead of flickering
        //     back toward base and then forward again.
        //   - castSessionActive resets on a linger commit (timed out
        //     without a re-cast — the session is definitively over).
        // Base window has to cover the typical recovery gap between
        // firebolt-tempo casts (~250ms observed) so session can actually
        // activate on the second cast; otherwise every back-to-back cast
        // commits with session=false and the camera flickers between
        // each trigger release. Session window then holds through slower
        // chained casts.
        constexpr int kCastLingerBaseMs    = 350;
        // kCastLingerSessionMs is declared at the top of the function
        // for the per-hand release tracker; reuse it here.

        // Linger is specifically a "fire-and-forget / ritual release" thing.
        // Concentration casts don't wind up and release the same way, so
        // they don't trigger or participate in the session-escalation path.
        const auto isFAF = [](CastType t) {
            return t == CastType::FireAndForget || t == CastType::Ritual;
        };

        // Publish the LIVE caster state before the linger freezes anything —
        // the noise's early hand-off consults this so it can stand down when
        // the linger is masking a real cast (a concentration stream in the
        // other hand, or the next charge already building) rather than plain
        // silence. See StateResolver::IsLiveCasting.
        castLiveCasting = info.active;

        if (castLingering) {
            if (isFAF(info.type)) {
                // Fresh F&F cast started during linger — the player is
                // rapid-firing. Activate session so the NEXT cast end uses
                // the longer linger window. Cancel current linger and sync.
                castSessionActive = true;
                castLingering = false;
                if (info.active != isHandCasterActive) {
                    isHandCasterActive = info.active;
                    spdlog::debug("StateResolver: hand caster active = {}", info.active);
                    stateChanged = true;
                }
                if (info.type != cachedCastType) {
                    cachedCastType = info.type;
                    spdlog::debug("StateResolver: cast type = {} (live, session active)", CastTypeName(info.type));
                    stateChanged = true;
                }
                if (info.school != cachedCastSchool) {
                    cachedCastSchool = info.school;
                    spdlog::debug("StateResolver: cast school = {} (live)", SchoolName(info.school));
                    stateChanged = true;
                }
                if (info.hand != cachedCastHand) {
                    cachedCastHand = info.hand;
                    stateChanged = true;
                }
            } else {
                // Not a fresh F&F cast. Could be Concentration continuing or
                // nothing at all. Let linger run — cached F&F values stay
                // frozen — then sync to whatever's live when it expires.
                const int  lingerMs = castSessionActive ? kCastLingerSessionMs : kCastLingerBaseMs;
                const auto elapsed  = CameraEffectClock::Now() - castEndTime;
                const auto ms       = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (ms >= lingerMs) {
                    // Linger done — sync to the now-live state. If Concentration
                    // is running (player still holding the other hand), we
                    // settle onto it. If nothing is live, we go to None.
                    isHandCasterActive = info.active;
                    cachedCastType     = info.type;
                    cachedCastSchool   = info.school;
                    cachedCastHand     = info.hand;
                    castLingering      = false;
                    const bool wasSession = castSessionActive;
                    castSessionActive  = false;
                    spdlog::debug("StateResolver: cast end committed (linger {}ms, session={}, now={})",
                                 ms, wasSession, CastTypeName(info.type));
                    stateChanged = true;
                }
                // else: still lingering — cached values frozen, no updates.
            }
        } else {
            // Not lingering. Trigger linger on any F&F/Ritual release,
            // regardless of what comes next (None OR Concentration). That
            // way firing Firebolt while holding Flames still gets the same
            // ~350ms linger window as firing Firebolt alone.
            // Only start a linger when the cycle actually fired — a
            // tap-cancel (kCharging → kNone, never reached kCasting)
            // still drops cachedCastType from F&F to None this frame,
            // but should not keep the camera parked on the F&F profile.
            const bool fafJustReleased = isFAF(cachedCastType) && !isFAF(info.type) && anyRealRelease;
            if (fafJustReleased) {
                castEndTime   = CameraEffectClock::Now();
                castLingering = true;
                // Prime session from the per-hand release tracker so a
                // dual-cast (right F&F → left F&F) lands here with
                // session already active, matching single-hand rapid
                // fire behavior.
                if (castSessionPending) {
                    castSessionActive = true;
                    castSessionPending = false;
                }
                // Hold the F&F values until the window expires.
            } else {
                if (info.active != isHandCasterActive) {
                    isHandCasterActive = info.active;
                    spdlog::debug("StateResolver: hand caster active = {}", info.active);
                    stateChanged = true;
                }
                if (info.type != cachedCastType) {
                    cachedCastType = info.type;
                    spdlog::debug("StateResolver: cast type = {} (live)", CastTypeName(info.type));
                    stateChanged = true;
                }
                if (info.school != cachedCastSchool) {
                    cachedCastSchool = info.school;
                    spdlog::debug("StateResolver: cast school = {} (live)", SchoolName(info.school));
                    stateChanged = true;
                }
                if (info.hand != cachedCastHand) {
                    cachedCastHand = info.hand;
                    stateChanged = true;
                }
            }
        }
        if (info.staffType != cachedStaffCastType ||
            info.staffConcentration != cachedStaffConcentration) {
            cachedStaffCastType = info.staffType;
            cachedStaffConcentration = info.staffConcentration;
            spdlog::debug("StateResolver: staff cast type = {} (live, concentration={})",
                         CastTypeName(info.staffType), info.staffConcentration);
            stateChanged = true;
        }
        if (info.staffSchool != cachedStaffCastSchool) {
            cachedStaffCastSchool = info.staffSchool;
            spdlog::debug("StateResolver: staff school = {} (live)", SchoolName(info.staffSchool));
            stateChanged = true;
        }
        if (info.locked != isCastLocked) {
            isCastLocked = info.locked;
            spdlog::debug("StateResolver: cast lock = {} (ritual/LS-windup)", info.locked);
            // No stateChanged here — the lock flag only gates free-rotation in
            // the hook; it doesn't affect profile selection.
        }
        if (info.ward != isCastingWard) {
            isCastingWard = info.ward;
            spdlog::debug("StateResolver: ward casting = {}", info.ward);
            stateChanged = true;
        }
    }

    void StateResolver::PollSneak(RE::PlayerCharacter* a_player)
    {
        const bool now = a_player->IsSneaking();
        if (now == isSneaking) return;
        isSneaking = now;
        spdlog::debug("StateResolver: sneak {}", now ? "entered" : "exited");
        stateChanged = true;
    }

    void StateResolver::PollParaglide(RE::PlayerCharacter* a_player)
    {
        // Grounded safety net for the paraglide latch — the graph tags own
        // the edges (see the StartPara/EndPara handling in ProcessEvent);
        // this only CLEARS. It mirrors the paraglider mod's own force-end
        // exactly (its watcher drops the glide on the char controller's
        // kOnGround), so a missed EndPara — a graph swap mid-glide, a race
        // change, the sink briefly deaf — ends at the same moment the mod
        // itself would have ended it. Swimming gets the same treatment: a
        // glide cannot survive entering water.
        if (!isParagliding) return;
        bool grounded = false;
        if (auto* cc = a_player->GetCharController()) {
            grounded = cc->context.currentState ==
                       RE::hkpCharacterStateType::kOnGround;
        }
        if (grounded || isSwimming) {
            isParagliding = false;
            stateChanged  = true;
            spdlog::debug("StateResolver: paraglide ended ({})",
                         grounded ? "grounded" : "swimming");
        }
    }

    // ----- Sub-state precedence: Shout > Attack > Sprint > Sneak > None -----

    void StateResolver::PollSprint(RE::PlayerCharacter* a_player)
    {
        // Use actorState1.sprinting (the engine's input-driven flag) rather
        // than the "IsSprinting" anim-graph variable. The graph variable lags
        // the actual key release by the deceleration animation duration
        // (~200-300ms), which the user sees as a "delay" before our camera
        // transition starts. The actor-state flag flips on the same frame as
        // the key release, so the camera transition starts immediately.
        bool now = false;
        if (auto* actorState = a_player->AsActorState()) {
            now = actorState->IsSprinting();
        }

        // Sprint-jump carry: jumping (Better Jumping's sprint jumps, vanilla
        // sprint-leaps off ledges) clears the engine's sprinting flag for the
        // airtime, which yanked the camera to the base state mid-leap and
        // back on landing. If the player left the ground WHILE sprinting,
        // keep reporting sprint until they land; a jump from a walk (carry
        // primed false on the ground) is untouched. On landing the live flag
        // takes over — still holding sprint resumes seamlessly, otherwise
        // the normal sprint-stop transition runs at touchdown.
        const bool inAir = a_player->IsInMidair();
        if (!inAir) {
            sprintAirCarry = now;   // grounded: prime from the live flag
        } else if (sprintAirCarry) {
            now = true;             // airborne off a sprinting takeoff: hold
        }

        if (now == isSprinting) return;
        isSprinting = now;
        spdlog::debug("StateResolver: sprint {}", now ? "started" : "stopped");
        stateChanged = true;
    }

    void StateResolver::PollSwimming(RE::PlayerCharacter* a_player)
    {
        const auto* actorState = a_player->AsActorState();
        bool now = actorState && actorState->IsSwimming();

        // Mounted case: the player's swim bit stays false while on a horse —
        // swim/water state lives on the horse. Read the horse's own swimming
        // bit (the engine sets it when the horse actually starts paddling,
        // i.e. deep enough to be off its feet). IsInWater() was too lenient
        // — any puddle would trip it.
        if (!now && isHorseback) {
            RE::NiPointer<RE::Actor> mount;
            if (a_player->GetMount(mount) && mount) {
                const auto* mountState = mount->AsActorState();
                now = mountState && mountState->IsSwimming();
            }
        }

        if (now == isSwimming) return;
        isSwimming = now;
        spdlog::debug("StateResolver: swim {}", now ? "entered" : "exited");
        stateChanged = true;
    }

    void StateResolver::RequestMenuStateHold()
    {
        // Re-armed every frame either menu is open. A small TTL (vs a
        // one-shot bool) tolerates MenuUI's tick and StateResolver::Update
        // running at slightly different points in the frame.
        menuHoldTtl = 3;
        if (CameraEffectClock::IsPaused() && !menuHoldActive) UpdateMenuStateHold();
    }

    void StateResolver::UpdateMenuStateHold()
    {
        if (menuHoldTtl > 0) {
            --menuHoldTtl;
            if (!menuHoldActive) {
                // First armed frame: snapshot the REPORTED state — through
                // the getters, so attack-lag holds, cast lingers and the
                // shout linger fold into the captured picture — then flip
                // active. Everything from here on reports this snapshot;
                // the underlying machine keeps running truthfully so no
                // timer or watchdog is ever corrupted by a long edit.
                MenuHoldSnapshot snap{};
                snap.state          = GetState();
                snap.sub            = GetSubState();
                snap.school         = GetSchool();
                snap.cast           = GetCastType();
                snap.concentration  = IsConcentrationCast();
                snap.hand           = GetCastingHand();
                snap.block          = GetBlockKind();
                snap.dragon         = GetDragonAction();
                snap.paDir          = GetPowerAttackDirection();
                snap.sneaking       = IsSneaking();
                snap.sprinting      = IsSprinting();
                snap.weaponDrawn    = IsWeaponDrawn();
                snap.locked         = IsTargetLocked();
                snap.attackSprint   = IsAttackSprint();
                snap.attackSneak    = IsAttackSneak();
                snap.powerAttacking = IsPowerAttacking();
                snap.wwFeeding      = IsWerewolfFeeding();
                snap.vlLevitating   = IsVampireLordLevitating();
                snap.paragliding    = IsParagliding();
                snap.chargingSpell  = IsChargingSpell();
                snap.castingStream  = IsCastingStream();
                snap.bowZoomed      = IsBowZoomed();
                snap.rider          = GetRiderWeapon();
        snap.mountSide      = GetMountAttackSide();
                snap.shoutId        = GetActiveShoutId();
                std::uint32_t fid = 0;
                std::string   plg;
                if (GetActiveModShout(fid, plg)) {
                    snap.modShoutFormID = fid;
                    snap.modShoutPlugin = std::move(plg);
                }
                menuHold       = std::move(snap);
                menuHoldActive = true;
                spdlog::debug("StateResolver: menu state hold engaged (state={} sub={})",
                             static_cast<int>(menuHold.state),
                             static_cast<int>(menuHold.sub));
            }
        } else if (menuHoldActive) {
            // Menus closed — drop the hold and let the live state take
            // back over with a normal transition.
            menuHoldActive = false;
            menuHold       = {};
            stateChanged   = true;
            spdlog::debug("StateResolver: menu state hold released");
        }
    }

    void StateResolver::PollShoutLinger()
    {
        // Werewolf roar scheduled-start. kVoiceCast handler queued
        // isShouting=true for ~500ms after the howl began so the camera
        // transition syncs with the animation peak, not its wind-up.
        // Fire when the schedule expires.
        if (werewolfRoarScheduled) {
            const auto nowTp = CameraEffectClock::Now();
            if (nowTp >= werewolfRoarStartTime) {
                werewolfRoarScheduled = false;
                if (!isShouting) {
                    isShouting = true;
                    stateChanged = true;
                    spdlog::debug("StateResolver: werewolf roar scheduled-start fired");
                }
                // THE ROAR'S LENGTH LIVES HERE, and it is not obvious:
                // shoutEndTime is set into the FUTURE at the scheduled start,
                // and PollShoutLinger's tail then adds `lingerMs` (700ms for a
                // beast form) on top. So the roar state runs for this value
                // PLUS 700ms from the deferred start, and this is the number to
                // move when the roar outstays the howl. Tuning the noise
                // crossfade's hand-off instead — which is what the previous
                // round did — barely registers next to it, which is why that
                // change read as "unchanged".
                //
                // 1375 -> 775 -> 575 -> 475, each step trimming the roar's
                // tail by exactly that much (the later start above is already
                // priced in). This is THE knob for "the roar noise runs on too
                // long".
                shoutEndTime   = nowTp + std::chrono::milliseconds(475);
                shoutLingering = true;
            }
        }

        // Safety net for a missing shout-stop. Some shouts — notably
        // movement shouts like Whirlwind Sprint — don't deliver a
        // recognised stop tag: the dash swaps the behaviour graph mid-cast,
        // so the stop can fire on a graph we're momentarily between
        // subscriptions on (or the framework renames/omits it). Without a
        // stop the linger never starts, isShouting sticks true, and the
        // camera freezes on the shout profile (e.g. Whirlwind Sprint's
        // pulled-back framing) until the NEXT shout re-resolves the state.
        // If a shout has been held past the longest real shout-cast window
        // with no stop, start the linger so it ends cleanly. Werewolf roars
        // set shoutLingering at their scheduled start, so the !shoutLingering
        // guard already excludes them.
        if (isShouting && !shoutLingering) {
            constexpr auto kMaxShoutHold = std::chrono::milliseconds(3000);
            const auto now = CameraEffectClock::Now();
            // PAUSED TIME IS NOT SHOUT TIME (2026-09-04). This poll keeps
            // running while the game is paused in a menu, and the window is
            // wall-clock, so opening the DDC menu mid-shout to tune it
            // force-ended the shout 3.0s after it began — every time. The
            // 19:28-19:31 log: four shouts, each "menu state hold engaged
            // (sub=5)" then "safety-timeout" exactly 3.0s after "shout
            // started", while every shout cast with no menu ended on its
            // shoutStop. On close the real kVoiceFire / shoutStop then
            // arrived ~0.5s later with no shout state left to receive them,
            // so the shout played on the non-shout camera. Accumulate the
            // paused span and measure the window net of it.
            // ...AND THE DDC MENU HOLD. The 21:44 session showed the same
            // three force-ends with "paused 0ms excluded": the DDC menu is an
            // unpaused framework window (input blocked, world running), so
            // GameIsPaused() never reads true under it. While the menu state
            // hold is engaged the shout is being TUNED, not cast - its stop
            // cannot arrive until the menu closes - so that span is excluded
            // the same way.
            if (shoutPauseTick.time_since_epoch().count() != 0) {
                auto* ui = RE::UI::GetSingleton();
                const bool held = (ui && ui->GameIsPaused()) || menuHoldActive;
                if (held) shoutPausedAccum += (now - shoutPauseTick);
            }
            shoutPauseTick = now;
            if ((now - shoutStartTime) - shoutPausedAccum >= kMaxShoutHold) {
                shoutEndTime   = now;
                shoutLingering = true;
                spdlog::info("StateResolver: shout safety-timeout — no stop event seen, forcing end "
                             "(paused/menu-held {}ms excluded)",
                             std::chrono::duration_cast<std::chrono::milliseconds>(shoutPausedAccum).count());
            }
        } else {
            shoutPauseTick = {};
        }

        // Cinematic tail on shout exits. shoutstop anim event sets
        // shoutLingering; we commit isShouting=false once the window
        // elapses. If a new shout starts mid-linger, the anim event
        // handler clears shoutLingering and the new shout takes over.
        if (!shoutLingering) return;

        // 250ms tail that smooths the engine's shout-end animation — this
        // is what makes a shout-end transition feel smooth in vanilla.
        // Beast-form roars get a LONGER tail, not a shorter one: the howl
        // sets the engine's attack state for its whole recovery (~0.7s past
        // the voice window), and with a 0ms linger the resolver fell through
        // to the ATTACK sub-state the instant the voice cleared — the
        // post-roar zoom snap. Holding the Shout sub through the recovery
        // keeps the roar's own framing in charge until the animation is
        // actually done.
        // Per-entry shout Lag rides ON TOP of the base tail: the resolved
        // shout entry's CameraProfile::shoutLag (published each 3p frame by
        // CameraController) holds the Shout sub-state — and with it the
        // shout framing — for that long before the switch back to
        // sheathed/unsheathed. The shouts twin of Projectile Lag.
        const int lagMs = static_cast<int>(
            std::clamp(activeShoutLagSec, 0.0f, 30.0f) * 1000.0f);
        const int baseLingerMs = (isWerewolf || isVampireLord) ? 700 : 250;
        const int lingerMs     = baseLingerMs + lagMs;

        const auto elapsed = CameraEffectClock::Now() - shoutEndTime;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        // Lag is a FRAMING device, exactly like Attack Lag: it holds the
        // Shout sub-state so the CAMERA keeps the shout's framing. Camera
        // NOISE must not inherit it — noise has its own Fade Duration, and
        // stretching the state stretched the shout noise past that fade
        // ("Lag for Whirlwind Sprint extended the noise beyond its fade
        // duration", user 2026-08-16). This flag marks the window that is
        // purely lag — past the natural tail — so the noise resolver can
        // treat the shout as already over while the camera holds on.
        shoutLagHolding = (lagMs > 0) && (ms >= baseLingerMs) && (ms < lingerMs);

        if (ms >= lingerMs) {
            isShouting      = false;
            shoutLingering  = false;
            shoutLagHolding = false;
            activeShoutId  = std::nullopt;
            activeModShoutFormID = 0;
            activeModShoutPlugin.clear();
            spdlog::debug("StateResolver: shout ended (linger {}ms, lag {}ms)", ms, lagMs);
            stateChanged = true;
            // A HOWL-BORN ATTACK ENDS WITH THE HOWL. The werewolf howl's own
            // animation emits MeleeStart, so an "attack" session starts on the
            // roar's first frame — and its recovery outlives this linger by
            // ~0.6s (18:55 log: shout ended 53.216, attack ended 53.823). For
            // that window the resolver reported sub=Attack and the camera
            // eased toward transformationsWerewolfAttack and back: the
            // "zooms in after the roar before going back to unsheathed" dip,
            // present however the roar and base profiles are tuned, because
            // the profile in between is a THIRD one. The linger above was
            // grown to cover the recovery once already (see the comment) and
            // the recovery still wins; stop chasing its length and end the
            // phantom attack here instead. howlAttackSwallow keeps the rising
            // edge suppressed until the engine's IsAttacking actually drops,
            // so the same graph session can't restart it next frame.
            if (attackBeganDuringHowl && isAttacking) {
                isAttacking           = false;
                isPowerAttack         = false;
                powerFlagFromGraph    = false;
                attackLingering       = false;
                attackSessionActive   = false;
                attackBeganDuringHowl = false;
                howlAttackSwallow     = true;
                spdlog::debug("StateResolver: howl-born attack ended with the roar");
            }
        }
    }

    void StateResolver::NotifyWerewolfFeed(RE::FormID a_effect)
    {
        // Rising edge latches and starts the clock. A re-notify while already
        // feeding is COUNTED, not used to restart the clock: the old
        // "refresh the timestamp" rule meant any effect that re-applies
        // faster than the 12 s cap kept the feed alive for as long as the
        // player stood still (user, 2026-09-05: "the noise never ends" unless
        // they moved). The effect itself is the live signal now - see
        // PollWerewolfFeed.
        if (!isWerewolfFeeding) {
            isWerewolfFeeding = true;
            feedStartTp  = CameraEffectClock::Now();
            feedEffectId = a_effect;
            feedNotifies = 1;
            feedAnimDrivenSeen = false;
            feedLastStatusTp   = {};
            spdlog::debug("[FEED] werewolf feed latched (effect {:08X})", a_effect);
            return;
        }
        ++feedNotifies;
        if (a_effect != 0) feedEffectId = a_effect;
        if (feedNotifies <= 6)
            spdlog::debug("[FEED] re-notified while feeding (#{}, effect {:08X}) - clock NOT restarted",
                         feedNotifies, a_effect);
    }

    void StateResolver::PollWerewolfFeed(RE::PlayerCharacter* a_player)
    {
        if (!isWerewolfFeeding) return;
        const auto nowTp = CameraEffectClock::Now();
        const float elapsed = std::chrono::duration<float>(nowTp - feedStartTp).count();

        // The feed idle roots the werewolf over the corpse, so real
        // horizontal movement after the opening moment means it ended —
        // the same stillness-net shape the beast-form landings use. Leaving
        // beast form releases immediately, and a hard timeout covers a feed
        // interrupted by something that never moves the player (the vanilla
        // feed runs ~5s; 12s is safely past any modded variant).
        static RE::NiPoint3 sFeedPrevPos{};
        static bool         sFeedPosInit = false;
        static std::chrono::steady_clock::time_point sFeedLastTp{};
        if (elapsed < 0.05f) sFeedPosInit = false;   // fresh latch

        bool moving = false;
        if (a_player) {
            const RE::NiPoint3 p = a_player->GetPosition();
            if (sFeedPosInit) {
                const float dt = std::clamp(
                    std::chrono::duration<float>(nowTp - sFeedLastTp).count(),
                    1.0f / 240.0f, 0.05f);
                const float dx = p.x - sFeedPrevPos.x, dy = p.y - sFeedPrevPos.y;
                const float spd = std::sqrt(dx * dx + dy * dy) / dt;
                moving = spd > 90.0f && spd < 4000.0f;   // upper bound: cell-load teleport isn't "moving"
            }
            sFeedPrevPos = p;
            sFeedPosInit = true;
            sFeedLastTp  = nowTp;
        }

        // The feed is an idle animation, so the animation is the end signal
        // (2026-09-05; the effect turned out to be applied once and gone
        // within 0.5 s, and releasing on it ended the feed instantly).
        // bAnimationDriven is read every frame: once it has been seen true
        // during this feed, its falling edge releases. The idle-end graph
        // tags release too (NotifyWerewolfFeedGraphTag). A status line four
        // times a second records what the graph looked like, so if neither
        // signal fires the log still says which variable moved at the end.
        bool animDriven = false;
        bool isNPC      = false;
        std::int32_t iState = -1;
        if (a_player) {
            a_player->GetGraphVariableBool("bAnimationDriven", animDriven);
            a_player->GetGraphVariableBool("IsNPC", isNPC);
            a_player->GetGraphVariableInt("iState", iState);
        }
        const bool animDrivenEnded = feedAnimDrivenSeen && !animDriven && elapsed > 1.0f;
        if (animDriven) feedAnimDrivenSeen = true;
        if (std::chrono::duration<float>(nowTp - feedLastStatusTp).count() > 0.25f) {
            feedLastStatusTp = nowTp;
            static int sStatusLines = 0;
            if (sStatusLines < 160) {
                ++sStatusLines;
                spdlog::debug("[FEED] +{:.2f}s animDriven={} isNPC={} iState={} moving={} killMove={} sitSleep={}",
                             elapsed, animDriven, isNPC, iState, moving,
                             a_player ? a_player->IsInKillMove() : false,
                             a_player && a_player->AsActorState()
                                 ? static_cast<int>(a_player->AsActorState()->GetSitSleepState()) : -1);
            }
        }

        const bool leftForm = !isWerewolf;
        if (leftForm || animDrivenEnded || (elapsed > 1.0f && moving) || elapsed > 12.0f) {
            isWerewolfFeeding = false;
            spdlog::debug("[FEED] released after {:.2f}s ({}; notifies={})", elapsed,
                         leftForm ? "left beast form"
                         : animDrivenEnded ? "bAnimationDriven fell"
                         : (elapsed > 12.0f ? "timeout" : "moved"),
                         feedNotifies);
        }
    }

    void StateResolver::NotifyWerewolfFeedGraphTag(const char* a_tag)
    {
        if (!isWerewolfFeeding || !a_tag) return;
        const float elapsed = std::chrono::duration<float>(
            CameraEffectClock::Now() - feedStartTp).count();
        static int sTagLines = 0;
        if (sTagLines < 120) {
            ++sTagLines;
            spdlog::debug("[FEED] +{:.2f}s tag '{}'", elapsed, a_tag);
        }
        // The idle's own end tags. Guarded past the opening seconds so a
        // stray tag from the activation itself cannot end a feed that has
        // barely started (the 12:33 log ended one at 0.5 s on a bad signal).
        // 12:55 log, the feed's exit burst at +4.97 s: 'attackStop',
        // 'InitiateEnd', 'EndAnimatedCamera', 'InitiateStart*',
        // 'Collision_PitchEnd' all in one frame, bAnimationDriven falling
        // half a second later. 'EndAnimatedCamera' / 'attackStop' are the
        // earliest of those; 'InitiateEnd' also fires at the START (+0.05 s),
        // which the 2 s guard already excludes.
        const bool endTag = _stricmp(a_tag, "IdleStop") == 0 ||
                            _stricmp(a_tag, "IdleStopInstant") == 0 ||
                            _stricmp(a_tag, "IdleForceDefaultState") == 0 ||
                            _stricmp(a_tag, "EndAnimatedCamera") == 0 ||
                            _stricmp(a_tag, "attackStop") == 0 ||
                            _stricmp(a_tag, "InitiateEnd") == 0;
        if (endTag && elapsed > 2.0f) {
            isWerewolfFeeding = false;
            spdlog::debug("[FEED] released after {:.2f}s (idle end tag '{}'; notifies={})",
                         elapsed, a_tag, feedNotifies);
        }
    }

    void StateResolver::PollAttack(RE::PlayerCharacter* a_player)
    {
        bool attackingNow = false;
        a_player->GetGraphVariableBool("IsAttacking", attackingNow);

        // Power-attack flag from the engine's BGSAttackData on the player's
        // high process. This is the only signal that distinguishes power from
        // normal attacks across vanilla + MCO + BFCO (animation tags / graph
        // vars don't — see reference_attack_data_path). The engine sets it
        // synchronously at attack start; we read it live and OR it across the
        // swing (sticky) so a frame-late flag still registers.
        // Returns nullopt when attackData is absent (no current attack data) so
        // callers can distinguish "unknown" from "genuinely a normal attack".
        // This matters at swing end: the engine nulls high->attackData during
        // the recovery animation while IsAttacking is still briefly true. A
        // plain bool would read that null as "normal attack" and flip a power
        // swing to the normal-attack profile for a few frames before the linger
        // drops to Unsheathed — the "power attack -> normal attack -> unsheathed"
        // glitch. A real combo follow-up populates a fresh non-null attackData,
        // so power<->normal switching mid-combo still works.
        auto readPowerAttack = [a_player, this]() -> std::optional<bool> {
            auto& rt = a_player->GetActorRuntimeData();
            if (auto* process = rt.currentProcess) {
                if (auto* high = process->high) {
                    if (auto atkData = high->attackData) {
                        bool p = atkData->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
                        // Werewolf fallback ("werewolf power attack isn't
                        // detected", 2026-08-15): the beast race's attack-data
                        // entries apparently don't all carry kPowerAttack, so
                        // the EVENT NAME decides too — vanilla names its power
                        // attacks attackPowerStart* on every race, werewolves
                        // included. Human races keep the flag-only read (their
                        // framework graphs reuse event names loosely — see the
                        // graph-tag override below).
                        // VAMPIRE LORD JOINS THE NAME READ (2026-09-05). The
                        // 10:06-10:17 log: 24 "attack started" lines in
                        // VampireLordMelee, every one power=false, while the
                        // user was power attacking - the same flag gap the
                        // werewolf had. Same remedy, same race-gated scope.
                        if (isWerewolf || isVampireLord) {
                            const char* ev = atkData->event.c_str();
                            const bool evPower = ev && ContainsCI(ev, "power");
                            if (evPower && !p) p = true;
                            // Trace distinct beast power-attack events in Verbose Logging.
                            static std::string sWwpaLastEv;
                            if (spdlog::should_log(spdlog::level::debug) && ev && sWwpaLastEv != ev) {
                                sWwpaLastEv = ev;
                                spdlog::debug(
                                            "[WWPA] {} attackData ev=\"{}\" kPowerAttack={} -> power={}",
                                            isWerewolf ? "werewolf" : "vampire lord", ev,
                                            atkData->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack), p);
                            }
                        }
                        return p;
                    }
                    // Werewolf swing with NO attackData at all — the other
                    // way detection can fail; say so once.
                    if (isWerewolf) {
                        static bool sWwpaNullLogged = false;
                        if (!sWwpaNullLogged) {
                            sWwpaNullLogged = true;
                            spdlog::debug("[WWPA] werewolf attack with NULL attackData (flag path unavailable)");
                        }
                    }
                }
            }
            return std::nullopt;
        };

        // Power-attack early trigger. The "IsAttacking" graph variable is set
        // by the behaviour graph, and behaviour frameworks (MCO / BFCO / SkySA)
        // raise it at the ATTACK WINDOW — the hit-frame region — not at the
        // start of the animation. A power attack's wind-up is long, so waiting
        // on that flag put the whole power-attack camera + noise after the swing
        // had already played ("the power attack noise doesn't happen until the
        // attack has already happened"). high->attackData is populated by the
        // engine when the attack action is COMMITTED, ahead of the graph flag,
        // and it carries the power flag — so a present-and-power attackData is
        // itself proof that a power attack has begun. Normal attacks are left on
        // the graph flag: their wind-up is short enough that the flag isn't
        // perceptibly late, and this keeps the early path to the one case that
        // needed it. The engine nulls attackData during recovery, so this can't
        // hold the state open past the swing.
        // EXTENDED to normal attacks (2026-08-15, "first person normal
        // attack noise seems delayed"): attackData is populated for ANY
        // committed attack, and the 1p attack beat arms off the resolved
        // key flip — waiting for the graph's hit-frame IsAttacking put the
        // swing's noise perceptibly after the swing. Bash attack data is
        // excluded: bashes belong to Blocking, and their attackData would
        // otherwise force the Attack sub-state under a raised shield.
        bool attackCommitted = false;
        {
            auto& rtC = a_player->GetActorRuntimeData();
            if (auto* processC = rtC.currentProcess) {
                if (auto* highC = processC->high) {
                    if (auto atkDataC = highC->attackData) {
                        attackCommitted = !atkDataC->data.flags.any(
                            RE::AttackData::AttackFlag::kBashAttack);
                    }
                }
            }
        }
        if (attackCommitted && !attackingNow) {
            static bool sLoggedEarly = false;
            if (!sLoggedEarly) {
                sLoggedEarly = true;
                spdlog::debug("StateResolver: attack seen via attackData before IsAttacking "
                             "(early trigger active for this behaviour graph, power={})",
                             readPowerAttack().value_or(false));
            }
            attackingNow = true;
        }

        // --- Animation bracket (see the header comment on
        //     attackAnimBracketOpen) ---
        // The graph's own Start/Stop tags are the authority on how long the
        // attack ANIMATION runs. While the bracket is open the attack is
        // playing, no matter what the IsAttacking window says — that is what
        // makes the state span the wind-up and the recovery instead of just
        // the hit frames. The start COUNTER covers a swing whose entire
        // bracket opened and closed inside one frame.
        // Brackets only count once they have proven themselves (a complete
        // open→close cycle) and have not been withdrawn. Everywhere else in
        // this function `animSeen` false means "behave exactly as before".
        const bool animTrusted = attackAnimBracketProven.load(std::memory_order_relaxed) &&
                                 !attackAnimDistrust.load(std::memory_order_relaxed);
        const bool animSeen = animTrusted;
        bool       animOpen = animTrusted &&
                              attackAnimBracketOpen.load(std::memory_order_relaxed);

        // Always consume the counter (so an untrusted period can't bank a
        // huge edge for later), but only ACT on it once brackets are trusted.
        const std::uint32_t animStartCtr = attackAnimStartCounter.load(std::memory_order_relaxed);
        const bool animStartEdge = animTrusted && (animStartCtr != attackAnimStartSeen);
        attackAnimStartSeen      = animStartCtr;

        // Stuck-bracket watchdog. If a graph ever emits a start tag without a
        // matching stop, the Attack sub-state would pin forever. The clock
        // measures how long the engine has CONTINUOUSLY disagreed that an
        // attack is happening (no IsAttacking, no attackData) with no fresh
        // start tag — not merely how long the bracket has been open. A combo
        // can hold the bracket open for many seconds legitimately, and it
        // dips through brief IsAttacking gaps between swings; timing from the
        // open would let one of those gaps trip the watchdog mid-combo.
        constexpr int kAnimBracketWatchdogMs = 4000;
        const auto    nowTp = CameraEffectClock::Now();
        if (animOpen) {
            if (!attackAnimOpenObserved || attackingNow || animStartEdge) {
                attackAnimOpenObserved = true;
                attackAnimOpenedAt     = nowTp;   // engine still agrees — restart the clock
            } else {
                const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         nowTp - attackAnimOpenedAt).count();
                // (The 350ms "adaptive recovery release" lived here for
                // three failed attempts on 2026-08-31 — the quiet clock
                // could not be given a correct ruler. REPLACED by the
                // user's design: the attack cell's Duration slider bounds
                // the FOV hold too — see ResolveFirstPersonFovProfile.)
                if (quietMs >= kAnimBracketWatchdogMs) {
                    attackAnimBracketOpen.store(false, std::memory_order_relaxed);
                    animOpen = false;
                    attackAnimOpenObserved = false;
                    // Give the authority back. A graph that leaves a bracket
                    // hanging can't be relied on to close the next one
                    // either, and a permanently-late attack state is a far
                    // worse outcome than a slightly-early one. From here on
                    // this graph uses the plain IsAttacking + linger path,
                    // exactly as it did before the bracket existed.
                    attackAnimDistrust.store(true, std::memory_order_relaxed);
                    spdlog::warn("StateResolver: attack animation bracket open with no engine-side "
                                 "attack for {}ms — forced closed, and animation bracketing is now "
                                 "DISABLED for this graph (falling back to IsAttacking + linger)",
                                 quietMs);
                }
            }
        } else {
            attackAnimOpenObserved = false;
        }
        if (animOpen || animStartEdge) {
            static bool sLoggedAnimDriven = false;
            if (!sLoggedAnimDriven) {
                sLoggedAnimDriven = true;
                spdlog::debug("StateResolver: attack window is animation-bracketed for this graph "
                             "(Attack sub-state now spans the animation, not the IsAttacking window)");
            }
            attackingNow = true;
        }

        // Hybrid direction classifier: substring-match the BGSAttackData
        // event name (vanilla embeds Forward/Backward/Left/Right/InPlace),
        // falling back to ActorState1 movement flags when the event is
        // flattened (MCO_PowerAttackInitiate / BFCO_AttackWinStart /
        // SkySA_AttackWinStart). For MCO/BFCO movesets that flow direction
        // chooses anim via OAR conditions on the same input flags, so the
        // fallback agrees with what the moveset actually played. See
        // [[power-attack-direction-classification]].
        auto classifyPADir = [a_player]() -> PowerAttackDirection {
            auto containsIC = [](const char* hay, const char* needle) {
                if (!hay || !needle) return false;
                for (; *hay; ++hay) {
                    const char* h = hay;
                    const char* n = needle;
                    while (*h && *n &&
                           std::tolower(static_cast<unsigned char>(*h)) ==
                           std::tolower(static_cast<unsigned char>(*n))) {
                        ++h; ++n;
                    }
                    if (!*n) return true;
                }
                return false;
            };
            const char* evStr = "(none)";
            const char* via   = "default-InPlace";
            PowerAttackDirection result = PowerAttackDirection::InPlace;

            // 1) Event direction. Vanilla embeds the REAL direction in the event
            //    (attackPowerStartForward/Backward/Left/Right). MCO & BFCO collapse
            //    EVERY power attack -- directional ones included -- to the generic
            //    'attackPowerStartInPlace', so an "InPlace" event is NOT a
            //    trustworthy direction signal; only a real directional substring
            //    is. ([PADIR] proved it: MCO left/right PAs all logged
            //    ev='attackPowerStartInPlace' while the movement flags carried the
            //    true direction.) Track whether the event named a REAL direction
            //    -- only that is allowed to override the movement fallback below.
            PowerAttackDirection eventDir = PowerAttackDirection::InPlace;
            bool eventHasRealDir = false;
            auto& rt = a_player->GetActorRuntimeData();
            if (auto* process = rt.currentProcess) {
                if (auto* high = process->high) {
                    if (auto atkData = high->attackData) {
                        const char* ev = atkData->event.c_str();
                        evStr = ev ? ev : "(empty)";
                        if      (containsIC(ev, "Forward"))  { eventDir = PowerAttackDirection::Forward; eventHasRealDir = true; }
                        else if (containsIC(ev, "Backward")) { eventDir = PowerAttackDirection::Back;    eventHasRealDir = true; }
                        else if (containsIC(ev, "Left"))     { eventDir = PowerAttackDirection::Left;    eventHasRealDir = true; }
                        else if (containsIC(ev, "Right"))    { eventDir = PowerAttackDirection::Right;   eventHasRealDir = true; }
                        // "InPlace" is intentionally NOT a real direction (MCO uses
                        // it generically) -- fall through to the movement flags.
                    }
                }
            }

            // 2) Movement-intent direction (input-driven flags). Lateral
            //    (Left/Right) takes priority over Forward/Back: an MCO directional
            //    power attack is performed while running INTO the enemy, so the
            //    forward flag is usually ALSO set ([PADIR]: a left PA logged
            //    F1+L1). The lateral key is the one that selects the side moveset,
            //    so it must win the diagonal -- otherwise every PA-with-forward
            //    collapses to Forward and left/right never trigger.
            int fF = 0, fB = 0, fL = 0, fR = 0;
            PowerAttackDirection moveDir = PowerAttackDirection::InPlace;
            bool moveHasDir = false;
            if (auto* as = a_player->AsActorState()) {
                const auto& s1 = as->actorState1;
                fF = s1.movingForward; fB = s1.movingBack;
                fL = s1.movingLeft;    fR = s1.movingRight;
                if      (fL) { moveDir = PowerAttackDirection::Left;    moveHasDir = true; }
                else if (fR) { moveDir = PowerAttackDirection::Right;   moveHasDir = true; }
                else if (fF) { moveDir = PowerAttackDirection::Forward; moveHasDir = true; }
                else if (fB) { moveDir = PowerAttackDirection::Back;    moveHasDir = true; }
            }

            // 3) A real (vanilla) directional event wins. Otherwise the movement
            //    intent overrides the bogus/absent InPlace event (the MCO/BFCO
            //    case). No input at all -> genuine in-place.
            if (eventHasRealDir)  { result = eventDir; via = "event"; }
            else if (moveHasDir)  { result = moveDir;  via = "flag";  }
            else                  { result = PowerAttackDirection::InPlace; via = "default-InPlace"; }
            (void)evStr; (void)via;
            return result;
        };

        // (The [WWPA-HAND] attack-hand classifier lived here for one day —
        // removed 2026-08-30 with the werewolf PA variants. Its one run
        // documented the werewolf attack event names; see memory
        // werewolf-pa-variants before rebuilding anything hand-keyed.)

        // Exit-linger with session adaptation (same pattern as cast linger).
        // Base window handles the recovery gap between single swings.
        // Session window kicks in when a new swing starts during linger
        // (proving combo tempo) and covers the longer gap between slower
        // follow-up swings. Session resets on a linger commit.
        //
        // These two are the BLIND path — used only when the graph gives us
        // no attack bracket, where the linger has to stand in for the whole
        // unobserved recovery. When the bracket IS available the animation's
        // own end is known exactly, so the linger shrinks to a short bridge
        // whose only job is to keep a combo from flickering if a graph
        // closes and reopens the bracket between swings. Anything longer is
        // the attack camera outstaying the attack.
        constexpr int kAttackLingerBaseMs    = 150;
        constexpr int kAttackLingerSessionMs = 500;
        constexpr int kAttackLingerAnimMs    = 140;

        if (attackLingering) {
            if (attackingNow) {
                // New swing during linger — combo. Session activates so
                // the NEXT linger uses the longer window. isAttacking was
                // already true (we held it during the linger), so no
                // state change here — the consumer stays on Attack.
                attackLingering     = false;
                attackSessionActive = true;
                // A combo swing is a NEW swing: re-latch the modifiers that
                // name which variant it is, so the follow-up to a sprint
                // attack routes as the standing attack it actually is.
                attackSprintLatched = isSprinting;
                attackSneakLatched  = isSneaking;
                // Re-classify direction when the combo swing is a power
                // attack. Without this, two consecutive PAs in different
                // directions both use the FIRST PA's profile because the
                // rising-edge branch below never fires (isAttacking was
                // held true through linger) and the live-flip branch
                // further down only fires when isPowerAttack toggles.
                if (const auto powerNow = readPowerAttack().value_or(false); powerNow) {
                    powerAttackDir = classifyPADir();
                }
            } else {
                const int  lingerMs = animSeen ? kAttackLingerAnimMs
                                               : (attackSessionActive ? kAttackLingerSessionMs
                                                                      : kAttackLingerBaseMs);
                const auto elapsed  = CameraEffectClock::Now() - attackEndTime;
                const auto ms       = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (ms >= lingerMs) {
                    isAttacking         = false;
                    isPowerAttack       = false;
                    powerFlagFromGraph  = false;   // next session re-earns it
                    attackLingering     = false;
                    attackBeganDuringHowl = false;
                    const bool wasSession = attackSessionActive;
                    attackSessionActive = false;
                    spdlog::debug("StateResolver: attack ended (linger {}ms, anim-bracketed={}, session={})",
                                 ms, animSeen, wasSession);
                    stateChanged = true;
                }
            }
        } else {
            // After a howl-born attack was force-ended (see the shout-end
            // commit), the engine's IsAttacking can still be true through the
            // rest of the howl's recovery, and the rising edge below would
            // immediately restart the session we just ended. Swallow restarts
            // until the graph's own attack actually drops; a real swing can't
            // begin while the same graph session is still running, so nothing
            // legitimate is lost.
            if (howlAttackSwallow) {
                if (!attackingNow) howlAttackSwallow = false;
            } else if (attackingNow && !isAttacking) {
                isAttacking   = true;
                isPowerAttack = readPowerAttack().value_or(false);
                // The howl's own animation emits MeleeStart, so an attack that
                // begins while the werewolf is mid-shout is the HOWL's, not a
                // swing — remembered so the shout-end commit can end it with
                // the roar instead of letting its recovery outlive the linger.
                attackBeganDuringHowl = isWerewolf && isShouting;
                // Latch the variant modifiers for this swing. The engine
                // drops the sprint flag long before a sprint power attack's
                // animation finishes; holding the start-of-swing value keeps
                // the whole swing on one profile.
                attackSprintLatched = isSprinting;
                attackSneakLatched  = isSneaking;
                if (isPowerAttack) {
                    powerAttackDir = classifyPADir();
                }
                spdlog::debug("StateResolver: attack started (power={}, dir={}, sprint={}, sneak={})",
                             isPowerAttack, static_cast<int>(powerAttackDir),
                             attackSprintLatched, attackSneakLatched);
                stateChanged = true;
            } else if (!attackingNow && isAttacking) {
                // Start linger — hold isAttacking true until the window
                // elapses. Expiry handled by the branch above on the next
                // frame(s).
                attackEndTime   = CameraEffectClock::Now();
                attackLingering = true;
            }
        }

        // Track the CURRENT swing's power flag. Read it live every frame a
        // swing is active (attackingNow) so a normal swing following a power
        // swing — or vice versa, in a combo — switches profiles immediately
        // instead of latching. During the linger gap (attackingNow false) the
        // last value is held; it's cleared when the attack session fully ends
        // (the linger-expiry branch above). A latching OR was the old bug:
        // power stayed stuck until the player returned to melee idle.
        if (attackingNow) {
            // Only flip when attackData is present (a known value). A null read
            // (nullopt) means the engine has cleared attackData mid-swing — most
            // commonly at the end of a power attack's recovery — and must NOT be
            // treated as a normal attack, which would flash the normal-attack
            // profile before the linger drops to Unsheathed.
            // ...and only while the GRAPH has not already ruled on this swing.
            // Without that guard the two sources fight: the graph tag flips the
            // flag to false once per swing, and stale attackData flips it back
            // to true on every frame in between. The 17:01 log caught exactly
            // that — "power flag from graph tag -> false" eight times in a row
            // and never a "-> true", which can only happen if something kept
            // setting it true again behind us.
            if (const auto powerNow = readPowerAttack();
                !powerFlagFromGraph && powerNow && *powerNow != isPowerAttack) {
                isPowerAttack = *powerNow;
                if (isPowerAttack) {
                    // Combo flipped INTO a power attack: re-classify direction
                    // from the fresh attackData so the per-direction profile
                    // picker sees the new swing's intent.
                    powerAttackDir = classifyPADir();
                }
                stateChanged  = true;
            }
            // GRAPH TAGS OVERRULE attackData for the power flag.
            //
            // attackData is the engine's own answer and is normally right, but
            // it goes STALE inside a combo on animation-framework graphs. The
            // 16:55 log is unambiguous: one "attack started (power=true)", then
            // the graph fires SBF_PowerAttackStop + SBF_NormalAttackStart +
            // MCO_AttackInitiate for the follow-up swing, and attackData STILL
            // reported kPowerAttack (the early-trigger line fires again a
            // second later). So the live flip above never saw a false, the
            // power profile stuck for the rest of the combo, and a normal
            // attack after a power attack never switched.
            //
            // A start tag NAMES the swing that just began, which is exactly the
            // fact in question, so a fresh one wins. Counter-compared rather
            // than flag-polled so an announcement can never be missed by
            // landing on a frame this branch didn't run — and it degrades
            // cleanly: a graph that emits no start tags never bumps the
            // counter, and attackData keeps full authority as before.
            {
                const auto pc = attackAnimPowerCounter.load(std::memory_order_relaxed);
                if (pc != lastAttackAnimPowerCounter) {
                    lastAttackAnimPowerCounter = pc;
                    // A start tag names a NEW swing, so the variant modifiers
                    // belong to that swing and not the one before it.
                    attackSprintLatched = isSprinting;
                    attackSneakLatched  = isSneaking;
                    // WEREWOLF EXCEPTION (2026-08-30, caught on tape): the
                    // beast graph's start tags never say "power", so the hint
                    // flipped a correctly-detected power attack to FALSE 18ms
                    // after the rising edge ("attack started (power=true)" →
                    // "power flag from graph tag -> false", 14:02:32) and
                    // then owned the flag, disabling the live re-read too —
                    // every werewolf PA read as a normal claw. The overrule
                    // exists for HUMANOID framework graphs whose attackData
                    // goes stale mid-combo; this same log shows werewolf
                    // attackData flipping correctly swing by swing
                    // (attackStartLeft power=false → AttackStartLeftPower
                    // power=true), so attackData keeps authority in beast
                    // form and the graph hint stands down.
                    // Beast forms: the vampire lord joins the werewolf here
                    // (2026-09-05 11:33 log: attackData read
                    // "AttackPowerStart_Right" -> power=true, but this graph
                    // hint had already claimed the flag and never announced
                    // the power swing, so "attack started (power=true)" never
                    // came). Both beast graphs answer through attackData.
                    if (!isWerewolf && !isVampireLord) {
                        powerFlagFromGraph = true;   // this graph announces swings;
                                                     // it owns the flag from here
                        const bool hinted = attackAnimPowerHint.load(std::memory_order_relaxed);
                        if (hinted != isPowerAttack) {
                            isPowerAttack = hinted;
                            if (isPowerAttack) powerAttackDir = classifyPADir();
                            stateChanged = true;
                            static int sPwrLogs = 0;
                            if (sPwrLogs < 10) {
                                ++sPwrLogs;
                                spdlog::debug("StateResolver: power flag from graph tag -> {}",
                                             isPowerAttack);
                            }
                        }
                    }
                }
            }

            // Continuously re-classify direction while power-attacking. The
            // combo branch captures whatever attackData.event was at the
            // moment it fired — which is usually the PREVIOUS swing's event
            // because the engine hasn't yet overwritten it with the new
            // swing's string. The third-branch above only fires when
            // isPowerAttack flips, so for a power→power combo the dir would
            // otherwise stay frozen at the previous value. Per-frame catches
            // the update the moment attackData.event lands.
            if (isPowerAttack) {
                const auto liveDir = classifyPADir();
                // Only UPGRADE to a real direction; never downgrade a captured
                // direction back to InPlace. On MCO the movement flags clear the
                // instant the keys release -- well before the PA linger ends --
                // and classifyPADir then returns InPlace. The old unconditional
                // update wiped the real Left/Right the swing started with, so by
                // the time Quick Tune (which freezes the last resolved state) was
                // opened during the linger it saw InPlace. Holding the captured
                // direction keeps the directional profile live through the swing.
                if (liveDir != PowerAttackDirection::InPlace && liveDir != powerAttackDir) {
                    powerAttackDir = liveDir;
                    stateChanged   = true;
                }
            }
        }
    }

    void StateResolver::PollBlocking(RE::PlayerCharacter* a_player)
    {
        // Use actorState2.wantBlocking — the engine's input-driven block
        // desire flag — rather than the "IsBlocking" anim-graph variable
        // OR Actor::IsBlocking() (both trail the button release by the
        // block-exit animation, ~200-300ms, which the user sees as a
        // "delay" before the camera transition starts). wantBlocking flips
        // the same frame the block button is released. Same fix pattern as
        // PollSprint with actorState1.sprinting. Wards go through
        // PollHandCaster so they release instantly already.
        // The previous bash-attack override (BGSAttackData::kBashAttack) was
        // removed when the priority was demoted: bashes are attacks, and
        // attacks now win over Blocking in ResolveBlocking. PollAttack
        // already reads IsAttacking which fires on bashes, so the bash route
        // ends up at the weapon-state Attacking sub-state.
        bool blockingNow = false;
        if (auto* as = a_player->AsActorState()) {
            blockingNow = static_cast<bool>(as->actorState2.wantBlocking);
        }

        if (blockingNow != isBlocking) {
            isBlocking = blockingNow;
            spdlog::debug("StateResolver: blocking {}", blockingNow ? "started" : "ended");
            stateChanged = true;
        }
    }

    void StateResolver::PollTargetLock()
    {
        // Release-cause evidence, cached while the lock is alive. The
        // 2026-08-14 log showed TDM itself dropping the lock (rawLock=false)
        // twice, each ~9-10s after acquire and each within ~0.6s of a menu
        // close — but by the falling edge TDM has already forgotten its
        // target, so whether the TARGET caused it (died, was flung out of
        // lock range by the shout being tested, lost LOS) is unknowable
        // unless the target is captured while the lock still holds. Refresh
        // every poll, not just on acquire: TDM target-switch changes the
        // target with no lock edge.
        static RE::ActorHandle                       sLockTarget{};
        static std::chrono::steady_clock::time_point sLockAcquiredAt{};

        const bool lockedNow = TDMIntegration::GetSingleton().IsTargetLocked();
        if (lockedNow) sLockTarget = TDMIntegration::GetSingleton().GetCurrentTarget();
        if (lockedNow == isTargetLocked) return;

        const bool fallingEdge = isTargetLocked && !lockedNow;
        isTargetLocked = lockedNow;
        if (lockedNow) sLockAcquiredAt = CameraEffectClock::Now();
        {
            // Claim snapshot for lock-off freeze diagnosis: yaw/dm are OUR
            // TDM claims, rawLock is TDM's lock state ignoring the
            // dead-target override (true here on release = stale TDM lock).
            auto& tdmDiag = TDMIntegration::GetSingleton();
            spdlog::debug("StateResolver: target lock {} (yaw={} dm={} rawLock={})",
                         lockedNow ? "acquired" : "released",
                         tdmDiag.HasYawControl(),
                         tdmDiag.HasDirectionalMovementDisabled(),
                         tdmDiag.GetRawTargetLockState());
        }
        if (fallingEdge) {
            // [TLREL] one line per release: who was locked, what state they
            // were in at the drop, and what the UI was doing. Distinguishes
            // "TDM had a reason" (dead / far / ragdolling target) from
            // "the release correlates with menus for no target-side reason",
            // which is the open question behind the Tween-snap report.
            const float heldSec = std::chrono::duration<float>(
                CameraEffectClock::Now() - sLockAcquiredAt).count();
            const auto* ply   = RE::PlayerCharacter::GetSingleton();
            auto*       tgt   = sLockTarget ? sLockTarget.get().get() : nullptr;
            float       dist  = -1.0f;
            float       tHp   = std::numeric_limits<float>::quiet_NaN();
            const char* tName = "?";
            bool tDead = false, tRagdoll = false;
            if (tgt && ply) {
                dist = tgt->GetPosition().GetDistance(ply->GetPosition());
                if (const char* n = tgt->GetName(); n && n[0]) tName = n;
                tDead    = tgt->IsDead();
                tRagdoll = tgt->IsInRagdollState();
                // Health closes the one gap the first capture left open: a
                // release with rawLock=true and dead=false is still explained
                // if health<=0 (IsTargetLocked's killing-blow window — IsDead
                // lags the blow).
                if (auto* avo = tgt->AsActorValueOwner())
                    tHp = avo->GetActorValue(RE::ActorValue::kHealth);
            }
            auto*       ui        = RE::UI::GetSingleton();
            const auto* ctrlMap   = RE::ControlMap::GetSingleton();
            const bool  tweenOpen = ui && ui->IsMenuOpen(RE::TweenMenu::MENU_NAME);
            spdlog::debug("[TLREL] lock released after {:.1f}s | target=\"{}\" resolved={} "
                         "dead={} ragdoll={} hp={:.0f} dist={:.0f} | tween={} paused={} moveCtl={}",
                         heldSec, tName, tgt != nullptr, tDead, tRagdoll, tHp, dist,
                         tweenOpen, ui && ui->GameIsPaused(),
                         ctrlMap && ctrlMap->IsMovementControlsEnabled());
        }
        stateChanged = true;

        // On lock-OFF: cancel any active exit-lingers so the camera
        // releases the F&F / shout / roar / attack profile immediately
        // instead of riding out the remaining tail. The user's mental
        // model: "lock off ends combat focus" — keeping a tail running
        // after release feels like the camera is still reacting to a
        // fight that's over. PollHandCaster / PollAttack on the next
        // frame sync the cached values to live state once their lingering
        // flags are cleared. Shout state has no separate live source —
        // committing the end inline.
        if (fallingEdge) {
            castLingering         = false;
            castSessionActive     = false;
            shoutLingering        = false;
            shoutLagHolding       = false;
            isShouting            = false;
            activeShoutId         = std::nullopt;
            activeModShoutFormID  = 0;
            activeModShoutPlugin.clear();
            werewolfRoarScheduled = false;
            attackLingering       = false;
            attackSessionActive   = false;
            isAttacking           = false;
            attackBeganDuringHowl = false;
            howlAttackSwallow     = false;
        }
    }

    CameraSubState StateResolver::ResolveSubState() const
    {
        // Target lock is orthogonal to sub-state — see IsTargetLocked().
        // Sub-state resolution ignores lock so profiles for sprint/sneak/
        // attack/swim/shout can be composed with the target-lock axis.
        if (isShouting)       return CameraSubState::Shout;
        if (isSwimming)       return CameraSubState::Swimming;
        if (isAttacking)      return CameraSubState::Attack;
        if (isSprinting)      return CameraSubState::Sprint;
        if (isSneaking)       return CameraSubState::Sneak;
        return CameraSubState::None;
    }

    // ----- Priority stack: Transformation > Mount > Weapon > Sheathed -----

    bool StateResolver::ResolveTransformation()
    {
        if (isWerewolf) {
            state     = CameraState::Werewolf;
            // Werewolf supports Sprint, Swimming, Attack, and Shout
            // (which routes to the Roar profile in the controller).
            // Sneak doesn't apply — the beast can't crouch in vanilla —
            // so suppress that signal.
            auto sub = ResolveSubState();
            if (sub == CameraSubState::Sneak) {
                sub = CameraSubState::None;
            }
            subState  = sub;
            school    = MagicSchool::None;
            castType  = CastType::None;
            blockKind = BlockKind::None;
            return true;
        }
        // VampireLord is intentionally NOT handled here — see ResolveVampireLord
        // for its dedicated sub-state resolution.
        return false;
    }

    bool StateResolver::ResolveVampireLord()
    {
        if (!isVampireLord) return false;

        // Levitation state from the vanilla DLC1VampireLevitateStateGlobal
        // (0 = not VL, 1 = grounded, 2 = levitating). Cached on first read
        // (can't resolve before kDataLoaded; FormID varies across Dawnguard
        // versions, so match by EditorID). >= 1.5 tolerates float roundtrip.
        // Drives BOTH the Sheathed and the Sprint Ground/Levitating splits.
        {
            static RE::TESGlobal* sLevitateState = nullptr;
            static bool           sLevitateLookupTried = false;
            if (!sLevitateState && !sLevitateLookupTried) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    auto& globals = dh->GetFormArray<RE::TESGlobal>();
                    for (auto* g : globals) {
                        if (!g) continue;
                        const auto* edid = g->GetFormEditorID();
                        if (edid && std::strcmp(edid, "DLC1VampireLevitateStateGlobal") == 0) {
                            sLevitateState = g;
                            spdlog::info("StateResolver: VL levitate global resolved fid=0x{:X} init_value={:.2f}",
                                         g->GetFormID(), g->value);
                            break;
                        }
                    }
                    if (!sLevitateState) {
                        spdlog::warn("StateResolver: VL levitate global not found by EditorID — defaulting to ground");
                    }
                }
                sLevitateLookupTried = true;
            }
            isVampireLordLevitating = sLevitateState && sLevitateState->value >= 1.5f;
        }

        // VL sub-state resolution per user spec.
        //
        // The "hands sheathed" axis (WEAPON_STATE::kSheathed) is checked
        // FIRST. VL has two sheathed contexts — hovering with hands at sides
        // and grounded with hands at sides — both of which the user wants
        // mapped to the Sheathed profile, regardless of whether a spell is
        // selected. Only when WEAPON_STATE::kDrawn (claws out / spell raised
        // for casting) does the spell-vs-melee distinction matter.
        //
        // FLY_STATE::kHovering exists as a separate engine signal but isn't
        // needed here — hover and grounded both feed the same Sheathed slot
        // when WEAPON_STATE is sheathed.
        //
        // Resolution table:
        //   !isWeaponDrawn                           → VampireLordSheathed
        //   isWeaponDrawn + isSpellEquipped + cast:
        //       FAF / Ritual                         → VampireLordFireAndForget
        //       Concentration                        → VampireLordConcentration
        //       (caster active, no cast type)        → VampireLordMagic
        //   isWeaponDrawn + isSpellEquipped + idle   → VampireLordMagic
        //   isWeaponDrawn + !isSpellEquipped         → VampireLordMelee (claws out)
        if (!isWeaponDrawn) {
            state    = isVampireLordLevitating ? CameraState::VampireLordSheathedLevitating
                                               : CameraState::VampireLordSheathed;
            school   = MagicSchool::None;
            castType = CastType::None;
        } else if (isSpellEquipped) {
            if (isHandCasterActive) {
                switch (cachedCastType) {
                case CastType::FireAndForget:
                case CastType::Ritual:
                    state = CameraState::VampireLordFireAndForget;
                    break;
                case CastType::Concentration:
                    state = CameraState::VampireLordConcentration;
                    break;
                default:
                    state = CameraState::VampireLordMagic;
                    break;
                }
            } else {
                state = CameraState::VampireLordMagic;
            }
            // VL doesn't use school for profile selection (the 5 VL slots are
            // school-agnostic) but expose the active-cast school anyway for
            // diagnostic consistency with the regular Magic state.
            school   = isHandCasterActive ? cachedCastSchool : spellSchool;
            castType = isHandCasterActive ? cachedCastType   : CastType::None;
        } else {
            state    = CameraState::VampireLordMelee;
            school   = MagicSchool::None;
            castType = CastType::None;
        }
        // VL supports Sprint (grounded VL can sprint while levitation
        // is off). Sneak / shout / attack / swim don't apply — the lord
        // animation set has no crouch, no shout events fire in beast
        // forms, and water entry is gated. Match the Werewolf pattern:
        // use the standard sub-state resolver but suppress everything
        // except Sprint. Without this, the vampireLordSprint profile
        // (and its per-state camera noise binding) is unreachable.
        auto vlSub = ResolveSubState();
        // Sprint applies to any VL state (grounded levitation-off sprint).
        // Attack applies ONLY to the melee state (VL Melee Attacking / Power
        // Attacking profiles); VL magic/sheathed have no attack profile.
        // Everything else (sneak/shout/swim) doesn't apply to the lord.
        if (vlSub == CameraSubState::Sprint) {
            // keep
        } else if (vlSub == CameraSubState::Attack && state == CameraState::VampireLordMelee) {
            // keep — routes to vampireLordMeleeAttack / vampireLordMeleePowerAttack
        } else {
            vlSub = CameraSubState::None;
        }
        subState  = vlSub;
        blockKind = BlockKind::None;
        return true;
    }

    bool StateResolver::ResolveMount()
    {
        if (isDragonRiding) {
            state     = CameraState::DragonRiding;
            subState  = CameraSubState::None;
            school    = MagicSchool::None;
            castType  = CastType::None;
            blockKind = BlockKind::None;
            return true;
        }
        if (isHorseback) {
            state     = CameraState::Horseback;
            subState  = ResolveSubState();
            school    = MagicSchool::None;
            castType  = CastType::None;
            blockKind = BlockKind::None;
            return true;
        }
        return false;
    }

    bool StateResolver::ResolveBlocking()
    {
        // Priority cascade: this resolver runs after Transformation /
        // VampireLord / Mount but BEFORE Weapon. Even when blocking or
        // ward-casting we still yield to higher-priority signals:
        //
        //   1. Attacking → existing weapon-state Attack sub-state wins.
        //      Bashes are attacks (IsAttacking goes true on bash), so a
        //      bash from a blocked stance routes to weaponsMeleeAttack
        //      instead of any Blocking slot.
        //   2. Non-ward spell cast → Magic / Staves state wins. A spell
        //      cast from one hand while a ward is up in the other should
        //      route to the cast's profile, not to ward-blocking.
        //   3. Otherwise, if blocking OR ward-casting → Blocking state
        //      with one of 4 BlockKinds.
        //   4. Else → fall through to Weapon resolution.

        // Yield to attacks — let ResolveWeapon pick the Attack sub-state
        // for the equipped weapon. Bash attacks come through this path.
        if (isAttacking) return false;

        // Yield to non-ward spell casts. Staff fires also count — staff
        // routing in ResolveWeapon needs to win over ward routing.
        const bool nonWardSpellCasting = isHandCasterActive && !isCastingWard;
        const bool staffFiring = (cachedStaffCastType != CastType::None);
        if (nonWardSpellCasting || staffFiring) return false;

        // Need a weapon-drawn signal OR a ward cast to claim Blocking.
        // Pure ward casts have weapon drawn anyway (engine raises hands),
        // but use an explicit OR so we self-heal if the graph var lags.
        if (!isBlocking && !isCastingWard) return false;
        if (!isWeaponDrawn && !isCastingWard) return false;

        // Resolve the BlockKind. Priority within block:
        //   Shield > TwoHanded > OneHanded > Ward (blocking takes priority
        //   over a ward cast that happens at the same time, since the
        //   block animation drives the camera).
        BlockKind kind = BlockKind::None;
        if (isBlocking) {
            if (isShieldEquipped)        kind = BlockKind::Shield;
            else if (isTwoHandedEquipped) kind = BlockKind::TwoHanded;
            else if (isOneHandedEquipped) kind = BlockKind::OneHanded;
            else                          kind = BlockKind::OneHanded; // unarmed/fallback
        } else if (isCastingWard) {
            kind = BlockKind::Ward;
        }

        state     = CameraState::Blocking;
        blockKind = kind;
        // Sub-state composition: Sneak applies on top of any kind. Sprint
        // is only meaningful for Shield (vanilla allows shield-sprinting,
        // not weapon-block-sprinting). Other modifiers (Shout/Swimming)
        // don't compose with Blocking so we ignore ResolveSubState here.
        if (isSneaking) {
            subState = CameraSubState::Sneak;
        } else if (isSprinting && kind == BlockKind::Shield) {
            subState = CameraSubState::Sprint;
        } else {
            subState = CameraSubState::None;
        }
        school   = MagicSchool::None;
        castType = CastType::None;
        return true;
    }

    bool StateResolver::ResolveWeapon()
    {
        if (!isWeaponDrawn) return false;

        const auto sub = ResolveSubState();

        // Sub-priority within weapon:
        //   archery > mixed melee+staff > mixed staff+spell > staves > mixed melee+spell > magic > melee
        // Mixed loadouts default to Melee and only flip to the spell/staff
        // profile while the hand caster is in a non-idle state. Pure
        // staff/spell loadouts always read as their respective profile.
        if (isBowEquipped) {
            state    = CameraState::Bow;
            subState = sub;
            school   = MagicSchool::None;
            castType = CastType::None;
            return true;
        }
        if (isCrossbowEquipped) {
            // Archery is a blanket for bows AND crossbows — a crossbow resolves
            // to the Bow ("Archery") camera state so both share one profile set
            // (categories, TL, noise, shouts all key off CameraState::Bow). The
            // crossbow profiles/menu entries are retired. Projectile tracing and
            // the crosshair read the live weapon type, not this state, so bolt
            // physics still resolve correctly.
            state    = CameraState::Bow;
            subState = sub;
            school   = MagicSchool::None;
            castType = CastType::None;
            return true;
        }
        if (isStaffEquipped && isMeleeEquipped) {
            state    = isHandCasterActive ? CameraState::Staves : CameraState::Melee;
            subState = sub;
            school   = isHandCasterActive ? cachedStaffCastSchool : MagicSchool::None;
            castType = isHandCasterActive ? cachedStaffCastType   : CastType::None;
            return true;
        }
        if (isStaffEquipped && isSpellEquipped) {
            // Mixed staff+spell: spell-hand cast switches to Magic with
            // school+type. Staff fires stay in Staves with staff school+type.
            const bool spellCasting = (cachedCastType != CastType::None);
            state    = spellCasting ? CameraState::Magic : CameraState::Staves;
            subState = sub;
            if (spellCasting) {
                school   = cachedCastSchool;
                castType = cachedCastType;
            } else {
                school   = cachedStaffCastSchool;
                castType = cachedStaffCastType;
            }
            return true;
        }
        if (isStaffEquipped) {
            state    = CameraState::Staves;
            subState = sub;
            school   = cachedStaffCastSchool;
            castType = cachedStaffCastType;
            return true;
        }
        if (isSpellEquipped && isMeleeEquipped) {
            const bool casting = isHandCasterActive;
            state    = casting ? CameraState::Magic : CameraState::Melee;
            subState = sub;
            // When casting, use the live cast hand's school (handles dual-
            // wielding different schools — only the actively-casting hand's
            // school matters for profile selection). When idle, the school
            // doesn't matter (the Melee branch ignores it).
            school   = casting ? cachedCastSchool : MagicSchool::None;
            castType = casting ? cachedCastType   : CastType::None;
            return true;
        }
        if (isSpellEquipped) {
            state    = CameraState::Magic;
            subState = sub;
            // When casting, the active cast hand's school wins. When idle,
            // fall back to the cached primary school for the Magic base
            // profile (PickMagicProfile ignores school when castType is None
            // and returns weaponsMagic, but it's still a sane diagnostic).
            school   = isHandCasterActive ? cachedCastSchool : spellSchool;
            castType = isHandCasterActive ? cachedCastType   : CastType::None;
            return true;
        }
        if (isMeleeEquipped) {
            state    = CameraState::Melee;
            subState = sub;
            school   = MagicSchool::None;
            castType = CastType::None;
            return true;
        }
        return false;
    }

    void StateResolver::ResolveSheathed()
    {
        state     = CameraState::Sheathed;
        subState  = ResolveSubState();
        school    = MagicSchool::None;
        castType  = CastType::None;
        blockKind = BlockKind::None;
    }
}
