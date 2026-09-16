#include "Settings/CameraCollisionSettings.h"
#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace DietDrCamera;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

static unsigned Mask(const CameraCollisionEnvironment& environment)
{
    return (environment.disable ? 16 : 0) | (environment.keep.ground ? 1 : 0) |
           (environment.keep.doors ? 2 : 0) | (environment.keep.trees ? 4 : 0) |
           (environment.keep.walls ? 8 : 0);
}

static CameraCollisionEnvironment Environment(unsigned mask)
{
    return {bool(mask & 16), {bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)}};
}

int main()
{
    // Every independent pair of master/exception settings survives actual TOML
    // formatting and parsing, including saved exceptions with the master off.
    for (unsigned outdoor = 0; outdoor < 32; ++outdoor) {
        for (unsigned indoor = 0; indoor < 32; ++indoor) {
            CameraCollisionSettings settings;
            settings.outdoors = Environment(outdoor);
            settings.indoors = Environment(indoor);
            toml::table root;
            settings.Write(root);
            std::ostringstream encoded;
            encoded << root;
            CameraCollisionSettings loaded;
            loaded.Read(toml::parse(encoded.str()));
            Check(Mask(loaded.ForEnvironment(false)) == outdoor, "outdoor settings lost or copied from indoor");
            Check(Mask(loaded.ForEnvironment(true)) == indoor, "indoor settings lost or copied from outdoor");
            Check(!root.contains("general"), "writer resurrected shared collision keys");
            if (!(outdoor | indoor)) Check(root.empty(), "default settings are not sparse");
        }
    }

    // Legacy gates: neither means both environments, Outdoors wins if both set.
    for (unsigned master = 0; master < 2; ++master) {
        for (unsigned gates = 0; gates < 4; ++gates) {
            for (unsigned keep = 0; keep < 16; ++keep) {
                const bool outdoorsOnly = gates & 1, indoorsOnly = gates & 2;
                const toml::table legacy{{"general", toml::table{
                    {"camera_collision", bool(master)},
                    {"camera_collision_outdoors_only", outdoorsOnly},
                    {"camera_collision_indoors_only", indoorsOnly},
                    {"camera_keep_collision_ground", bool(keep & 1)},
                    {"camera_keep_collision_doors", bool(keep & 2)},
                    {"camera_keep_collision_trees", bool(keep & 4)},
                    {"camera_keep_collision_walls", bool(keep & 8)}}}};
                CameraCollisionSettings loaded;
                loaded.Read(legacy);
                const auto expectedOut = keep | ((master && (outdoorsOnly || !indoorsOnly)) ? 16 : 0);
                const auto expectedIn = keep | ((master && !outdoorsOnly) ? 16 : 0);
                Check(Mask(loaded.outdoors) == expectedOut && Mask(loaded.indoors) == expectedIn,
                    "migration changed effective collision settings");
                toml::table upgraded;
                loaded.Write(upgraded);
                CameraCollisionSettings reloaded;
                reloaded.Read(upgraded);
                Check(Mask(reloaded.outdoors) == expectedOut && Mask(reloaded.indoors) == expectedIn,
                    "migration failed after re-saving");
            }
        }
    }

    CameraCollisionSettings partial;
    partial.outdoors = Environment(17); // Ground, enabled
    partial.indoors = Environment(18);  // Doors, enabled
    partial.Read(toml::parse("[collision.outdoors]\nkeep_trees = true\n"));
    Check(Mask(partial.outdoors) == 21 && Mask(partial.indoors) == 18, "partial read leaked between groups");
    partial.Read(toml::parse("[collision.indoors]\ndisable = false\nkeep_doors = false\n"));
    Check(Mask(partial.outdoors) == 21 && Mask(partial.indoors) == 0, "explicit false was ignored");
    partial.Read(toml::parse("[general]\ncamera_collision_trees = true\n"));
    Check(Mask(partial.outdoors) == 21 && Mask(partial.indoors) == 0, "dormant key was repurposed");
    partial.Read(toml::parse("[general]\ncamera_collision = true\n[collision.indoors]\nkeep_walls = true\n"));
    Check(Mask(partial.outdoors) == 21 && Mask(partial.indoors) == 8, "legacy settings overrode new groups");
    partial = {};
    partial.Read(toml::parse("[general]\ncamera_collision = true\ncamera_keep_collision_doors = true\n"));
    partial.Read(toml::parse("[general]\ncamera_collision_outdoors_only = true\n"));
    Check(Mask(partial.outdoors) == 18 && Mask(partial.indoors) == 2, "partial legacy load lost prior values");
    partial = {};
    partial.Read(toml::table{});
    Check(Mask(partial.outdoors) == 0 && Mask(partial.indoors) == 0, "reset carried settings into another preset");
    std::cout << "Collision settings checks passed: 1024 environment pairs, 128 legacy migrations, partial reads and reset\n";
}
