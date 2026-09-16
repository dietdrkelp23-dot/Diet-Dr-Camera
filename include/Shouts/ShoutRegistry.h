#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace RE { class TESShout; }

namespace DietDrCamera
{
    // 27 player-obtainable shouts across Skyrim + Dawnguard + Dragonborn.
    // Enum values are the array indices into SettingsManager::shoutProfiles
    // and shoutProfilesSneak. Declared in alphabetical order so iterating
    // 0..Count-1 naturally gives alphabetical rendering.
    enum class ShoutId : int {
        AnimalAllegiance = 0,
        AuraWhisper,
        BattleFury,
        BecomeEthereal,
        BendWill,
        CallDragon,
        CallOfValor,
        ClearSkies,
        Cyclone,
        Disarm,
        Dismay,
        DragonAspect,
        Dragonrend,
        DrainVitality,
        ElementalFury,
        FireBreath,
        FrostBreath,
        IceForm,
        KynesPeace,
        MarkedForDeath,
        SlowTime,
        SoulTear,
        StormCall,
        SummonDurnehviir,
        ThrowVoice,
        UnrelentingForce,
        WhirlwindSprint,
        Count,
    };

    constexpr std::size_t kShoutCount = static_cast<std::size_t>(ShoutId::Count);

    struct ShoutEntry {
        ShoutId         id;
        const char*     displayName;   // UI label, e.g. "Unrelenting Force"
        const char*     tomlKey;       // TOML subkey, e.g. "unrelenting_force"
        const char*     esmName;       // plugin that defines the shout
        std::uint32_t   baseFormId;    // FormID within the esm (no load-order byte)
    };

    // Full table — see ShoutRegistry.cpp for values. Indexed by ShoutId.
    extern const std::array<ShoutEntry, kShoutCount> kShouts;

    // Populated at plugin data-load time by ShoutRegistry::Init from the
    // TESDataHandler. Entries may be null if the containing esm isn't loaded
    // (e.g. no Dawnguard/Dragonborn); those shouts simply never match and
    // their profiles never trigger. Indexed by ShoutId.
    class ShoutRegistry
    {
    public:
        static ShoutRegistry& GetSingleton();

        // Look up every shout via TESDataHandler and cache the resolved
        // TESShout* pointers. Logs resolution status per-entry. Safe to
        // call multiple times.
        void Init();

        // Reverse lookup: given the currently-equipped shout pointer,
        // return its ShoutId if it matches a cached entry. Nullopt for
        // unknown/mod-added shouts.
        [[nodiscard]] std::optional<ShoutId> Find(const RE::TESShout* form) const;

        // Spell-side lookups for NPC shouts: a shout FIRES as one of its
        // per-word variation SPELLS, and TESSpellCastEvent carries that
        // spell's FormID for every actor. IsShoutSpell answers "was this cast
        // a shout at all" (any TESShout's variation, known or mod-added);
        // FindBySpell resolves it to a ShoutId when the owning shout is one
        // of the named 27.
        [[nodiscard]] bool                   IsShoutSpell(std::uint32_t a_spell) const;
        [[nodiscard]] std::optional<ShoutId> FindBySpell(std::uint32_t a_spell) const;
        // Null for a spell shared by different shouts; the actor's current
        // shout remains authoritative when that identity is available.
        [[nodiscard]] RE::TESShout* FindFormBySpell(std::uint32_t a_spell) const;

    private:
        ShoutRegistry() = default;

        // Multiple TESShout forms can share the same FullName — Skyrim
        // defines per-word variants (e.g. "Unrelenting Force" at each
        // learning tier), and the engine may hand any of them back in a
        // kVoiceCast event. The map catches every variant so no matter
        // which form the engine passes, we resolve to the same ShoutId.
        std::unordered_map<const RE::TESShout*, ShoutId> formToId;
        std::unordered_map<std::uint32_t, ShoutId>       spellToId;      // named shouts only
        std::unordered_map<std::uint32_t, RE::TESShout*> spellToForm;
        std::unordered_set<std::uint32_t>                allShoutSpells; // every shout's variations
    };
}
