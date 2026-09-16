#pragma once

#include "Camera/CollisionPolicy.h"
#include <toml++/toml.hpp>

namespace DietDrCamera
{
    struct CameraCollisionEnvironment
    {
        bool disable{};
        CollisionPolicy::Selection keep{};
    };

    struct CameraCollisionSettings
    {
        CameraCollisionEnvironment outdoors, indoors;

        const CameraCollisionEnvironment& ForEnvironment(bool interior) const
        {
            return interior ? indoors : outdoors;
        }

        void Read(const toml::table& root)
        {
            if (const auto* collision = root["collision"].as_table()) {
                const auto read = [](const toml::table* table, CameraCollisionEnvironment& environment) {
                    if (!table) return;
                    environment.disable = (*table)["disable"].value_or(environment.disable);
                    environment.keep.ground = (*table)["keep_ground"].value_or(environment.keep.ground);
                    environment.keep.doors = (*table)["keep_doors"].value_or(environment.keep.doors);
                    environment.keep.trees = (*table)["keep_trees"].value_or(environment.keep.trees);
                    environment.keep.walls = (*table)["keep_walls"].value_or(environment.keep.walls);
                };
                read((*collision)["outdoors"].as_table(), outdoors);
                read((*collision)["indoors"].as_table(), indoors);
                return; // New groups are authoritative if legacy keys coexist.
            }

            // Migrate the original shared settings without changing which cells
            // disable collision. Missing keys keep their values on partial reads.
            // The dormant camera_collision_trees/props/actors keys are unrelated.
            const auto* general = root["general"].as_table();
            if (!general) return;
            bool present = false;
            const auto readLegacy = [&](const char* key, bool& value) {
                if (const auto loaded = (*general)[key].value<bool>()) {
                    value = *loaded;
                    present = true;
                }
            };
            readLegacy("camera_collision", _legacy.disable);
            readLegacy("camera_keep_collision_ground", _legacy.keep.ground);
            readLegacy("camera_keep_collision_doors", _legacy.keep.doors);
            readLegacy("camera_keep_collision_trees", _legacy.keep.trees);
            readLegacy("camera_keep_collision_walls", _legacy.keep.walls);
            readLegacy("camera_collision_outdoors_only", _legacyOutdoorsOnly);
            readLegacy("camera_collision_indoors_only", _legacyIndoorsOnly);
            if (!present) return;
            // Outdoors Only had priority if a hand-edited file set both gates.
            outdoors = {_legacy.disable && (_legacyOutdoorsOnly || !_legacyIndoorsOnly), _legacy.keep};
            indoors = {_legacy.disable && !_legacyOutdoorsOnly, _legacy.keep};
        }

        void Write(toml::table& root) const
        {
            toml::table collision;
            const auto write = [&](const char* name, const CameraCollisionEnvironment& environment) {
                toml::table table;
                if (environment.disable) table.insert("disable", true);
                if (environment.keep.ground) table.insert("keep_ground", true);
                if (environment.keep.doors) table.insert("keep_doors", true);
                if (environment.keep.trees) table.insert("keep_trees", true);
                if (environment.keep.walls) table.insert("keep_walls", true);
                if (!table.empty()) collision.insert(name, std::move(table));
            };
            write("outdoors", outdoors);
            write("indoors", indoors);
            if (!collision.empty()) root.insert("collision", std::move(collision));
        }

    private:
        CameraCollisionEnvironment _legacy{};
        bool _legacyOutdoorsOnly{}, _legacyIndoorsOnly{};
    };
}
