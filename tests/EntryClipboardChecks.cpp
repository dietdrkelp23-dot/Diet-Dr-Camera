#include "Settings/CameraProfile.h"
#include "UI/EntryClipboardTarget.h"
#include "UI/TabClipboard.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static CameraProfile TunedProfile(float seed)
{
    CameraProfile p;
    p.sideOffset = seed;
    p.height = seed + 1.0f;
    p.zoom = seed + 2.0f;
    p.fov = seed + 60.0f;
    p.rotation = seed / 10.0f;
    p.pitchOffset = seed / 20.0f;
    p.transitionSetZoom = true;
    p.transitionZoom = 0.25f;
    p.transitionSetAimBias = true;
    p.transitionAimBias = 0.75f;
    p.transitionSetPitchBias = true;
    p.transitionPitchBias = -0.5f;
    p.SyncTransitionOverride();
    return p;
}

static void RequireProfile(const CameraProfile& actual, const CameraProfile& expected)
{
    Require(actual.sideOffset == expected.sideOffset && actual.height == expected.height &&
            actual.zoom == expected.zoom && actual.fov == expected.fov &&
            actual.rotation == expected.rotation && actual.pitchOffset == expected.pitchOffset &&
            actual.transitionSetZoom == expected.transitionSetZoom &&
            actual.transitionZoom == expected.transitionZoom &&
            actual.transitionSetAimBias == expected.transitionSetAimBias &&
            actual.transitionAimBias == expected.transitionAimBias &&
            actual.transitionSetPitchBias == expected.transitionSetPitchBias &&
            actual.transitionPitchBias == expected.transitionPitchBias &&
            actual.transitionOverride == expected.transitionOverride,
            "Enemy framing/transition settings did not reach the selected destination");
}

int main()
{
    try {
        const std::vector<std::string> sourceKeys{"Unsheathed/0", "Unsheathed/1", "Power Attack/0"};
        const std::vector<std::string> destinationKeys{"Unsheathed/1", "Drawing/0", "Unsheathed/0"};
        const auto pairs = MatchTabEntries(sourceKeys, destinationKeys);
        Require(pairs == std::vector<std::pair<std::size_t, std::size_t>>{{1,0},{0,2}},
            "Tab paste mixed environments or matched unrelated sub-states by position");
        Require(MatchTabEntries({"same", "same"}, {"same"}).empty() &&
            MatchTabEntries({"same"}, {"same", "same"}).empty(), "Ambiguous tab entries must not be pasted");
        struct LocationSlot { int locIdx; float tuning; };
        std::vector<LocationSlot> locations{{0, 12.0f}, {1, 24.0f}, {2, 36.0f}, {-1, 48.0f}};
        RemapTabLocationSlots(locations, {2, -1, 0});
        Require(locations[0].locIdx == 2 && locations[0].tuning == 12.0f &&
            locations[1].locIdx == -1 && locations[2].locIdx == 0 && locations[3].locIdx == -1,
            "Tab paste moved location tuning to another place after a preset reordered its locations");
        // Model the real render order: an enemy toolbar button is marked,
        // then each (unhovered) list row tries to attach its own storage.
        // The old frame-only guard replaced the button's coordinate with
        // the last row. Distinct profiles expose that misrouting on copy/paste.
        std::array<CameraProfile, 2> source{TunedProfile(23.0f), TunedProfile(47.0f)};
        std::array<CameraProfile, 2> destination{};
        std::array<CameraProfile, 2> destinationOtherEnvironment{};
        std::array<std::array<CameraProfile, 2>, 5> otherRows{};
        auto* coordinate = &source;
        EntryClipboardTarget owner{100, 0x1001};
        for (std::size_t row = 0; row < otherRows.size(); ++row) {
            auto* rowCoordinate = &otherRows[row];
            Require(!owner.Attach(100, 0x2000 + static_cast<std::uint32_t>(row), coordinate, rowCoordinate),
                    "Unhovered row replaced the enemy button's copy coordinate");
        }
        const auto clipboard = *coordinate;
        // A clipboard is a snapshot, even if the source is tuned afterwards.
        source[0].zoom = -3.0f;
        owner = {101, 0x1002};
        coordinate = &destination;
        for (std::size_t row = 0; row < otherRows.size(); ++row) {
            auto* rowCoordinate = &otherRows[row];
            Require(!owner.Attach(101, 0x3000 + static_cast<std::uint32_t>(row), coordinate, rowCoordinate),
                    "Unhovered row replaced the enemy button's paste coordinate");
        }
        *coordinate = clipboard;
        RequireProfile(destination[0], TunedProfile(23.0f));
        RequireProfile(destination[1], TunedProfile(47.0f));
        RequireProfile(destinationOtherEnvironment[0], CameraProfile{});
        for (const auto& row : otherRows) RequireProfile(row[0], CameraProfile{});

        // Pasting directly onto an entry must still attach THAT row's enemy
        // coordinate, while later rows and a different frame are rejected.
        owner = {102, 0x4001};
        coordinate = nullptr;
        auto* rowDestination = &destinationOtherEnvironment;
        Require(owner.Attach(102, 0x4001, coordinate, rowDestination), "Hovered row lost its enemy context");
        auto* unrelated = &otherRows[0];
        Require(!owner.Attach(102, 0x4002, coordinate, unrelated), "Later row stole the hovered row's context");
        Require(!owner.Attach(103, 0x4001, coordinate, unrelated), "Stale frame accepted context");
        *coordinate = clipboard;
        RequireProfile(destinationOtherEnvironment[1], TunedProfile(47.0f));
        Require(!EntryClipboardTarget{102, 0}.Attach(102, 0, coordinate, unrelated),
                "An item without an ID accepted context");

        // Enable flags use the same owner guard: hovering one row must not
        // silently switch on a different row's enable flag when pasting.
        bool selectedEnabled = false, otherEnabled = false;
        bool* enable = nullptr;
        bool* selectedFlag = &selectedEnabled;
        bool* otherFlag = &otherEnabled;
        Require(owner.Attach(102, 0x4001, enable, selectedFlag), "Selected enable flag was not attached");
        Require(!owner.Attach(102, 0x4002, enable, otherFlag), "Later row replaced the selected enable flag");
        *enable = true;
        Require(selectedEnabled && !otherEnabled, "Paste enabled the wrong row");
        std::cout << "PASS enemy button and row copy/paste ownership, profile snapshot, "
                     "environment destination, untouched rows, and enable flags\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
