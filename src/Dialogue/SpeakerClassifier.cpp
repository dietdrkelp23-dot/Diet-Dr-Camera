#include "PCH.h"
#include "Dialogue/SpeakerClassifier.h"

#include "Settings/SettingsManager.h"

#include <RE/A/Actor.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESRace.h>

namespace DietDrCamera::SpeakerClassifier
{
    namespace
    {
        // Returns true if the actor matches a SpecificNPC look's
        // (plugin, localID) pair. Resolves the configured local FormID
        // through TESDataHandler so load-order changes (added/removed
        // plugins) don't break the binding.
        bool MatchesSpecificNpcEntry(RE::TESObjectREFR* speaker, const DialogueLook& look)
        {
            if (look.targetFormID == 0) return false;
            if (!speaker) return false;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return false;
            // The configured target_form_id is the LOCAL form id; resolve
            // it relative to the configured plugin to get the engine's
            // current effective FormID for that record. Compare against
            // the speaker's FormID.
            const auto* pluginCStr = look.targetPluginName.empty()
                ? "Skyrim.esm"
                : look.targetPluginName.c_str();
            // LookupFormID expects (localFormID, modName); returns the
            // resolved global FormID, or 0 if the plugin isn't loaded.
            // The stored ID is the BASE form's FormID (that's what Bind
            // Current Speaker records) — a TESNPC for actors, a TESObjectACTI
            // for talking activators — so compare against the speaker's base
            // object, not its REFR FormID.
            const std::uint32_t resolved = dh->LookupFormID(look.targetFormID, pluginCStr);
            if (resolved == 0) return false;
            auto* base = speaker->GetBaseObject();
            if (!base) return false;
            return base->GetFormID() == resolved;
        }
    }

    DialogueCategory Classify(RE::TESObjectREFR* speaker)
    {
        if (!speaker) return DialogueCategory::Outdoors;

        // 1. SpecificNPC: scan that bucket for a FormID match. We check
        // both POVs because the user might have assigned the NPC in either
        // 3p or 1p — the assignment is conceptually about the NPC, not
        // about the active POV.
        const auto& s = SettingsManager::GetSingleton();
        for (auto pov : { DialoguePOV::ThirdPerson, DialoguePOV::FirstPerson }) {
            const auto idx = DialogueBucketIndex(DialogueCategory::SpecificNPC, pov);
            const auto& bucket = s.dialogueBuckets[idx];
            for (const auto& look : bucket.looks) {
                if (MatchesSpecificNpcEntry(speaker, look)) {
                    return DialogueCategory::SpecificNPC;
                }
            }
        }

        // 2. Race-based: dragon > creature > NPC (humanoid). Skyrim's
        // race records carry ActorType keywords that classify them
        // regardless of which mod added them, so this is robust to
        // mod-added races and DLC content.
        // Race axis only exists for Actors — a talking activator has no race
        // and falls straight through to the cell check below.
        auto* speakerActor = speaker->As<RE::Actor>();
        if (auto* race = speakerActor ? speakerActor->GetRace() : nullptr) {
            if (race->HasKeywordString("ActorTypeDragon")) {
                return DialogueCategory::Dragons;
            }
            if (race->HasKeywordString("ActorTypeCreature")) {
                return DialogueCategory::Creatures;
            }
            // Some races flagged ActorTypeAnimal (wolves, sabre cats) lack
            // the generic Creature keyword. Treat them as creatures too.
            if (race->HasKeywordString("ActorTypeAnimal")) {
                return DialogueCategory::Creatures;
            }
            // Note: undead (draugr, vampires) carry ActorTypeNPC, so they
            // fall through to Indoors/Outdoors classification — same as
            // normal humanoid NPCs. If users want to single them out,
            // they can use the SpecificNPC bucket per-record.
        }

        // 3. Cell-based: indoor vs outdoor.
        if (auto* cell = speaker->GetParentCell()) {
            if (cell->IsInteriorCell()) {
                return DialogueCategory::Indoors;
            }
        }

        return DialogueCategory::Outdoors;
    }
}
