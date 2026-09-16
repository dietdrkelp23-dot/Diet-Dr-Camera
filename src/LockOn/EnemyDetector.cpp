#include "PCH.h"
#include "LockOn/EnemyDetector.h"
#include <string_view>

namespace DietDrCamera
{
    namespace {
        // Substring matchers. Editor IDs are case-sensitive in CK convention,
        // and every race we want to catch uses the exact casing here:
        //   Mammoths   — vanilla "MammothRace", modded "FemaleMammothRace" etc.
        //   Centurions — vanilla "DwarvenCenturionRace" + Master/Forgemaster
        //                variants. Tightened from "Centurion" so a Roman-
        //                themed mod adding "RomanCenturionRace" wouldn't be
        //                misclassified as a Dwemer automaton.
        //   Lurkers    — DLC2 "DLC2LurkerRace" + "DLC2LurkerBossRace".
        constexpr std::string_view kMammothNeedle   = "Mammoth";
        constexpr std::string_view kCenturionNeedle = "DwarvenCenturion";
        constexpr std::string_view kLurkerNeedle    = "Lurker";

        bool ContainsCI(std::string_view haystack, std::string_view needle)
        {
            // Editor IDs have stable casing in vanilla Bethesda data, so a
            // case-sensitive find is correct AND faster. If a modder ships
            // an off-case variant ("mammothrace"), they'd be the rare
            // exception — escalate to a CI search if we ever see one in the
            // wild.
            return haystack.find(needle) != std::string_view::npos;
        }
    }

    EnemyDetector& EnemyDetector::GetSingleton()
    {
        static EnemyDetector instance;
        return instance;
    }

    void EnemyDetector::Init()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::warn("EnemyDetector::Init: TESDataHandler unavailable, skipping");
            return;
        }

        // Reset state in case Init runs twice (e.g. after a save load).
        dragonKeyword = nullptr;
        giantKeyword  = nullptr;
        mammothRaces.clear();
        centurionRaces.clear();
        lurkerRaces.clear();

        // Keyword-based detection — the canonical Bethesda creature-type
        // keywords. Every vanilla dragon and giant inherits these via race,
        // and properly-tagged modded variants do too.
        dragonKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeDragon");
        giantKeyword  = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeGiant");

        // Race-substring detection — walk the loaded race table once.
        const auto& races = dh->GetFormArray<RE::TESRace>();
        for (auto* race : races) {
            if (!race) continue;
            const char* edidPtr = race->GetFormEditorID();
            if (!edidPtr || edidPtr[0] == '\0') continue;
            std::string_view edid{edidPtr};
            if (ContainsCI(edid, kMammothNeedle))   mammothRaces.push_back(race);
            if (ContainsCI(edid, kCenturionNeedle)) centurionRaces.push_back(race);
            if (ContainsCI(edid, kLurkerNeedle))    lurkerRaces.push_back(race);
        }

        spdlog::info("EnemyDetector: dragonKW={} giantKW={} mammothRaces={} centurionRaces={} lurkerRaces={}",
                     dragonKeyword ? "found" : "MISSING",
                     giantKeyword  ? "found" : "MISSING",
                     mammothRaces.size(),
                     centurionRaces.size(),
                     lurkerRaces.size());

        initialized = true;
    }

    EnemyType EnemyDetector::Classify(RE::Actor* actor) const
    {
        if (!actor || !initialized) return EnemyType::None;

        // Keyword categories first — cheap and stable.
        if (dragonKeyword && actor->HasKeyword(dragonKeyword)) return EnemyType::Dragons;
        if (giantKeyword  && actor->HasKeyword(giantKeyword))  return EnemyType::Giants;

        // Race-pointer compare against the cached substring-match lists.
        auto* race = actor->GetRace();
        if (!race) return EnemyType::None;
        for (auto* r : mammothRaces)   { if (r == race) return EnemyType::Mammoths;   }
        for (auto* r : centurionRaces) { if (r == race) return EnemyType::Centurions; }
        for (auto* r : lurkerRaces)    { if (r == race) return EnemyType::Lurkers;    }
        return EnemyType::None;
    }
}
