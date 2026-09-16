#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Settings/CameraProfile.h"

namespace DietDrCamera
{
    // Categories for dialogue camera framing. Priority order at runtime is
    // Horseback (while mounted) > SpecificNPC > Dragons > Creatures >
    // Indoors/Outdoors. Indoors/Outdoors is the catch-all for untagged speakers.
    enum class DialogueCategory : std::uint8_t
    {
        Outdoors    = 0,
        Indoors     = 1,
        Dragons     = 2,
        Creatures   = 3,
        SpecificNPC = 4,
        // APPENDED 2026-08-24. The tab sits between First Person and Specific
        // NPCs in the menu, but the ENUM VALUE goes on the end — these indices
        // are persisted, and renumbering would silently reinterpret every look
        // in every existing preset. UI order and storage order are allowed to
        // disagree; storage order is the contract.
        Horseback   = 5,

        Count = 6,
    };

    enum class DialoguePOV : std::uint8_t
    {
        ThirdPerson = 0,
        FirstPerson = 1,

        Count = 2,
    };

    // Outdoor / Indoor split — applies to Dragons, Creatures, and
    // SpecificNPC buckets (their UI exposes an Outdoor/Indoor toggle).
    // The intrinsic-env categories (Outdoors, Indoors) ignore this axis
    // at runtime and only ever use Outdoor's slot in the bucket array.
    enum class DialogueEnv : std::uint8_t
    {
        Outdoor = 0,
        Indoor  = 1,

        Count = 2,
    };

    // A named camera framing within a bucket. SpecificNPC bucket entries
    // additionally store the target FormID + plugin name so the binding
    // survives load-order changes (engine-internal FormID changes when
    // plugins are added/removed; the (plugin, localID) pair is stable).
    struct DialogueLook
    {
        std::string   name;
        CameraProfile profile;

        // Stable identity for this look, unique across every bucket, assigned
        // on creation and preserved through save/load (2026-08-20, added for
        // per-place dialogue overrides). A location's override has to point at
        // ONE look, and neither of the obvious keys works: the list index moves
        // whenever a look is added or deleted, and the name is user-editable
        // and defaults to "Preset 1" in twenty different buckets. 0 means "not
        // assigned yet" — every load and every Dialogue render pass fills those
        // in via SettingsManager::AssignDialogueLookUids(), so a preset written
        // before this existed picks up IDs on first open and keeps them.
        std::uint32_t uid = 0;

        // SpecificNPC bucket only — 0 / empty for other buckets.
        std::uint32_t targetFormID        = 0;
        std::string   targetPluginName;
        std::string   targetDisplayName;

        bool operator==(const DialogueLook&) const = default;
    };

    // A list of looks for one (category, POV) combination plus the active
    // index and the random-on-entry toggle. activeIndex == -1 means the
    // bucket is empty and the resolver should fall through to the next
    // broader bucket.
    struct DialogueBucket
    {
        std::vector<DialogueLook> looks;
        int                       activeIndex   = -1;
        bool                      randomEnabled = false;
    };

    // 5 categories × 2 POVs × 2 envs = 20 buckets, indexed flat. The
    // env axis is only meaningful for Dragons / Creatures / SpecificNPC
    // (which expose an Outdoor/Indoor toggle in the UI). Outdoors and
    // Indoors categories ignore env at runtime and always use the
    // Outdoor slot — the Indoor slot for those two categories is dead
    // storage we accept to keep indexing flat and uniform.
    inline constexpr std::size_t kDialogueBucketCount =
        static_cast<std::size_t>(DialogueCategory::Count) *
        static_cast<std::size_t>(DialoguePOV::Count)      *
        static_cast<std::size_t>(DialogueEnv::Count);

    constexpr std::size_t DialogueBucketIndex(DialogueCategory cat, DialoguePOV pov,
                                              DialogueEnv env = DialogueEnv::Outdoor)
    {
        return ((static_cast<std::size_t>(cat) * static_cast<std::size_t>(DialoguePOV::Count))
              + static_cast<std::size_t>(pov)) * static_cast<std::size_t>(DialogueEnv::Count)
              + static_cast<std::size_t>(env);
    }

    constexpr const char* DialogueEnvKey(DialogueEnv env)
    {
        switch (env) {
        case DialogueEnv::Outdoor: return "outdoor";
        case DialogueEnv::Indoor:  return "indoor";
        default:                   return "unknown";
        }
    }

    constexpr const char* DialogueCategoryKey(DialogueCategory cat)
    {
        switch (cat) {
        case DialogueCategory::Outdoors:    return "outdoors";
        case DialogueCategory::Indoors:     return "indoors";
        case DialogueCategory::Dragons:     return "dragons";
        case DialogueCategory::Creatures:   return "creatures";
        case DialogueCategory::SpecificNPC: return "specific_npc";
        case DialogueCategory::Horseback:   return "horseback";
        default:                            return "unknown";
        }
    }

    constexpr const char* DialoguePOVKey(DialoguePOV pov)
    {
        switch (pov) {
        case DialoguePOV::ThirdPerson: return "third_person";
        case DialoguePOV::FirstPerson: return "first_person";
        default:                       return "unknown";
        }
    }

    constexpr const char* DialogueCategoryDisplayName(DialogueCategory cat)
    {
        switch (cat) {
        case DialogueCategory::Outdoors:    return "Outdoors";
        case DialogueCategory::Indoors:     return "Indoors";
        case DialogueCategory::Dragons:     return "Dragons";
        case DialogueCategory::Creatures:   return "Creatures";
        case DialogueCategory::SpecificNPC: return "Specific NPCs";
        case DialogueCategory::Horseback:   return "Horseback";
        default:                            return "Unknown";
        }
    }
}
