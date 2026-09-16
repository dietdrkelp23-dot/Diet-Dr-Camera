#include "PCH.h"
#include "Shouts/ShoutRegistry.h"

namespace DietDrCamera
{
    // FormIDs below are the base IDs within their owning esm (no load-order
    // prefix). TESDataHandler::LookupForm resolves them against the loaded
    // plugin by name, which is stable regardless of user load order.
    //
    // If a FormID is wrong the entry just fails to resolve at init and the
    // corresponding profile silently never activates — no crash.
    const std::array<ShoutEntry, kShoutCount> kShouts = {{
        { ShoutId::AnimalAllegiance,  "Animal Allegiance",  "animal_allegiance",  "Skyrim.esm",      0x0002F7BA },
        { ShoutId::AuraWhisper,       "Aura Whisper",       "aura_whisper",       "Skyrim.esm",      0x000F71D3 },
        { ShoutId::BattleFury,        "Battle Fury",        "battle_fury",        "Dragonborn.esm",  0x0001ECE2 },
        { ShoutId::BecomeEthereal,    "Become Ethereal",    "become_ethereal",    "Skyrim.esm",      0x000B6439 },
        { ShoutId::BendWill,          "Bend Will",          "bend_will",          "Dragonborn.esm",  0x0001CDBE },
        { ShoutId::CallDragon,        "Call Dragon",        "call_dragon",        "Skyrim.esm",      0x00046B88 },
        { ShoutId::CallOfValor,       "Call of Valor",      "call_of_valor",      "Skyrim.esm",      0x000C48CD },
        { ShoutId::ClearSkies,        "Clear Skies",        "clear_skies",        "Skyrim.esm",      0x00032917 },
        { ShoutId::Cyclone,           "Cyclone",            "cyclone",            "Dragonborn.esm",  0x0002AB17 },
        { ShoutId::Disarm,            "Disarm",             "disarm",             "Skyrim.esm",      0x0005D16C },
        { ShoutId::Dismay,            "Dismay",             "dismay",             "Skyrim.esm",      0x0005D17C },
        { ShoutId::DragonAspect,      "Dragon Aspect",      "dragon_aspect",      "Dragonborn.esm",  0x000178BD },
        { ShoutId::Dragonrend,        "Dragonrend",         "dragonrend",         "Skyrim.esm",      0x0004DC74 },
        { ShoutId::DrainVitality,     "Drain Vitality",     "drain_vitality",     "Dawnguard.esm",   0x0000E01C },
        { ShoutId::ElementalFury,     "Elemental Fury",     "elemental_fury",     "Skyrim.esm",      0x0002F7C2 },
        { ShoutId::FireBreath,        "Fire Breath",        "fire_breath",        "Skyrim.esm",      0x00039406 },
        { ShoutId::FrostBreath,       "Frost Breath",       "frost_breath",       "Skyrim.esm",      0x000F3A8D },
        { ShoutId::IceForm,           "Ice Form",           "ice_form",           "Skyrim.esm",      0x000F71D1 },
        { ShoutId::KynesPeace,        "Kyne's Peace",       "kynes_peace",        "Skyrim.esm",      0x0002F7BF },
        { ShoutId::MarkedForDeath,    "Marked for Death",   "marked_for_death",   "Skyrim.esm",      0x0008BB27 },
        { ShoutId::SlowTime,          "Slow Time",          "slow_time",          "Skyrim.esm",      0x000AE7F0 },
        { ShoutId::SoulTear,          "Soul Tear",          "soul_tear",          "Dawnguard.esm",   0x0000E01B },
        { ShoutId::StormCall,         "Storm Call",         "storm_call",         "Skyrim.esm",      0x000AA031 },
        { ShoutId::SummonDurnehviir,  "Summon Durnehviir",  "summon_durnehviir",  "Dawnguard.esm",   0x0000E01E },
        { ShoutId::ThrowVoice,        "Throw Voice",        "throw_voice",        "Skyrim.esm",      0x000602A0 },
        { ShoutId::UnrelentingForce,  "Unrelenting Force",  "unrelenting_force",  "Skyrim.esm",      0x000602A1 },
        { ShoutId::WhirlwindSprint,   "Whirlwind Sprint",   "whirlwind_sprint",   "Skyrim.esm",      0x000602A4 },
    }};

    ShoutRegistry& ShoutRegistry::GetSingleton()
    {
        static ShoutRegistry instance;
        return instance;
    }

    void ShoutRegistry::Init()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("ShoutRegistry::Init: no TESDataHandler — skipping");
            return;
        }

        // Pre-index our 27 known shouts by display name for O(1) lookup
        // as we scan the form array. All TESShout records whose FullName
        // matches get mapped to the same ShoutId — Skyrim defines per-
        // word-level variants sharing display names, and the engine can
        // hand us any of them in a kVoiceCast event.
        std::unordered_map<std::string, ShoutId> nameToId;
        nameToId.reserve(kShoutCount);
        for (const auto& entry : kShouts) {
            nameToId.emplace(entry.displayName, entry.id);
        }

        formToId.clear();
        spellToId.clear();
        spellToForm.clear();
        allShoutSpells.clear();
        std::array<int, kShoutCount> matchCounts{};

        const auto& allShouts = dh->GetFormArray<RE::TESShout>();
        spdlog::info("ShoutRegistry: scanning {} TESShout forms", allShouts.size());

        for (auto* shout : allShouts) {
            if (!shout) continue;
            // Every shout's per-word variation SPELLS go into the spell set —
            // named or not — so an NPC's TESSpellCastEvent can be classified
            // as "a shout" even for mod-added shouts (those resolve to the
            // per-state base noise entry downstream).
            for (const auto& var : shout->variations) {
                if (var.spell) {
                    allShoutSpells.insert(var.spell->GetFormID());
                    auto [owner, inserted] = spellToForm.emplace(var.spell->GetFormID(), shout);
                    if (!inserted && owner->second != shout) owner->second = nullptr;
                }
            }
            const char* name = shout->GetFullName();
            if (!name || !name[0]) continue;
            auto it = nameToId.find(name);
            if (it == nameToId.end()) continue;
            formToId.emplace(shout, it->second);
            for (const auto& var : shout->variations) {
                if (var.spell) spellToId.emplace(var.spell->GetFormID(), it->second);
            }
            matchCounts[static_cast<std::size_t>(it->second)]++;
            spdlog::debug("ShoutRegistry: '{}' variant -> form {:08X}", name, shout->formID);
        }

        int resolvedIds = 0;
        for (const auto& entry : kShouts) {
            const int count = matchCounts[static_cast<std::size_t>(entry.id)];
            if (count > 0) {
                ++resolvedIds;
            } else {
                spdlog::info("ShoutRegistry: '{}' had no matching forms — likely plugin not loaded", entry.displayName);
            }
        }
        spdlog::info("ShoutRegistry: {}/{} shouts resolved (total {} form variants)",
                     resolvedIds, kShoutCount, formToId.size());
    }

    std::optional<ShoutId> ShoutRegistry::Find(const RE::TESShout* form) const
    {
        if (!form) return std::nullopt;
        auto it = formToId.find(form);
        if (it == formToId.end()) return std::nullopt;
        return it->second;
    }

    bool ShoutRegistry::IsShoutSpell(std::uint32_t a_spell) const
    {
        return allShoutSpells.contains(a_spell);
    }

    RE::TESShout* ShoutRegistry::FindFormBySpell(std::uint32_t a_spell) const
    {
        const auto it = spellToForm.find(a_spell);
        return it == spellToForm.end() ? nullptr : it->second;
    }

    std::optional<ShoutId> ShoutRegistry::FindBySpell(std::uint32_t a_spell) const
    {
        auto it = spellToId.find(a_spell);
        if (it == spellToId.end()) return std::nullopt;
        return it->second;
    }
}
