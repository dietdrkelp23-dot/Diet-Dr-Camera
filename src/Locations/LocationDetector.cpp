#include "PCH.h"
#include "Locations/LocationDetector.h"

#include "Settings/SettingsManager.h"

#include <RE/B/BGSLocation.h>
#include <RE/B/BSMultiBoundNode.h>
#include <RE/E/ExtraDataTypes.h>
#include <RE/E/ExtraRoom.h>
#include <RE/N/NiAVObject.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESRegion.h>
#include <RE/T/TESRegionData.h>
#include <RE/T/TESRegionList.h>
#include <RE/T/TESWorldSpace.h>

#include <algorithm>
#include <vector>

#include <unordered_map>

namespace DietDrCamera
{
    namespace
    {
        // Depth cap on the parent walk. Vanilla chains are 2-3 deep
        // (Breezehome -> Whiterun -> Whiterun Hold -> Tamriel); the cap is
        // purely a cycle guard for a malformed modded record.
        constexpr int kMaxParentDepth = 8;

        RE::BGSLocation* PlayerLocation()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return nullptr;
            // The tracked field is what the quest/map systems use and what
            // updates on cell change; GetCurrentLocation() is the fallback for
            // the rare frame where it hasn't been populated yet.
            if (auto* tracked = player->GetPlayerRuntimeData().currentLocation) return tracked;
            return player->GetCurrentLocation();
        }

        // Resolve a location rule to a live form. Returns nullptr when the
        // plugin isn't in the load order, which is how a Dawnguard place
        // silently no-ops on an install without Dawnguard.
        RE::TESForm* ResolveRule(const SettingsManager::LocationOverride& a_lo)
        {
            if (a_lo.plugin.empty() || a_lo.formID == 0) return nullptr;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return nullptr;
            return dh->LookupForm(a_lo.formID, a_lo.plugin);
        }

        std::string LocationName(RE::BGSLocation* a_loc)
        {
            if (!a_loc) return {};
            const char* n = a_loc->GetFullName();
            if (n && *n) return n;
            return fmt::format("0x{:08X}", a_loc->GetFormID());
        }

        // ---- Ambiguous location names ------------------------------------
        //
        // Location display names are NOT unique. The College of Winterhold
        // gives both student halls a location literally named "Dormitory"
        // (Skyrim.esm 857829 in the Hall of Attainment, 125812 in the Hall of
        // Countenance) — two different records, two different bindings, one
        // word on screen. And the parent chain cannot separate them: both
        // parent to "Winterhold College". The CELL is what differs.
        //
        // Which names need qualifying is MEASURED, not tabled — the same
        // reasoning KeywordCounts uses one screen down. Count how many
        // location records in the whole load order carry each display name;
        // anything appearing more than once is ambiguous by construction, and
        // a mod that adds a third "Dormitory" is ranked correctly on the first
        // run with no maintenance.
        //
        // Measuring it also keeps the qualifier HONEST, which a blanket
        // "always append the cell" would not: one Bleak Falls Barrow location
        // spans the Barrow, the Temple and the Sanctum cells, so tagging it
        // with whichever cell you happened to be standing in would imply a
        // scope the binding does not have. That name is unique, so it is left
        // alone.
        const std::unordered_map<std::string, int>& LocationNameCounts()
        {
            static std::unordered_map<std::string, int> counts;
            static bool built = false;
            if (!built) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
                        if (!loc) continue;
                        const char* n = loc->GetFullName();
                        if (!n || !*n) continue;
                        std::string key(n);
                        for (auto& c : key)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        ++counts[key];
                    }
                    built = true;   // only latch once the data was really there
                }
            }
            return counts;
        }

        bool LocationNameIsAmbiguous(const std::string& a_name)
        {
            if (a_name.empty()) return false;
            std::string key = a_name;
            for (auto& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto& counts = LocationNameCounts();
            const auto  it     = counts.find(key);
            return it != counts.end() && it->second > 1;
        }

        // The player's current cell name, when it is worth showing next to
        // a_name (non-empty, and not just a_name again).
        std::string CellQualifierFor(const std::string& a_name)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return {};
            auto* cell = player->GetParentCell();
            if (!cell) return {};
            const char* cn = cell->GetFullName();
            if (!cn || !*cn) return {};
            if (_stricmp(cn, a_name.c_str()) == 0) return {};
            return cn;
        }

        // THE name to show the player for a location: its own, plus the cell
        // that pins it down when its own name is shared with another record.
        // "Dormitory" becomes "Dormitory - Hall of Attainment"; "Breezehome"
        // and "Bleak Falls Barrow" stay exactly as they are.
        std::string DisplayLocationName(RE::BGSLocation* a_loc)
        {
            std::string name = LocationName(a_loc);
            // The " Hold" suffix exists for exactly THREE records (user
            // ruling 2026-08-31): Whiterun, Falkreath and Winterhold name
            // their HOLD record like their capital city, so a tundra bind
            // read as the CITY ("the location override just says whiterun",
            // 2026-08-30) and the dedupe pass dropped one of the twin rows
            // inside the walls. No other hold shares its capital's name (The
            // Pale / The Reach / Eastmarch / Haafingar / Hjaalmarch / The
            // Rift). The name allowlist is the gate, not the keywords alone:
            // mods hang hold keywords on ordinary interiors, which is how
            // "Abandoned Prison Hold" happened when only the keywords
            // decided.
            if (a_loc && (_stricmp(name.c_str(), "Whiterun") == 0 ||
                          _stricmp(name.c_str(), "Falkreath") == 0 ||
                          _stricmp(name.c_str(), "Winterhold") == 0) &&
                (a_loc->HasKeywordString("LocTypeHoldMajor") ||
                 a_loc->HasKeywordString("LocTypeHoldMinor") ||
                 a_loc->HasKeywordString("LocTypeHold"))) {
                return name + " Hold";   // the qualified name is unambiguous
            }
            if (!LocationNameIsAmbiguous(name)) return name;
            const std::string q = CellQualifierFor(name);
            return q.empty() ? name : name + " - " + q;
        }

        // The four MINOR-hold capitals. Vanilla tags only the five walled
        // cities LocTypeCity; Falkreath, Morthal, Dawnstar and Winterhold get
        // LocTypeTown, which makes them KEYWORD-IDENTICAL to Riverwood:
        //
        //   WhiterunLocation    LocTypeCity  LocTypeHabitation  ...HasInn
        //   FalkreathLocation   LocTypeTown  LocTypeHabitation  ...HasInn
        //   RiverwoodLocation   LocTypeTown  LocTypeHabitation  ...HasInn
        //
        // (verified against Skyrim.esm; there is no LocTypeHoldCapital in the
        // data — the HOLD records carry LocTypeHoldMajor/Minor, not the
        // settlements). So no keyword rule can separate a hold capital from a
        // village, and this has to be an explicit table, same as Fort Dawnguard
        // and Castle Volkihar carrying no keyword at all.
        //
        // Matched by FORM ID rather than editor ID: EDIDs are not retained at
        // runtime for non-keyword forms unless something restores them.
        constexpr std::uint32_t kHoldCapitalTowns[] = {
            0x018A49,   // FalkreathLocation
            0x018A53,   // MorthalLocation
            0x018A50,   // DawnstarLocation
            0x018A51,   // WinterholdLocation
        };

        bool IsHoldCapitalTown(RE::BGSLocation* a_loc)
        {
            if (!a_loc) return false;
            // Skyrim.esm is always mod index 0, and these are its records.
            if ((a_loc->GetFormID() >> 24) != 0x00) return false;
            const std::uint32_t local = a_loc->GetFormID() & 0x00FFFFFF;
            for (const auto id : kHoldCapitalTowns) {
                if (id == local) return true;
            }
            return false;
        }

        // Every LocType keyword on a location, in record order.
        std::vector<std::string> LocTypeKeywords(RE::BGSLocation* a_loc)
        {
            std::vector<std::string> out;
            if (!a_loc) return out;
            const bool capital = IsHoldCapitalTown(a_loc);
            for (auto* kw : a_loc->GetKeywords()) {
                if (!kw) continue;
                const char* raw = kw->formEditorID.c_str();
                const std::string_view id{ raw ? raw : "" };
                if (id.rfind("LocType", 0) != 0) continue;
                // A hold capital reads as a City, not a Town — it should group
                // with Whiterun and Windhelm, not with Riverwood.
                if (capital && id == "LocTypeTown") {
                    out.emplace_back("LocTypeCity");
                    continue;
                }
                out.emplace_back(id);
            }
            return out;
        }

        std::string KeywordsJoined(RE::BGSLocation* a_loc)
        {
            std::string out;
            for (const auto& k : LocTypeKeywords(a_loc)) {
                if (!out.empty()) out += ", ";
                out += k;
            }
            return out.empty() ? std::string{ "(none)" } : out;
        }

        // Player-facing name for a LocType keyword.
        //
        // The named cases are the ones whose editor ID doesn't read as English
        // ("LocTypeDraugrCrypt" is what the game calls a Nordic ruin) or where
        // the plain conversion would be misleading. Everything else - including
        // any keyword a mod invents - falls through to a generic conversion, so
        // the list never has to be exhaustive to be useful. That fallback is
        // the whole reason this is a lookup with a default rather than a table
        // the feature depends on.
        std::string KeywordLabel(const std::string& a_kw)
        {
            struct Named { const char* id; const char* label; };
            static constexpr Named kNamed[] = {
                { "LocTypeDraugrCrypt",      "All Nordic Dungeons" },
                { "LocTypeDwarvenAutomatons","All Dwarven Ruins" },
                { "LocTypeFalmerHive",       "All Falmer Hives" },
                { "LocTypeVampireLair",      "All Vampire Lairs" },
                { "LocTypeWarlockLair",      "All Warlock Lairs" },
                { "LocTypeNecromancerLair",  "All Necromancer Lairs" },
                { "LocTypeHagravenNest",     "All Hagraven Nests" },
                { "LocTypeSprigganGrove",    "All Spriggan Groves" },
                { "LocTypeWitchmistGrove",   "All Witchmist Groves" },
                { "LocTypeBanditCamp",       "All Bandit Camps" },
                { "LocTypeForswornCamp",     "All Forsworn Camps" },
                { "LocTypeGiantCamp",        "All Giant Camps" },
                { "LocTypeAnimalDen",        "All Animal Dens" },
                { "LocTypeDragonPriestLair", "All Dragon Priest Lairs" },
                { "LocTypeDragonLair",       "All Dragon Lairs" },
                { "LocTypeDungeon",          "All Dungeons" },
                { "LocTypeCave",             "All Caves" },
                { "LocTypeMine",             "All Mines" },
                { "LocTypeShipwreck",        "All Shipwrecks" },
                { "LocTypeDaedricShrine",    "All Daedric Shrines" },
                { "LocTypeMilitaryFort",     "All Forts" },
                { "LocTypeMilitaryCamp",     "All Military Camps" },
                { "LocTypeCity",             "All Cities" },
                { "LocTypeTown",             "All Towns" },
                { "LocTypeSettlement",       "All Settlements" },
                { "LocTypeHabitation",       "Anywhere People Live" },
                { "LocTypeHabitationHasInn", "Anywhere With An Inn" },
                { "LocTypeHold",             "Whole Holds" },
                { "LocTypeHoldCapital",      "All Hold Capitals" },
                { "LocTypeHoldMajor",        "All Major Holds" },
                { "LocTypeHoldMinor",        "All Minor Holds" },
                { "LocTypeDwelling",         "All Dwellings" },
                { "LocTypeHouse",            "All Houses" },
                { "LocTypePlayerHouse",      "All Player Homes" },
                { "LocTypeInn",              "All Inns" },
                { "LocTypeStore",            "All Shops" },
                { "LocTypeTemple",           "All Temples" },
                { "LocTypeCastle",           "All Castles" },
                { "LocTypeJail",             "All Jails" },
                { "LocTypeBarracks",         "All Barracks" },
                { "LocTypeFarm",             "All Farms" },
                { "LocTypeLumberMill",       "All Lumber Mills" },
                { "LocTypeOrcSettlement",    "All Orc Strongholds" },
                { "LocTypeGuild",            "All Guild Halls" },
                { "LocTypeStewardsDwelling", "All Jarls' Quarters" },
                { "LocTypeShip",             "All Ships" },
                { "LocTypeDragonRoost",      "All Dragon Roosts" },
                { "LocTypeStandingStone",    "All Standing Stones" },
                { "LocTypeStreamSource",     "All Stream Sources" },
                { "LocTypstronghold",        "All Strongholds" },
            };
            for (const auto& n : kNamed)
                if (_stricmp(n.id, a_kw.c_str()) == 0) return n.label;

            // Generic: "LocTypeSomethingElse" -> "All Something Elses".
            std::string body = a_kw.rfind("LocType", 0) == 0 ? a_kw.substr(7) : a_kw;
            std::string spaced;
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (i > 0 && std::isupper(static_cast<unsigned char>(body[i])) &&
                    !std::isupper(static_cast<unsigned char>(body[i - 1]))) {
                    spaced += ' ';
                }
                spaced += body[i];
            }
            if (spaced.empty()) return a_kw;
            if (spaced.back() != 's' && spaced.back() != 'S') spaced += 's';
            return "All " + spaced;
        }

        // Keywords never worth offering as a "kind of place".
        //
        // Two groups. The first describe a game RULE rather than a kind of
        // place (can this be cleared, is it a no-reset zone). The second are
        // the HOLD tiers: the hold is offered as a place in its own right, by
        // name, so "All Major Holds" alongside "Whiterun Hold" is the same
        // idea twice and the broad one is never the one anyone wants.
        bool IsNoiseKeyword(const std::string& a_kw)
        {
            static constexpr const char* kSkip[] = {
                "LocTypeClearable", "LocTypeSafeZone", "LocTypeWorldspace",
                "LocTypeCityLocation", "LocTypeNoResetZone",
                "LocTypeHabitationHasInn",
                "LocTypeHold", "LocTypeHoldCapital", "LocTypeHoldMajor",
                "LocTypeHoldMinor",
            };
            for (const char* k : kSkip)
                if (_stricmp(k, a_kw.c_str()) == 0) return true;
            return false;
        }

        // How many locations in the whole load order carry each LocType
        // keyword. Built once, lazily.
        const std::unordered_map<std::string, int>& KeywordCounts()
        {
            static std::unordered_map<std::string, int> counts;
            static bool built = false;
            if (!built) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
                        if (!loc) continue;
                        for (const auto& k : LocTypeKeywords(loc)) ++counts[k];
                    }
                    built = true;   // only latch once the data was really there
                }
            }
            return counts;
        }

        // THE one type row: the most specific kind of place this record is.
        //
        // A farm carries LocTypeHabitation, LocTypeSettlement and LocTypeFarm.
        // Offering all three is nonsense - "All Settlements" next to "All
        // Farms" is the same place twice, at two widths nobody asked for. Only
        // one row is wanted, and it has to be the narrowest.
        //
        // Specificity is measured, not tabled: the keyword carried by the
        // FEWEST locations in the load order is the most specific one. That is
        // true by construction (LocTypeFarm is on the farms; LocTypeHabitation
        // is on every inhabited place in Skyrim), it needs no maintenance, and
        // it ranks a keyword invented by a mod correctly on the first run. A
        // hand-written priority table would have to be revised for every DLC
        // and every mod, and would still be a guess.
        std::string MostSpecificKeyword(RE::BGSLocation* a_loc)
        {
            const auto& counts = KeywordCounts();
            std::string best;
            int bestCount = 0;
            for (const auto& k : LocTypeKeywords(a_loc)) {
                if (IsNoiseKeyword(k)) continue;
                const auto it = counts.find(k);
                const int  c  = (it != counts.end()) ? it->second : 1;
                if (best.empty() || c < bestCount) { best = k; bestCount = c; }
            }
            return best;
        }

        // ---- Climate regions -------------------------------------------
        //
        // The answer for open country, and the reason it exists: 94% of
        // Skyrim's exterior cells carry NO location record, and the hold
        // cannot be derived from them (only 2 of 13 hold records even have a
        // map marker). But every one of those cells DOES sit inside climate
        // REGIONS, and those regions are real polygons that partition the map
        // the way a player experiences it:
        //     WeatherReach          the Reach
        //     WeatherFallForest     the Rift
        //     WeatherTundra         the Whiterun tundra
        //     WeatherVolcanicTundra Eastmarch
        //     WeatherPineForest     Falkreath's forest
        //     WeatherSnow           the frozen north
        //     WeatherCoast          the northern coast
        //     WeatherTundraMarsh    Hjaalmarch's marshes
        //     WeatherMountains      the mountain spine
        // That is exactly "different vibes in the Reach and in the Rift", and
        // it is arguably a better axis for a CAMERA than an administrative
        // hold boundary, because it tracks what the place actually looks like.
        // Mod worldspaces get theirs for free: a region carrying weather data
        // is a climate region whoever authored it.

        // Is this region a climate region? A region carries typed data blocks;
        // one of type kWeather is what makes it a climate zone rather than an
        // audio, lighting or navmesh-fix region (Skyrim has 297 regions with
        // polygons, and only ~55 of them are climate).
        bool IsClimateRegion(RE::TESRegion* a_reg)
        {
            if (!a_reg || !a_reg->dataList) return false;
            for (auto* d : a_reg->dataList->regionDataList) {
                if (d && d->GetType() == RE::TESRegionData::Type::kWeather) return true;
            }
            return false;
        }

        // Bounding-box area of a region's polygons, as a specificity measure:
        // regions nest (a spot is in WeatherTundra AND WeatherMountains), and
        // the smaller one is the more local, more meaningful answer. Same
        // narrowest-wins rule the keyword row uses, just measured differently
        // because regions have geometry and keywords have counts.
        float RegionSpan(RE::TESRegion* a_reg)
        {
            if (!a_reg || !a_reg->pointLists) return FLT_MAX;
            float best = FLT_MAX;
            for (auto* pl : *a_reg->pointLists) {
                if (!pl) continue;
                const float w = pl->maximums.x - pl->minimums.x;
                const float h = pl->maximums.y - pl->minimums.y;
                if (w <= 0.0f || h <= 0.0f) continue;
                best = (std::min)(best, w * h);
            }
            return best;
        }

        // The narrowest climate region the player's cell sits in, or nullptr.
        RE::TESRegion* ClimateRegionOf(RE::TESObjectCELL* a_cell)
        {
            if (!a_cell) return nullptr;
            auto* list = a_cell->GetRegionList(false);
            if (!list) return nullptr;
            RE::TESRegion* best = nullptr;
            float bestSpan = FLT_MAX;
            for (auto* reg : *list) {
                if (!IsClimateRegion(reg)) continue;
                const float span = RegionSpan(reg);
                if (!best || span < bestSpan) { best = reg; bestSpan = span; }
            }
            return best;
        }

        // Player-facing name for a climate region. The vanilla ones are named
        // after their weather, which is not how anyone thinks about them, so
        // the big nine get the name of the place they actually are. Anything
        // else - including a mod's own region - falls through to a generic
        // conversion, so the table never has to be complete.
        std::string RegionLabel(RE::TESRegion* a_reg)
        {
            const char* raw = a_reg ? a_reg->GetFormEditorID() : nullptr;
            const std::string id{ raw ? raw : "" };
            if (id.empty()) return "This area";

            struct Named { const char* id; const char* label; };
            static constexpr Named kNamed[] = {
                { "WeatherReach",          "The Reach" },
                { "WeatherFallForest",     "The Rift" },
                { "WeatherTundra",         "The Whiterun Tundra" },
                { "WeatherVolcanicTundra", "Eastmarch" },
                { "WeatherPineForest",     "The Pine Forest" },
                { "WeatherSnow",           "The Frozen North" },
                { "WeatherCoast",          "The Northern Coast" },
                { "WeatherTundraMarsh",    "The Marshes" },
                { "WeatherMountains",      "The Mountains" },
                { "WeatherVolcanicAsh01",  "The Ashlands" },
                { "WeatherVolcanicAsh02",  "The Ashlands" },
            };
            for (const auto& n : kNamed)
                if (_stricmp(n.id, id.c_str()) == 0) return n.label;

            std::string body = id;
            for (const char* pre : { "Weather", "Region", "FX" }) {
                const std::size_t pl = std::strlen(pre);
                if (body.size() > pl && _strnicmp(body.c_str(), pre, pl) == 0) {
                    body = body.substr(pl);
                    break;
                }
            }
            std::string spaced;
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (i > 0 && std::isupper(static_cast<unsigned char>(body[i])) &&
                    !std::isupper(static_cast<unsigned char>(body[i - 1]))) spaced += ' ';
                spaced += body[i];
            }
            return spaced.empty() ? id : spaced;
        }

        // The hold containing a spot with NO location record of its own.
        //
        // Hold LOCATION records are almost all markerless - only Falkreath and
        // Haafingar carry one - so asking a hold where it is gets you nothing,
        // and seeding a nearest-neighbour search with those two put everything
        // north of Whiterun in Falkreath. But the cities, towns and farms
        // inside them DO all have map markers, and every marker-carrying
        // settlement resolves to a hold through its parent chain: checked, 48
        // of 48.
        //
        // So the hold is the hold of the NEAREST SETTLEMENT. Verified offline
        // against the shipped records before being written here - Whiterun's
        // marker and points 2000 / 6000 / 12000 units north of it all resolve
        // to Whiterun Hold; Morthal to Hjaalmarch, Riften to the Rift,
        // Markarth to the Reach.
        //
        // Settlements ONLY, deliberately: dungeon records also have markers
        // and holds, but their parents carry Bethesda's filing quirks (Bleak
        // Falls Barrow's parent is Falkreath Hold), so seeding with them would
        // import those errors into open country.
        //
        // Distance CHOOSES between holds here. It never bounds one - that was
        // the mistake the old City environment made.

        struct SettlementSeed
        {
            RE::NiPoint3       pos;
            RE::BGSLocation*   hold  = nullptr;
            RE::TESWorldSpace* world = nullptr;   // may be null; see below
            std::string        name;              // for the decision log
        };

        // Built once. The count is logged because it is the single number that
        // says whether this works at all: it must be ~48 on a vanilla load
        // order, and a previous version silently produced a handful because it
        // filtered on a marker's worldspace, which is null for a reference
        // whose cell isn't loaded. That filter is why the answer outside
        // Whiterun came out as Hjaalmarch - almost every seed was being thrown
        // away and "nearest" was whatever happened to survive.
        const std::vector<SettlementSeed>& SettlementSeeds()
        {
            static std::vector<SettlementSeed> seeds;
            static bool built = false;
            if (built) return seeds;

            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return seeds;   // not ready yet - try again next call
            int dropped = 0;

            for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
                if (!loc) continue;
                if (!loc->HasKeywordString("LocTypeCity") &&
                    !loc->HasKeywordString("LocTypeTown") &&
                    !loc->HasKeywordString("LocTypeSettlement") &&
                    !loc->HasKeywordString("LocTypeHabitation")) continue;

                RE::BGSLocation* hold = nullptr;
                int depth = 0;
                for (RE::BGSLocation* l = loc; l && depth < kMaxParentDepth; l = l->parentLoc, ++depth) {
                    if (l->HasKeywordString("LocTypeHold")) { hold = l; break; }
                }
                if (!hold) continue;

                auto marker = loc->worldLocMarker.get();
                if (!marker) continue;

                SettlementSeed sd;
                // GetPosition() reads the reference's stored location and is
                // valid whether or not its cell is loaded, unlike the parent
                // cell (and therefore the worldspace), which is not.
                sd.pos   = marker->GetPosition();
                sd.hold  = hold;
                sd.name  = LocationName(loc);

                // A seed at exactly the world origin is a marker that failed to
                // give a position, not a place. It has to be dropped rather
                // than competed with, because WHITERUN sits ~2,900 units from
                // the origin - closer than most real neighbours - so a single
                // zero seed wins the whole tundra and reports whatever hold it
                // happens to belong to. That is precisely how "Hjaalmarch"
                // came out of standing outside Whiterun.
                if (std::abs(sd.pos.x) < 1.0f && std::abs(sd.pos.y) < 1.0f) {
                    ++dropped;
                    continue;
                }
                sd.world = marker->GetWorldspace();
                if (!sd.world) {
                    if (auto* sc = marker->GetSaveParentCell();
                        sc && !sc->IsInteriorCell()) {
                        sd.world = sc->GetRuntimeData().worldSpace;
                    }
                }
                seeds.push_back(sd);
            }
            built = true;
            int resolved = 0;
            for (const auto& sd : seeds) if (sd.world) ++resolved;
            spdlog::info("[LOC] settlement seeds built: {} (expect ~48 on a vanilla load order), "
                         "{} dropped for having no position, {} with a resolved worldspace",
                         seeds.size(), dropped, resolved);
            // One line per seed, once per session: the 2026-08-15 field log
            // showed "Heartwood Mill" winning the Whiterun tundra from a
            // near-origin position (real mill: tens of thousands of units
            // away by Lake Honrich) — a marker whose STORED position is
            // garbage can only be identified by dumping what was stored.
            for (const auto& sd : seeds) {
                // Worldspace by full name + form ID: editor IDs are not
                // retained at runtime for WRLD records.
                const char* wsName = sd.world ? sd.world->GetFullName() : nullptr;
                spdlog::info("[LOC]   seed \"{}\" pos=({:.0f},{:.0f}) hold=\"{}\" ws={}",
                             sd.name, sd.pos.x, sd.pos.y, LocationName(sd.hold),
                             sd.world ? fmt::format("\"{}\" 0x{:08X}",
                                                    (wsName && *wsName) ? wsName : "?",
                                                    sd.world->GetFormID())
                                      : std::string{ "(unresolved)" });
            }
            return seeds;
        }

        RE::BGSLocation* HoldByNearestSettlement()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return nullptr;
            auto* cell = player->GetParentCell();
            if (!cell || cell->IsInteriorCell()) return nullptr;
            auto* pw = player->GetWorldspace();
            if (!pw) return nullptr;

            const auto& seeds = SettlementSeeds();
            if (seeds.empty()) return nullptr;

            // Two tiers. A seed whose worldspace RESOLVED and matches the
            // player's is trustworthy: its position and its map are both
            // known. A seed whose worldspace could NOT be resolved only
            // competes when no trustworthy seed exists at all (a mod map
            // with no resolvable settlements) — the 2026-08-15 field log
            // showed why it can't compete head-to-head: "Heartwood Mill"
            // carried an unresolved worldspace AND a near-origin stored
            // position, so it beat Whiterun for the whole tundra and the
            // answer came out "The Rift". (The earlier bug was the
            // opposite mistake — dropping unresolved seeds ENTIRELY, back
            // when nearly every seed failed to resolve; the save-parent-
            // cell fallback above is what makes resolution the norm now,
            // and the seed dump at build time verifies that in the log.)
            const auto pos = player->GetPosition();
            RE::BGSLocation* best = nullptr;
            const SettlementSeed* bestSeed = nullptr;
            float bestD2 = -1.0f;
            for (int tier = 0; tier < 2 && !best; ++tier) {
                const bool wantResolved = (tier == 0);
                for (const auto& sd : seeds) {
                    if (wantResolved != (sd.world != nullptr)) continue;
                    if (sd.world && sd.world != pw) continue;
                    const float dx = pos.x - sd.pos.x;
                    const float dy = pos.y - sd.pos.y;   // planar: height says nothing about which hold
                    const float d2 = dx * dx + dy * dy;
                    if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; best = sd.hold; bestSeed = &sd; }
                }
            }

            // Log the DECISION, not just the inputs: which settlement won and
            // how far away it was. "It says the wrong hold" is unanswerable
            // without this - the seed count proves the table exists, this
            // proves what it chose and from where.
            static RE::BGSLocation* sLoggedHold = nullptr;
            if (best != sLoggedHold) {
                sLoggedHold = best;
                spdlog::info("[LOC] hold via nearest settlement: \"{}\" at {:.0f} units -> \"{}\" "
                             "(player {:.0f},{:.0f})",
                             bestSeed ? bestSeed->name : "?",
                             bestD2 >= 0.0f ? std::sqrt(bestD2) : -1.0f,
                             best ? LocationName(best) : "none", pos.x, pos.y);
            }

            // No cell-pointer cache here on purpose: the engine recycles cell
            // objects, so a pointer-keyed cache can hand back another hold's
            // answer after a load. The scan is ~48 entries of flat arithmetic.
            return best;
        }

        // Shared by the offered area and the runtime ancestry walk.
        RE::BGSLocation* OpenCountryHold(RE::BGSLocation* location)
        {
            return location ? nullptr : HoldByNearestSettlement();
        }

        // The AREA a location belongs to: simply its parent record.
        //
        // Verified against the shipped ESMs rather than assumed, because the
        // obvious guess (walk up to the first LocTypeHold) is wrong for two
        // separate reasons:
        //   Breezehome  -> WhiterunLocation -> WhiterunHoldLocation. The
        //                  useful area for a house in the city is the CITY;
        //                  the hold walk would skip past it.
        //   Black Book  -> DLC2Book01DungeonLocation -> DLC2ApocryphaLocation,
        //                  which carries NO keywords and no hold anywhere in
        //                  the chain. Same shape for Sovngarde (Hall of Heroes
        //                  -> Sovngarde) and the Soul Cairn. A hold walk
        //                  returns nothing at all in a Daedric realm.
        // The parent is right in every one of those, and in the plain case too
        // (BleakFallsBarrowLocation's parent is a hold record already).
        //
        // Returns nullptr when the location IS a hold: its parent is the
        // worldspace-level record (TamrielLocation), and "all of Tamriel" is
        // exactly the row the option list is meant not to have.
        RE::BGSLocation* AreaOf(RE::BGSLocation* a_loc)
        {
            if (!a_loc || a_loc->HasKeywordString("LocTypeHold")) return nullptr;
            return a_loc->parentLoc;
        }

        // ---- Room bounds ------------------------------------------------
        //
        // Interior cells are partitioned into ROOM BOUNDS by the portal /
        // occlusion system — real boxes the level designer drew around each
        // chamber. A room marker is an ordinary REFR (stable form ID) whose
        // extra list carries kRoomRefData, and whose loaded 3D is a
        // BSMultiBoundNode with a point-containment test. That makes a room
        // the perfect narrowest location rule: wall-accurate, authored, no
        // radius anywhere (the Windhelm lesson).
        //
        // Markers are cached per CELL — keyed by pointer AND form ID, because
        // the engine recycles cell objects (the hold-cache lesson) — and the
        // per-frame cost is a handful of QPointWithin calls.

        struct RoomMarker
        {
            RE::TESObjectREFR* refr   = nullptr;  // owned by the cell while cached
            std::string        plugin;
            std::uint32_t      formID = 0;        // local form ID
            int                index  = 0;        // 1-based, deterministic order
        };

        struct RoomCache
        {
            const void*             cellPtr = nullptr;
            std::uint32_t           cellID  = 0;
            std::string             cellName;
            std::vector<RoomMarker> markers;
        };

        RE::BSMultiBoundNode* RoomNodeOf(const RoomMarker& a_m, bool* a_via3d);

        const RoomCache& RoomMarkersOfCurrentCell()
        {
            static RoomCache cache;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell   = player ? player->GetParentCell() : nullptr;
            if (!cell || !cell->IsInteriorCell()) {
                cache = RoomCache{};
                return cache;
            }
            if (cache.cellPtr == cell && cache.cellID == cell->GetFormID())
                return cache;

            cache = RoomCache{};
            cache.cellPtr = cell;
            cache.cellID  = cell->GetFormID();
            const char* cn = cell->GetFullName();
            cache.cellName = (cn && *cn) ? cn : "this place";

            cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                if (a_ref && a_ref->extraList.HasType(RE::ExtraDataType::kRoomRefData)) {
                    RoomMarker m;
                    m.refr   = a_ref;
                    m.formID = a_ref->GetLocalFormID();
                    if (auto* file = a_ref->GetFile(0)) m.plugin = file->fileName;
                    if (!m.plugin.empty() && m.formID != 0)
                        cache.markers.push_back(std::move(m));
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            // Deterministic order (the enumeration order is a hash set's), so
            // "Room 3" names the same room every session.
            std::sort(cache.markers.begin(), cache.markers.end(),
                      [](const RoomMarker& a, const RoomMarker& b) {
                          return a.formID < b.formID;
                      });
            for (std::size_t i = 0; i < cache.markers.size(); ++i)
                cache.markers[i].index = static_cast<int>(i) + 1;
            // Say which resolution path the bounds are reachable through —
            // if a cell ever logs all-unreachable, that is the fact to fix.
            int via3d = 0, viaExtra = 0, none = 0;
            for (const auto& m : cache.markers) {
                bool b3d = false;
                if (RoomNodeOf(m, &b3d)) (b3d ? via3d : viaExtra)++;
                else ++none;
            }
            spdlog::debug("[ROOM] cell \"{}\": {} room markers (3d={}, extra={}, unreachable={})",
                         cache.cellName, cache.markers.size(), via3d, viaExtra, none);
            return cache;
        }

        // Resolve a marker to its multibound node, trying both places it can
        // live: the marker's own loaded 3D, and the ExtraRoom record holding
        // the BSMultiBoundRoom (which IS-A BSMultiBoundNode; no full header
        // in this CommonLibSSE, but Ni single inheritance puts the base at
        // offset 0, and BSMultiBoundNode::GetMultiBoundRoom documents the
        // relationship). Which path actually fires is logged once per cell —
        // the field log adjudicates, not an assumption.
        RE::BSMultiBoundNode* RoomNodeOf(const RoomMarker& a_m, bool* a_via3d = nullptr)
        {
            if (!a_m.refr) return nullptr;
            if (auto* obj = a_m.refr->Get3D()) {
                if (auto* mbn = obj->AsMultiBoundNode()) {
                    if (a_via3d) *a_via3d = true;
                    return mbn;
                }
            }
            if (auto* xr = a_m.refr->extraList.GetByType<RE::ExtraRoom>()) {
                if (xr->room) {
                    if (a_via3d) *a_via3d = false;
                    return reinterpret_cast<RE::BSMultiBoundNode*>(xr->room.get());
                }
            }
            return nullptr;
        }

        bool RoomContains(const RoomMarker& a_m, const RE::NiPoint3& a_pos)
        {
            auto* mbn = RoomNodeOf(a_m);
            if (!mbn) return false;
            return mbn->QPointWithin(a_pos);
        }

        // The room the player is in, with hysteresis: room bounds OVERLAP at
        // portals, so while the previous answer still contains the player it
        // keeps winning — no flip-flopping in a doorway. Returns nullptr when
        // no bound contains the point (markerless cell, or a genuine gap).
        const RoomMarker* CurrentRoomMarker()
        {
            const auto& cache = RoomMarkersOfCurrentCell();
            if (cache.markers.empty()) return nullptr;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return nullptr;
            // Test at chest height: a marker box authored snug to the floor
            // geometry can miss a point at the feet.
            RE::NiPoint3 pos = player->GetPosition();
            pos.z += 64.0f;

            static const void*   sHeldCell = nullptr;
            static std::uint32_t sHeldID   = 0;
            if (sHeldCell == cache.cellPtr && sHeldID != 0) {
                for (const auto& m : cache.markers)
                    if (m.formID == sHeldID && RoomContains(m, pos)) return &m;
            }
            for (const auto& m : cache.markers) {
                if (RoomContains(m, pos)) {
                    sHeldCell = cache.cellPtr;
                    sHeldID   = m.formID;
                    return &m;
                }
            }
            sHeldCell = cache.cellPtr;
            sHeldID   = 0;
            return nullptr;
        }

    }

    LocationDetector& LocationDetector::GetSingleton()
    {
        static LocationDetector instance;
        return instance;
    }

    // (Weather-conditioned places were removed 2026-08-13 at user request —
    // the weather classifier, the per-place mask and the two-pass ordering
    // all went with them.)

    void LocationDetector::Classify(RE::BGSLocation* a_loc, RE::BGSLocation* a_openCountryHold, std::vector<int>& a_out,
                                    std::string* a_outReason) const
    {
        auto& s = SettingsManager::GetSingleton();
        a_out.clear();

        const auto add = [&](std::size_t i, const char* fmtWhat, const std::string& what) {
            for (int e : a_out) if (e == static_cast<int>(i)) return;
            if (a_out.empty() && a_outReason) *a_outReason = fmt::format(fmt::runtime(fmtWhat), what);
            a_out.push_back(static_cast<int>(i));
        };

        // Room — the narrowest tier of all: a bound room beats the cell, the
        // location and everything above it, exactly like every other
        // "more specific wins for the fields it defines" rule in the mod.
        if (const auto* rm = CurrentRoomMarker()) {
            for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                const auto& lo = s.locationOverrides[i];
                if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Room) continue;
                if (lo.formID == rm->formID &&
                    _stricmp(lo.plugin.c_str(), rm->plugin.c_str()) == 0)
                    add(i, "room \"{}\"", lo.name);
            }
        }

        // EVERY match, narrowest first — not just the winner. Resolution picks
        // per ENTRY from this list (see SettingsManager::activeLocationChain),
        // so a broad place stays in effect for everything a narrow one doesn't
        // define.
        //
        // With no exact location (open wilderness), use the same recovered
        // hold offered by the binding menu. Cell rules still precede that
        // broad fallback; named locations keep their own ancestor chains.
        // The region and worldspace tiers also answer — which is the whole reason they exist.
        //
        // Outer loop is the ANCESTOR, inner loop is the place. That order is
        // what makes the ordering narrowest-first across ALL places, instead of
        // list order deciding. Within one ancestor a named LOCATION comes
        // before a KEYWORD: naming a place is a direct statement about it.
        const auto visitLocation = [&](RE::BGSLocation* loc) {
            for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                const auto& lo = s.locationOverrides[i];
                if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Location) continue;
                if (ResolveRule(lo) == static_cast<RE::TESForm*>(loc))
                    add(i, "\"{}\"", LocationName(loc));
            }

            const auto kws = LocTypeKeywords(loc);
            for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                const auto& lo = s.locationOverrides[i];
                if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Keyword) continue;
                for (const auto& k : kws) {
                    if (_stricmp(k.c_str(), lo.keyword.c_str()) != 0) continue;
                    add(i, "{}", lo.keyword + " on \"" + LocationName(loc) + "\"");
                    break;
                }
            }
        };
        const auto parentOf = [](RE::BGSLocation* location) { return location->parentLoc; };
        VisitLocationAncestors(a_loc, static_cast<RE::BGSLocation*>(nullptr), parentOf, visitLocation, kMaxParentDepth);

        // Cell — only ever created for a spot with no location record, so it
        // sits between the location walk and the broad tiers.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                    const auto& lo = s.locationOverrides[i];
                    if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Cell) continue;
                    auto* dh = RE::TESDataHandler::GetSingleton();
                    if (!dh || lo.plugin.empty() || lo.formID == 0) continue;
                    if (dh->LookupForm(lo.formID, lo.plugin) == static_cast<RE::TESForm*>(cell)) {
                        const char* cn = cell->GetFullName();
                        add(i, "cell \"{}\"", std::string(cn ? cn : "?"));
                    }
                }
            }
        }

        if (!a_loc)
            VisitLocationAncestors(a_loc, a_openCountryHold, parentOf, visitLocation, kMaxParentDepth);

        // Climate region — how open country is answered.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* reg = ClimateRegionOf(player->GetParentCell())) {
                for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                    const auto& lo = s.locationOverrides[i];
                    if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Region) continue;
                    auto* dh = RE::TESDataHandler::GetSingleton();
                    if (!dh || lo.plugin.empty() || lo.formID == 0) continue;
                    if (dh->LookupForm(lo.formID, lo.plugin) == static_cast<RE::TESForm*>(reg))
                        add(i, "climate \"{}\"", lo.name);
                }
            }
        }

        // Worldspace — the broadest rule, and the one that can answer when
        // there is no location record at all.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* ws = player->GetWorldspace()) {
                for (std::size_t i = 0; i < s.locationOverrides.size(); ++i) {
                    const auto& lo = s.locationOverrides[i];
                    if (!lo.enabled || lo.kind != SettingsManager::LocationOverride::Kind::Worldspace) continue;
                    auto* dh = RE::TESDataHandler::GetSingleton();
                    if (!dh || lo.plugin.empty() || lo.formID == 0) continue;
                    if (dh->LookupForm(lo.formID, lo.plugin) == static_cast<RE::TESForm*>(ws)) {
                        const char* wn = ws->GetFullName();
                        add(i, "worldspace \"{}\"", std::string(wn ? wn : "?"));
                    }
                }
            }
        }

        if (a_out.empty() && a_outReason)
            *a_outReason = "no place matches this location or any of its parents";
    }

    void LocationDetector::Update()
    {
        auto& s = SettingsManager::GetSingleton();
        if (!s.locationOverridesEnabled) {
            // Dormant. Clear the index so a mid-session disable takes effect
            // immediately, and force a fresh classification on re-enable.
            if (s.activeLocationIdx != -1) {
                s.activeLocationIdx = -1;
                spdlog::info("[LOC] -> none (location overrides disabled)");
            }
            s.activeLocationChain.clear();
            _lastChain.clear();
            _lastIdx      = -1;
            _loggedIdx    = -1;
            Invalidate();
            return;
        }

        auto* loc = PlayerLocation();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* ws = player ? player->GetWorldspace() : nullptr;
        auto* hold = OpenCountryHold(loc);
        auto* region = ClimateRegionOf(cell);
        const auto* room = CurrentRoomMarker();
        const LocationMatchContext context{loc, ws, cell, cell ? cell->GetFormID() : 0,
            room ? room->refr : nullptr, region, hold};

        // Room term of the cache key: walking through a doorway changes the
        // active room with the location pointer, worldspace and weather all
        // unchanged — the chain must reclassify on that alone.
        {
            const auto* rm = room;
            const std::uint32_t roomID = rm ? rm->formID : 0;
            if (roomID != _lastRoomID) {
                if (_lastRoomID != 0 || roomID != 0)
                    spdlog::debug("[ROOM] -> {}", rm ? fmt::format("room 0x{:06X}", roomID)
                                               : std::string{ "none" });
                _lastRoomID = roomID;
            }
        }

        if (_matchState.Refresh(context)) {
            std::string reason;
            Classify(loc, hold, _lastChain, &reason);
            const int idx = _lastChain.empty() ? -1 : _lastChain.front();
            _lastIdx        = idx;
            s.activeLocationChain = _lastChain;
            s.activeLocationIdx   = idx;
            LogChange(s, loc, idx, reason);
            return;
        }

        s.activeLocationChain = _lastChain;
        s.activeLocationIdx   = _lastIdx;
        // indoorMode can flip without the location pointer changing (a door
        // into a house whose interior shares the parent location), and that
        // flip alone can switch the place on or off via appliesIndoors — so
        // the effective-state log has to be re-checked every frame, not only
        // on a location change.
        LogChange(s, loc, _lastIdx, "(unchanged)");
    }

    // One line whenever the EFFECTIVE place changes. Logs the place the
    // detector picked and whether it is actually applying, which is the pair
    // that distinguishes "detection is wrong" from "detection is right but the
    // profile set didn't follow". Cheap: two compares per frame, one line per
    // real change.
    void LocationDetector::LogChange(SettingsManager& s, RE::BGSLocation* a_loc,
                                     int a_idx, const std::string& a_reason)
    {
        const bool applying = (s.ActiveLocation() != nullptr);
        if (a_idx == _loggedIdx && applying == _loggedApply) return;
        _loggedIdx   = a_idx;
        _loggedApply = applying;
        if (a_idx < 0) {
            spdlog::info("[LOC] location=\"{}\" keywords=[{}] -> no override ({})",
                         LocationName(a_loc), KeywordsJoined(a_loc), a_reason);
            return;
        }
        const auto& lo = s.locationOverrides[static_cast<std::size_t>(a_idx)];
        spdlog::info("[LOC] location=\"{}\" keywords=[{}] -> \"{}\" ({}) applying={} indoor={}",
                     LocationName(a_loc), KeywordsJoined(a_loc), lo.name, a_reason,
                     applying, s.indoorMode);
    }

    std::string LocationDetector::CurrentLocationName() const
    {
        // Qualified, like the bind row's own label: this is the "you are here"
        // header, and it must not say "Dormitory" when there are two of them.
        // It is also what Bind All Rooms stamps as the rooms' parentName, so
        // the two halls' room lists get separate group headers for free.
        return DisplayLocationName(PlayerLocation());
    }

    bool LocationDetector::CellHasRooms() const
    {
        return !RoomMarkersOfCurrentCell().markers.empty();
    }

    std::vector<LocationDetector::RoomInfo> LocationDetector::CellRooms() const
    {
        std::vector<RoomInfo> out;
        const auto& cache = RoomMarkersOfCurrentCell();
        out.reserve(cache.markers.size());
        for (const auto& m : cache.markers) {
            RoomInfo r;
            r.plugin   = m.plugin;
            r.formID   = m.formID;
            r.label    = fmt::format("Room {}", m.index);
            r.cellName = cache.cellName;
            out.push_back(std::move(r));
        }
        return out;
    }

    RE::TESObjectREFR* LocationDetector::CurrentRoomRefr() const
    {
        const auto* rm = CurrentRoomMarker();
        return rm ? rm->refr : nullptr;
    }

    bool LocationDetector::CurrentRoom(RoomInfo& a_out) const
    {
        const auto* rm = CurrentRoomMarker();
        if (!rm) return false;
        const auto& cache = RoomMarkersOfCurrentCell();
        a_out.plugin   = rm->plugin;
        a_out.formID   = rm->formID;
        a_out.label    = fmt::format("Room {}", rm->index);
        a_out.cellName = cache.cellName;
        return true;
    }

    std::vector<LocationDetector::Option> LocationDetector::GetOptions() const
    {
        // AT MOST THREE rows, and the list is built to stay that short:
        //   1. this exact place,
        //   2. what kind of place it is - the most specific type it carries,
        //   3. the area it belongs to - its parent record (a hold, a city, or
        //      Apocrypha, depending on where you are).
        // Nothing broader. "All Dungeons" and "Skyrim" were both offered in an
        // earlier pass and both were rejected as noise: they are technically
        // valid scopes that nobody would ever choose, and they buried the rows
        // that mattered.
        std::vector<Option> out;
        auto* loc = PlayerLocation();

        // Interior-ness of the spot the player is standing in — the rows
        // that name the CURRENT place (room / cell / exact location / its
        // type keyword) inherit it; the broader area rows (hold, region)
        // are exterior scopes by construction.
        bool inInterior = false;
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            if (auto* c = p->GetParentCell()) inInterior = c->IsInteriorCell();
        }

        // 0. The ROOM the player is standing in — narrower than everything
        //    else, so it leads the list. Only offered when the cell's room
        //    bounds are actually testable (see the [ROOM] cache log).
        {
            RoomInfo ri{};
            if (CurrentRoom(ri)) {
                Option r;
                r.isRoom  = true;
                r.isInterior = true;
                r.label   = ri.label;
                r.detail  = "Only in this room";
                r.plugin  = ri.plugin;
                r.formID  = ri.formID;
                // Group under the location's display name when there is one,
                // else the cell's.
                const std::string ln = LocationName(loc);
                r.parentName = !ln.empty() ? ln : ri.cellName;
                out.push_back(std::move(r));   // see the note on `here` below
            }
        }

        // 1. This exact place. When the spot has no location record at all,
        //    an interior cell IS the exact place, and naming it beats telling
        //    the player there is nothing here.
        if (loc) {
            Option here;
            // Qualified when the record's name is shared with another location
            // in the load order — see DisplayLocationName. This is the string
            // the bind button shows AND the default name the place is created
            // with, so a "Dormitory" is born already telling you which one.
            here.label  = DisplayLocationName(loc);
            here.detail = "Only in this exact place";
            here.isInterior = inInterior;
            here.formID = loc->GetLocalFormID();
            if (auto* file = loc->GetFile(0)) here.plugin = file->fileName;
            // DISAMBIGUATOR. Location names are not unique: the College of
            // Winterhold gives BOTH student halls a location literally named
            // "Dormitory", so two bound places read identically in the list
            // with no way to tell which is which (user report 2026-08-23).
            // The CELL is what separates them ("Hall of Attainment" vs "Hall
            // of Countenance"), so carry it alongside the label and let the
            // bind row qualify the name when it would otherwise collide.
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* cell = player->GetParentCell()) {
                    const char* cn = cell->GetFullName();
                    if (cn && *cn && _stricmp(cn, here.label.c_str()) != 0)
                        here.parentName = cn;
                }
            }
            // Pushed even when the plugin / form ID couldn't be read. A row
            // dropped here was a place the player could SEE they were standing
            // in and could not bind, with no explanation — the bind row now
            // shows it disabled and says why (see the unbindable branch there).
            out.push_back(std::move(here));
        } else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell(); cell && cell->IsInteriorCell()) {
                Option room;
                room.isCell = true;
                room.isInterior = true;
                const char* cn = cell->GetFullName();
                room.label  = (cn && *cn) ? cn : "This room";
                room.detail = "Only in this exact place";
                room.formID = cell->GetLocalFormID();
                if (auto* file = cell->GetFile(0)) room.plugin = file->fileName;
                out.push_back(std::move(room));   // see the note on `here` above
            }
        }

        // 2. What kind of place it is - ONE row, the narrowest type this
        //    record carries. The current location's own keywords only; a
        //    parent's categories are the parent's business.
        if (loc) {
            const std::string k = MostSpecificKeyword(loc);
            if (!k.empty()) {
                Option cat;
                cat.isKeyword = true;
                cat.isInterior = inInterior;
                cat.keyword   = k;
                cat.label     = KeywordLabel(k);
                cat.detail    = "Anywhere like this - " + k;
                cat.depth     = 1;
                out.push_back(std::move(cat));
            }
        }

        // 3. The area it belongs to - its parent record. That is the hold
        //    for a dungeon, the city for a house in one, and Apocrypha for a
        //    Black Book chapter.
        //
        //    In open country there is no location and therefore no parent,
        //    so the hold is recovered from the nearest SETTLEMENT marker (see
        //    HoldByNearestSettlement). Climate region is the fallback under
        //    that, for a worldspace with no settlements at all.
        bool haveArea = false;
        RE::BGSLocation* area = loc ? AreaOf(loc) : nullptr;
        bool areaIsNearest = false;
        if (!area && !loc) {
            area = OpenCountryHold(loc);
            areaIsNearest = (area != nullptr);
        }
        if (area && area != loc) {
            Option h;
            h.label  = DisplayLocationName(area);
            h.detail = areaIsNearest ? "Everywhere in this hold"
                                     : "Everywhere in this area";
            h.depth  = 2;
            h.formID = area->GetLocalFormID();
            if (auto* file = area->GetFile(0)) h.plugin = file->fileName;
            if (!h.plugin.empty() && h.formID != 0) { out.push_back(std::move(h)); haveArea = true; }
        }
        //    Last resort for the area row: a worldspace with no settlements to
        //    triangulate from (a Daedric realm, a mod's own map) still has
        //    CLIMATE regions, and those are real polygons rather than a guess.
        if (!haveArea) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* reg = ClimateRegionOf(player->GetParentCell())) {
                    Option r;
                    r.isRegion = true;
                    r.label    = RegionLabel(reg);
                    r.detail   = "Everywhere in this part of the world";
                    r.depth    = 2;
                    r.formID   = reg->GetLocalFormID();
                    if (auto* file = reg->GetFile(0)) r.plugin = file->fileName;
                    if (!r.plugin.empty() && r.formID != 0) out.push_back(std::move(r));
                }
            }
        }

        // Drop any row whose LABEL repeats one already offered.
        //
        // A place and its parent can carry the same display name - Whiterun the
        // city and the hold it belongs to both read "Whiterun" - and two
        // identical buttons is worse than one, because there is no way to tell
        // which is which or what picking either would do. The list is built
        // narrowest-first, so the first occurrence is the more specific one and
        // is the one worth keeping.
        {
            std::vector<Option> unique;
            unique.reserve(out.size());
            for (auto& o : out) {
                bool dup = false;
                for (const auto& u : unique)
                    if (_stricmp(u.label.c_str(), o.label.c_str()) == 0) { dup = true; break; }
                if (!dup) unique.push_back(std::move(o));
            }
            out.swap(unique);
        }

        // Absolute floor. Not a normal row - the map is far too broad to sit
        // alongside the three above - but open country genuinely has no
        // location record of any kind, and naming the map beats an empty list.
        if (out.empty()) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* ws = player->GetWorldspace()) {
                    Option w;
                    w.isWorldspace = true;
                    const char* wn = ws->GetFullName();
                    w.label  = (wn && *wn) ? wn : "This map";
                    w.detail = "Everywhere on this map";
                    w.depth  = 3;
                    w.formID = ws->GetLocalFormID();
                    if (auto* file = ws->GetFile(0)) w.plugin = file->fileName;
                    if (!w.plugin.empty() && w.formID != 0) out.push_back(std::move(w));
                }
            }
        }
        return out;
    }
}
