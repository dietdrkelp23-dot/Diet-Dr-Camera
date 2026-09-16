#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "Locations/LocationMatchState.h"

namespace RE
{
    class BGSLocation;
    class TESObjectREFR;
}

namespace DietDrCamera
{
    class SettingsManager;

    // Decides which user-defined Location Override the player is currently
    // inside, and publishes the answer as SettingsManager::activeLocationIdx.
    //
    // Replaced CityDetector on 2026-08-02. The old detector answered one
    // yes/no question ("is this a city?") for a single global City environment
    // and had to bound the answer with a radius from the town's map marker,
    // because a location record covers whole CELLS and so reaches well past
    // anything that looks like the town. That radius is what failed in
    // Windhelm: parts of the city sit further from the marker than the radius
    // could cover without also swallowing the road outside. There is no
    // correct radius, because the game supplies a centre (worldLocMarker, set
    // on every settlement) but no extent (worldLocRadius is 0 on 757 of the
    // 763 vanilla locations - checked, not assumed).
    //
    // So the question changed instead of the tolerance. Each PLACE is one rule
    // read off the world - this location record, or one LocType keyword it
    // carries - and the answer is an index rather than a bool. No distance is
    // involved anywhere.
    //
    // MATCHING - walk the player's location PARENT CHAIN (Breezehome ->
    // Whiterun -> Whiterun Hold -> Tamriel) and take the FIRST ancestor any
    // enabled place matches. One rule, two properties:
    //   * generalisation: a place on "Whiterun" covers every interior, street
    //     and district inside it, because they are its children;
    //   * precision: a place on Whiterun beats one on Whiterun Hold, because
    //     the walk reaches the child first - which is how the city and the
    //     tundra around it end up with different settings.
    // Within one ancestor a named LOCATION beats a KEYWORD: naming a place is
    // a direct statement about it, a keyword is a category.
    //
    // Keyword rules are what make mod-added content work with no patch: a new
    // dungeon carries the same LocType keywords its author already applies for
    // the map, fast travel and radiant quests.
    class LocationDetector
    {
    public:
        static LocationDetector& GetSingleton();

        // Refresh SettingsManager::activeLocationIdx from the player's
        // location. Cheap: the full classification only re-runs when the
        // player's location pointer changes or the places are edited.
        void Update();

        // One thing the player could choose to apply settings to, read off the
        // spot they are standing in. The Location popup is a list of these,
        // narrowest first, and it is deliberately short:
        //   this place -> what kind of place it is -> its hold -> this map.
        // Nothing broader is offered; "All Holds" and "All of Tamriel" are
        // technically valid and useless to choose.
        struct Option
        {
            bool          isKeyword    = false;
            bool          isWorldspace = false;
            bool          isCell       = false;
            bool          isRegion     = false;
            // The room the player is standing in — the narrowest option of
            // all, offered FIRST whenever the interior has room bounds.
            bool          isRoom       = false;
            // True when the option names an INDOOR place: a room, an
            // interior cell, the interior's own location or its type
            // keyword. Holds/areas/regions/exterior places are false. The
            // Location popup's bind row filters on this against the
            // Outdoor/Indoor edit toggle (user ruling 2026-08-16: a place
            // bound from the Indoor toggle should BE an indoor place).
            bool          isInterior   = false;
            // Room only: grouping header for the bound-places list (the
            // location, or cell, this room belongs to).
            std::string   parentName;
            std::string   label;     // "Bleak Falls Barrow" / "All Nordic Dungeons"
            std::string   detail;    // the record or keyword it came from
            std::string   plugin;    // location rule
            std::uint32_t formID = 0;
            std::string   keyword;   // keyword rule (editor ID)
            int           depth  = 0;
        };

        // Everything the player could pick right now, narrowest first.
        // NEVER empty in a loaded game: open wilderness has no location record,
        // but it still has a hold (nearest hold marker) and a worldspace, and
        // "no location data" is not an acceptable answer to give the player.
        [[nodiscard]] std::vector<Option> GetOptions() const;

        // ---- Rooms --------------------------------------------------------
        // The portal-system ROOM BOUNDS of the current interior cell — the
        // boxes level designers author for occlusion. A room marker is a real
        // REFR with a stable form ID, so a room binds exactly like any other
        // location rule (Kind::Room, plugin + local form ID), just narrower
        // than everything else. Rooms are unnamed in the data; `label` is a
        // deterministic "Room N" from the marker's order in the cell, and the
        // bound place's own name field is where the user renames it.
        struct RoomInfo
        {
            std::string   plugin;
            std::uint32_t formID = 0;   // local form ID of the room-marker REFR
            std::string   label;        // "Room 3"
            std::string   cellName;     // the cell it belongs to (for default naming)
        };

        // True when the current cell carries any testable room bounds.
        [[nodiscard]] bool CellHasRooms() const;
        // Every room of the current cell, in stable "Room N" order — the
        // popup's list source (a bound room you are not standing in must
        // still be reachable to edit or unbind).
        [[nodiscard]] std::vector<RoomInfo> CellRooms() const;
        // The room the player is standing in right now. False when none
        // (no markers here, bounds not loaded, or standing in a gap).
        [[nodiscard]] bool CurrentRoom(RoomInfo& a_out) const;

        // The current room's marker REFR itself (nullptr when none).
        // Currently unconsumed (its one consumer, the SpaceProbe, was
        // removed with the cinematic-camera feature 2026-08-16); kept
        // because the room machinery behind it also serves CurrentRoom().
        [[nodiscard]] RE::TESObjectREFR* CurrentRoomRefr() const;

        // Display name of the location the player is in, or empty.
        [[nodiscard]] std::string CurrentLocationName() const;

        // Force the next Update() to reclassify. Called when the places change
        // so the world reacts without waiting for a cell change.
        void Invalidate() { _matchState.Invalidate(); }

    private:
        LocationDetector() = default;

        // Every place matching a_loc's ancestor chain, NARROWEST FIRST.
        // a_outReason describes the narrowest match.
        void Classify(RE::BGSLocation* a_loc, RE::BGSLocation* a_openCountryHold, std::vector<int>& a_out,
                      std::string* a_outReason) const;

        // Emit one [LOC] line whenever the resolved place changes.
        void LogChange(SettingsManager& a_settings, RE::BGSLocation* a_loc,
                       int a_idx, const std::string& a_reason);

        std::vector<int> _lastChain;
        LocationMatchState _matchState;
        // Room identity term of the cache key: crossing a room bound inside
        // one cell must reclassify even though the location pointer and the
        // weather never moved.
        std::uint32_t _lastRoomID   = 0;
        int         _lastIdx      = -1;
        int         _loggedIdx    = -2;   // != any valid index or -1
        bool        _loggedApply  = false;
    };
}
