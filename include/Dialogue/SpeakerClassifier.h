#pragma once

#include "Settings/DialogueLook.h"

namespace RE
{
    class Actor;
    class TESObjectREFR;
}

namespace DietDrCamera::SpeakerClassifier
{
    // Classifies a dialogue speaker into one of the five DialogueCategory
    // buckets. Priority order: SpecificNPC > Dragons > Creatures > Indoors/
    // Outdoors. The "specific NPC" check scans the configured SpecificNPC
    // bucket for a matching (plugin, localID) entry — if found, that
    // category wins regardless of the actor's race or cell.
    //
    // Takes a TESObjectREFR, not an Actor: talking activators are legitimate
    // dialogue speakers (Statue of Mara, the Aspect of Hircine) and an
    // Actor-only signature silently dropped every one of them — including
    // out of the Specific NPCs binding UI (user report 2026-08-20).
    DialogueCategory Classify(RE::TESObjectREFR* speaker);
}
