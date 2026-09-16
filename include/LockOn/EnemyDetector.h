#pragma once

#include <array>
#include <vector>

namespace DietDrCamera
{
    enum class EnemyType : std::uint8_t
    {
        None       = 0,
        Dragons    = 1,
        Giants     = 2,
        Mammoths   = 3,
        Centurions = 4,
        Lurkers    = 5,
    };

    inline constexpr std::size_t kEnemyTypeCount = 5;

    // Curated creature-type keywords offered as broad "bind by type" groups in
    // the custom enemy override UI. editorID is resolved at runtime via
    // LookupByEditorID<BGSKeyword>; friendly is the menu label. Order = display
    // order. Add entries here to expand the type list.
    struct CreatureKeywordDef { const char* editorID; const char* friendly; };
    inline constexpr CreatureKeywordDef kCreatureKeywords[] = {
        { "ActorTypeNPC",      "Humanoids"          },
        { "ActorTypeUndead",   "Undead"             },
        { "ActorTypeDwarven",  "Dwarven Automatons" },
        { "ActorTypeAnimal",   "Animals"            },
        { "ActorTypeDaedra",   "Daedra"             },
        { "ActorTypeDragon",   "Dragons"            },
        { "ActorTypeGiant",    "Giants"             },
        { "ActorTypeTroll",    "Trolls"             },
        { "ActorTypeFalmer",   "Falmer"             },
        // ActorTypeCreature intentionally omitted: nearly every non-humanoid
        // carries it, so it's too broad to be a useful group (a wolf would show
        // both "All Animals" and "All Creatures"). Use the specific type instead.
    };

    [[nodiscard]] inline std::size_t EnemyTypeIndex(EnemyType t)
    {
        return static_cast<std::size_t>(t) - 1;
    }

    // Caches keyword forms + race lists at SKSE kDataLoaded so per-frame
    // ClassifyTarget() is just a couple of pointer comparisons. Keyword-
    // tagged categories (Dragons, Giants) match anything bearing the
    // canonical Bethesda creature-type keyword; substring categories
    // (Mammoth, DwarvenCenturion, Lurker) match by race editor ID so all
    // vanilla variants AND most modded variants are captured.
    class EnemyDetector
    {
    public:
        [[nodiscard]] static EnemyDetector& GetSingleton();

        // Build the cached keyword + race-list lookup tables. Safe to call
        // before TESDataHandler is ready (no-op until forms are available).
        // Idempotent — re-running re-scans, used after a save load too.
        void Init();

        // Classify a locked TDM target. Returns EnemyType::None when the
        // target doesn't match any of the five tracked categories or when
        // the actor pointer is null.
        [[nodiscard]] EnemyType Classify(RE::Actor* actor) const;

    private:
        EnemyDetector() = default;

        RE::BGSKeyword*              dragonKeyword  = nullptr;
        RE::BGSKeyword*              giantKeyword   = nullptr;
        std::vector<RE::TESRace*>    mammothRaces;
        std::vector<RE::TESRace*>    centurionRaces;
        std::vector<RE::TESRace*>    lurkerRaces;
        bool                         initialized    = false;
    };
}
