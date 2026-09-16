#include "Settings/CameraProfile.h"
#include "Settings/OverrideInheritance.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>

using namespace DietDrCamera;
namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    try {
        CameraProfile parent;
        parent.zoom = 42;
        parent.transitionSetPosition = true;
        parent.transitionPosition = 0.23f;
        parent.SyncTransitionOverride();
        CameraProfile child;
        SeedOverrideFromParent(child, parent);
        auto expected = parent;
        expected.parentSeeded = true;
        Require(child == expected, "first enable must copy framing and transition settings");
        parent.zoom = 99;
        SeedOverrideFromParent(child, parent);
        Require(child.zoom == 42, "re-enabling cannot follow later parent edits");

        CameraProfile indoorChild;
        indoorChild.height = 123;
        SeedOverrideFromParent(indoorChild, parent);
        Require(indoorChild.height == 123 && indoorChild.zoom == CameraProfile{}.zoom,
            "previously tuned environment must retain its entire profile");
        CameraProfile defaultSnapshot;
        SeedOverrideFromParent(defaultSnapshot, CameraProfile{});
        Require(!NeedsParentSeed(defaultSnapshot), "a default-valued snapshot still owns its settings");
        Require(defaultSnapshot != CameraProfile{}, "sparse preset writers must retain initialized defaults");

        toml::table table;
        WriteOverrideSeed(table, defaultSnapshot);
        std::ostringstream saved;
        saved << table;
        CameraProfile restored;
        ReadOverrideSeed(toml::parse(saved.str()), restored);
        SeedOverrideFromParent(restored, parent);
        Require(restored.zoom == CameraProfile{}.zoom, "preset reload must retain a default-valued snapshot");
        CameraProfile legacyActive;
        ReadOverrideSeed(toml::parse("enabled = true"), legacyActive);
        SeedOverrideFromParent(legacyActive, parent);
        Require(legacyActive.zoom == CameraProfile{}.zoom, "active legacy override must keep its saved defaults");
        CameraProfile legacyUntouched;
        ReadOverrideSeed(toml::table{}, legacyUntouched);
        Require(NeedsParentSeed(legacyUntouched), "untouched legacy slot must remain eligible");

        auto copied = defaultSnapshot;
        SeedOverrideFromParent(copied, parent);
        Require(copied.zoom == CameraProfile{}.zoom, "clipboard-style profile copy must preserve ownership");
        copied = CameraProfile{};
        SeedOverrideFromParent(copied, parent);
        Require(copied.zoom == parent.zoom, "reset must allow a fresh parent copy");
        std::cout << "Override inheritance checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
