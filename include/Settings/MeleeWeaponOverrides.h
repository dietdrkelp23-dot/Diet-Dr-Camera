#pragma once

#include "Settings/CameraProfile.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
    class PlayerCharacter;
    class TESForm;
}

namespace DietDrCamera
{
    // Melee weapon classification. Used by the per-weapon-type override
    // system in Categories / Target Lock / Camera Noise. Unarmed counts as
    // a melee "weapon" for override purposes — when both hands are empty,
    // the Unarmed override applies.
    enum class MeleeWeaponType : std::uint8_t
    {
        Dagger     = 0,
        Sword      = 1,
        WarAxe     = 2,
        Mace       = 3,
        Greatsword = 4,
        Battleaxe  = 5,
        Warhammer  = 6,
        Unarmed    = 7,

        Count      = 8,
    };

    inline constexpr std::size_t kMeleeWeaponCount =
        static_cast<std::size_t>(MeleeWeaponType::Count);

    const char* MeleeWeaponTypeDisplayName(MeleeWeaponType t);
    const char* MeleeWeaponTypeTomlKey(MeleeWeaponType t);

    // Per-weapon-type override set attached to each base Melee profile
    // (e.g., weaponsMelee, weaponsMeleeSprint). Each of the 8 weapon types
    // has an independent enabled toggle and CameraProfile; unset weapons
    // fall back to the base profile. Mirrors the Shouts override pattern.
    // Mod-added weapon type slot (2026-08-15). Matched by the weapon's
    // WeapType* KEYWORD (the community convention for spears, halberds,
    // katanas, claws...), so any mod's weapon drives it with no support
    // list — the compatibility-first hook. Custom slots can't join the
    // indoor-variant registry (dynamic storage), so each carries its own
    // indoor twin inline.
    struct CustomMeleeSlot
    {
        std::string   keyword;          // WeapType* keyword editor ID
        bool          set = false;
        CameraProfile profile{};        // outdoor
        CameraProfile profileIndoor{};  // indoor twin
        [[nodiscard]] CameraProfile&       For(bool indoor)       { return indoor ? profileIndoor : profile; }
        [[nodiscard]] const CameraProfile& For(bool indoor) const { return indoor ? profileIndoor : profile; }
    };

    struct MeleeWeaponOverrides
    {
        std::array<CameraProfile, kMeleeWeaponCount> perWeapon{};
        std::array<bool, kMeleeWeaponCount>          perWeaponSet{};
        // Per-custom-type slots, keyed by keyword (order-independent; slots
        // for since-unbound types are simply never rendered or resolved).
        std::vector<CustomMeleeSlot>                 custom;
    };

    // Resolves the active profile for a melee state given the player's
    // currently-equipped weapon type. Returns base when the matching
    // override slot is unset. `customKeyword` is the matched mod-added
    // weapon-type keyword this frame (null/empty when none) — an enabled
    // custom slot outranks the vanilla-type slot (more specific);
    // `useIndoor` picks the custom slot's env twin (vanilla slots resolve
    // env downstream via the registry).
    const CameraProfile* ResolveMeleeOverride(
        const CameraProfile&        base,
        const MeleeWeaponOverrides& overrides,
        MeleeWeaponType             current,
        const char*                 customKeyword = nullptr,
        bool                        useIndoor = false);

    // Does the player's current melee weapon carry this keyword?
    bool PlayerMeleeWeaponHasKeyword(RE::PlayerCharacter* player, const char* keyword);

    // WeapType* keywords on the player's current melee weapon that do NOT
    // map to one of the 8 built-in types — the bindable "mod-added weapon
    // type" candidates the popup's Custom tab offers.
    std::vector<std::string> CollectPlayerCustomWeapTypeKeywords(RE::PlayerCharacter* player);

    // Classify the player's currently-equipped melee weapon. Right hand
    // wins when both hold a melee weapon; falls back to left if right is
    // empty or non-melee. Both hands empty = Unarmed. Non-melee right
    // (bow/crossbow/staff) with melee left returns the left's type.
    MeleeWeaponType ClassifyMeleeWeaponForm(const RE::TESForm* form);
    MeleeWeaponType ClassifyPlayerMeleeWeapon(RE::PlayerCharacter* player);
}
