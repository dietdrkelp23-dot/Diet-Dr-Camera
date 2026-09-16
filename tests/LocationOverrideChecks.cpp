#include "Locations/LocationMatchState.h"
#include "Settings/CameraProfile.h"
#include "Settings/LocationProfileIdentity.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DietDrCamera;

namespace
{
    void Check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    struct Location { Location* parent{}; };
    struct Place
    {
        bool enabled{true};
        std::vector<CameraProfile> profiles, profilesIndoor;
        std::vector<bool> profileSet, profileSetIndoor;
        std::unordered_map<std::string, CameraProfile> bindingCam, dlgLooks;
    };
}

int main()
{
    try {
        Location tamriel, rift{&tamriel}, riften{&rift}, inn{&riften}, otherHold{&tamriel};
        const auto ancestry = [](Location* exact, Location* fallback) {
            std::vector<Location*> result;
            VisitLocationAncestors(exact, fallback, [](Location* node) { return node->parent; },
                [&](Location* node) { result.push_back(node); });
            return result;
        };
        Check(ancestry(nullptr, &rift) == std::vector<Location*>{&rift, &tamriel},
            "a hold offered in open country was absent from runtime matching");
        Check(ancestry(&inn, &otherHold) == std::vector<Location*>{&inn, &riften, &rift, &tamriel},
            "nearest settlement displaced an actual location's parent chain");
        Check(ancestry(nullptr, nullptr).empty(), "missing world context invented a match");
        Location malformed;
        malformed.parent = &malformed;
        Check(ancestry(&malformed, nullptr).size() == 8, "malformed parent chain did not terminate");

        int cellA{}, cellB{}, roomA{}, roomB{}, regionA{}, regionB{}, worldA{}, worldB{};
        LocationMatchContext context{nullptr, &worldA, &cellA, 17, nullptr, &regionA, &rift};
        LocationMatchState cache;
        Check(cache.Refresh(context), "initial location was not classified");
        Check(!cache.Refresh(context), "unchanged context rebuilt every frame");
        // The user's exact sequence: bind The Rift while standing still; no
        // camera value, location pointer, cell or worldspace changes.
        cache.Invalidate();
        Check(cache.Refresh(context), "new binding required movement or a slider edit");
        for (unsigned term = 0; term < 7; ++term) {
            auto changed = context;
            switch (term) {
            case 0: changed.location = &riften; break;
            case 1: changed.worldspace = &worldB; break;
            case 2: changed.cell = &cellB; break;
            case 3: changed.cellID = 18; break; // recycled cell pointer
            case 4: changed.room = &roomA; break;
            case 5: changed.region = &regionB; break;
            case 6: changed.openCountryHold = &otherHold; break; // move within one cell
            }
            Check(cache.Refresh(changed), "location context change did not invalidate matching");
            Check(!cache.Refresh(changed), "refreshed context failed to cache");
            Check(cache.Refresh(context), "return to prior location did not refresh");
        }
        context.room = &roomA;
        cache.Refresh(context);
        context.room = &roomB;
        Check(cache.Refresh(context), "crossing a room boundary retained its previous bindings");
        // Reset, preset replacement and last-binding removal use this same
        // invalidation; a cached vector index must not survive those mutations.
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            cache.Invalidate();
            Check(cache.Refresh(context), "settings replacement reused the old match chain");
        }

        // Categories, Target Lock Melee Unsheathed, shouts, and mounted slots.
        // Every profile deliberately has identical values throughout this test.
        std::array<CameraProfile, 4> parents;
        std::vector<CameraProfile*> canonical;
        for (auto& parent : parents) canonical.push_back(&parent);
        std::vector<CameraProfile> indoors(parents.size());
        std::vector<Place> places(2);
        for (auto& place : places) {
            place.profiles.resize(parents.size());
            place.profilesIndoor.resize(parents.size());
            place.profileSet.resize(parents.size(), true);
            place.profileSetIndoor.resize(parents.size(), true);
        }
        const std::vector<int> chain{0, 1}; // inn before hold
        for (unsigned index = 0; index < parents.size(); ++index) {
            Check(CanonicalLocationProfile(&indoors[index], indoors, places, canonical) == &parents[index],
                "indoor variant lost its canonical entry");
            for (unsigned owner = 0; owner < places.size(); ++owner) {
                for (bool indoor : {false, true}) {
                    auto& place = places[owner];
                    auto& profiles = indoor ? place.profilesIndoor : place.profiles;
                    auto& enabled = indoor ? place.profileSetIndoor : place.profileSet;
                    auto* resolved = &profiles[index];
                    Check(*resolved == parents[index], "test accidentally requires a slider change");
                    Check(CanonicalLocationProfile(resolved, indoors, places, canonical) == &parents[index],
                        "location copy failed to identify its outdoor entry");
                    Check(ResolvedLocationProfileOwner(resolved, chain, places, indoor) == static_cast<int>(owner),
                        "unchanged newly bound copy failed to display its actual owner");
                    Check(ResolvedLocationProfileOwner(resolved, chain, places, !indoor) == -1,
                        "location binding leaked into the other environment");
                    enabled[index] = false;
                    Check(ResolvedLocationProfileOwner(resolved, chain, places, indoor) == -1,
                        "removed binding still claimed ownership");
                    enabled[index] = true;
                }
            }
            Check(ResolvedLocationProfileOwner(&parents[index], chain, places, false) == -1,
                "matching values alone made a base profile look location-owned");
        }
        for (const char* key : {"specific-weapon|tl", "animcam.4|tl"}) {
            auto& value = places[1].bindingCam[key];
            Check(ResolvedLocationProfileOwner(&value, chain, places, false) == 1,
                "keyed weapon/animation location copy did not display its owner");
        }
        auto& dialogue = places[1].dlgLooks["37"];
        Check(ResolvedLocationProfileOwner(&dialogue, chain, places, true) == 1,
            "dialogue location copy did not display its owner");
        places[1].enabled = false;
        Check(ResolvedLocationProfileOwner(&dialogue, chain, places, true) == -1,
            "disabled place retained the dialogue label");
        places[1].enabled = true;
        Check(ResolvedLocationProfileOwner(&dialogue, std::vector<int>{0}, places, true) == -1,
            "a place outside the active chain claimed the label");
        CameraProfile unrelated;
        Check(CanonicalLocationProfile(&unrelated, indoors, places, canonical) == &unrelated,
            "an unrelated profile was mistaken for a location copy");
        std::cout << "Location override checks passed: open-country activation, cache lifecycle, untouched profile identity, both environments and keyed owners\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
