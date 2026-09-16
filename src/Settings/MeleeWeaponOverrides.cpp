#include "PCH.h"
#include "Settings/MeleeWeaponOverrides.h"

#include <RE/B/BGSKeyword.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectWEAP.h>

#include <cstring>

namespace DietDrCamera
{
    const char* MeleeWeaponTypeDisplayName(MeleeWeaponType t)
    {
        switch (t) {
        case MeleeWeaponType::Dagger:     return "Dagger";
        case MeleeWeaponType::Sword:      return "Sword";
        case MeleeWeaponType::WarAxe:     return "War Axe";
        case MeleeWeaponType::Mace:       return "Mace";
        case MeleeWeaponType::Greatsword: return "Greatsword";
        case MeleeWeaponType::Battleaxe:  return "Battleaxe";
        case MeleeWeaponType::Warhammer:  return "Warhammer";
        case MeleeWeaponType::Unarmed:    return "Unarmed";
        default:                          return "?";
        }
    }

    const char* MeleeWeaponTypeTomlKey(MeleeWeaponType t)
    {
        switch (t) {
        case MeleeWeaponType::Dagger:     return "dagger";
        case MeleeWeaponType::Sword:      return "sword";
        case MeleeWeaponType::WarAxe:     return "war_axe";
        case MeleeWeaponType::Mace:       return "mace";
        case MeleeWeaponType::Greatsword: return "greatsword";
        case MeleeWeaponType::Battleaxe:  return "battleaxe";
        case MeleeWeaponType::Warhammer:  return "warhammer";
        case MeleeWeaponType::Unarmed:    return "unarmed";
        default:                          return "unknown";
        }
    }

    const CameraProfile* ResolveMeleeOverride(
        const CameraProfile&        base,
        const MeleeWeaponOverrides& overrides,
        MeleeWeaponType             current,
        const char*                 customKeyword,
        bool                        useIndoor)
    {
        // A matched mod-added weapon type is more specific than the engine
        // WEAPON_TYPE bucket the weapon also falls into, so it wins.
        if (customKeyword && customKeyword[0]) {
            for (const auto& cs : overrides.custom) {
                if (cs.set && cs.keyword == customKeyword) {
                    return &cs.For(useIndoor);
                }
            }
        }
        const std::size_t i = static_cast<std::size_t>(current);
        if (i < kMeleeWeaponCount && overrides.perWeaponSet[i]) {
            return &overrides.perWeapon[i];
        }
        return &base;
    }

    MeleeWeaponType ClassifyMeleeWeaponForm(const RE::TESForm* form)
    {
        if (!form) return MeleeWeaponType::Unarmed;
        auto* weap = form->As<RE::TESObjectWEAP>();
        if (!weap) return MeleeWeaponType::Unarmed;
        using WT = RE::WEAPON_TYPE;
        switch (weap->GetWeaponType()) {
        case WT::kHandToHandMelee: return MeleeWeaponType::Unarmed;
        case WT::kOneHandSword:    return MeleeWeaponType::Sword;
        case WT::kOneHandDagger:   return MeleeWeaponType::Dagger;
        case WT::kOneHandAxe:      return MeleeWeaponType::WarAxe;
        case WT::kOneHandMace:     return MeleeWeaponType::Mace;
        case WT::kTwoHandSword:    return MeleeWeaponType::Greatsword;
        case WT::kTwoHandAxe:
            // Engine's WEAPON_TYPE doesn't distinguish battleaxe from
            // warhammer — both report kTwoHandAxe. The form-level keyword
            // is the canonical split (WeapTypeBattleaxe vs WeapTypeWarhammer).
            if (weap->HasKeywordString("WeapTypeWarhammer")) {
                return MeleeWeaponType::Warhammer;
            }
            return MeleeWeaponType::Battleaxe;
        default:
            // Bows / crossbows / staves aren't melee — treat as unarmed
            // for classification purposes (the caller shouldn't even be
            // asking when the state isn't Melee, but be defensive).
            return MeleeWeaponType::Unarmed;
        }
    }

    // The weapon FORM the classifier's hand-preference rules land on —
    // shared by the type classifier and the keyword paths below so all
    // three always describe the same weapon.
    static const RE::TESObjectWEAP* CurrentPlayerMeleeWeapon(RE::PlayerCharacter* player)
    {
        if (!player) return nullptr;
        auto isMelee = [](const RE::TESForm* f) {
            return f && ClassifyMeleeWeaponForm(f) != MeleeWeaponType::Unarmed;
        };
        const RE::TESForm* right = player->GetEquippedObject(false);
        const RE::TESForm* left  = player->GetEquippedObject(true);
        const RE::TESForm* pick  = nullptr;
        if (isMelee(right))     pick = right;
        else if (isMelee(left)) pick = left;
        // True unarmed / hand-to-hand "weapons" classify as Unarmed above,
        // so pick stays null for them — no keywords to read anyway.
        return pick ? pick->As<RE::TESObjectWEAP>() : nullptr;
    }

    bool PlayerMeleeWeaponHasKeyword(RE::PlayerCharacter* player, const char* keyword)
    {
        if (!keyword || !keyword[0]) return false;
        const auto* weap = CurrentPlayerMeleeWeapon(player);
        return weap && weap->HasKeywordString(keyword);
    }

    std::vector<std::string> CollectPlayerCustomWeapTypeKeywords(RE::PlayerCharacter* player)
    {
        std::vector<std::string> out;
        const auto* weap = CurrentPlayerMeleeWeapon(player);
        if (!weap) return out;
        // Keywords the 8 built-in types already cover — not "custom".
        static constexpr const char* kBuiltin[] = {
            "WeapTypeSword", "WeapTypeDagger", "WeapTypeWarAxe", "WeapTypeMace",
            "WeapTypeGreatsword", "WeapTypeBattleaxe", "WeapTypeWarhammer",
            "WeapTypeBow", "WeapTypeStaff",
        };
        for (std::uint32_t i = 0; i < weap->numKeywords; ++i) {
            const auto* kw = weap->keywords ? weap->keywords[i] : nullptr;
            if (!kw) continue;
            const char* ed = kw->GetFormEditorID();
            if (!ed || std::strncmp(ed, "WeapType", 8) != 0) continue;
            bool builtin = false;
            for (const char* b : kBuiltin)
                if (std::strcmp(ed, b) == 0) { builtin = true; break; }
            if (builtin) continue;
            out.emplace_back(ed);
        }
        return out;
    }

    MeleeWeaponType ClassifyPlayerMeleeWeapon(RE::PlayerCharacter* player)
    {
        if (!player) return MeleeWeaponType::Unarmed;
        auto* right = player->GetEquippedObject(false);
        auto* left  = player->GetEquippedObject(true);
        // Prefer right hand. If it's a melee weapon (any non-Unarmed result),
        // use it. Otherwise check left. Both empty → Unarmed.
        if (right) {
            const auto t = ClassifyMeleeWeaponForm(right);
            // Unarmed result from a non-null right form means right is a
            // non-melee weapon (bow/staff/etc) — try left.
            if (t != MeleeWeaponType::Unarmed) return t;
            // Fall through to check left in case the player has a 1H melee
            // in the off-hand with a non-melee in the main hand.
            if (left) {
                const auto lt = ClassifyMeleeWeaponForm(left);
                if (lt != MeleeWeaponType::Unarmed) return lt;
            }
            // Right form exists but is non-melee, and either no left or
            // left is also non-melee. The engine wouldn't put us in the
            // Melee camera state in this case, so return Unarmed as a
            // safe default.
            return MeleeWeaponType::Unarmed;
        }
        if (left) {
            return ClassifyMeleeWeaponForm(left);
        }
        return MeleeWeaponType::Unarmed;
    }
}
