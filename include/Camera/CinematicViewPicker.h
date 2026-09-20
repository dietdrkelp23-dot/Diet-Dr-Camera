#pragma once
#include "Core/CinematicViews.h"
#include "RE/N/NiFrustum.h"
#include "RE/N/NiTransform.h"
#include <string>
#include <vector>

namespace RE { class PlayerCharacter; }
namespace DietDrCamera::CinematicViewPicker
{
    struct Subject {
        std::string key, detail, searchText;
        CinematicViews::View view;
        float distance = 0, screenX = 0, screenY = 0;
        bool aimed = false, onScreen = false;
    };
    struct Result {
        std::vector<Subject> subjects;
        std::string status;
    };
    // Run on the game thread only, on request. Snapshots contain values and
    // stable form identities, never scene pointers or live references.
    Result Scan(RE::PlayerCharacter* player, const RE::NiTransform& camera,
        const RE::NiFrustum& frustum);
}
