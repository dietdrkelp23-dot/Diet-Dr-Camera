#pragma once

#include <cmath>
#include <string_view>

namespace DietDrCamera::CollisionPolicy
{
    enum class Category { Other, Ground, Doors, Trees, Walls };
    enum class Form { Unknown, Static, MovableStatic, Door, Tree, Landscape, Other };
    enum class Layer { Other, Static, AnimatedStatic, Transparent, Trees, Terrain,
                       Ground, TransparentWall, InvisibleWall, CollisionBox, StairHelper, DoorDetection };

    struct Selection
    {
        bool ground{}, doors{}, trees{}, walls{};
        constexpr bool Any() const { return ground || doors || trees || walls; }
        constexpr bool Keeps(Category type) const
        {
            switch (type) {
            case Category::Ground: return ground;
            case Category::Doors: return doors;
            case Category::Trees: return trees;
            case Category::Walls: return walls;
            default: return false;
            }
        }
    };

    struct Object
    {
        Form form{};
        Layer layer{};
        bool treeLOD{};
        bool treeModel{};
    };

    // Whole directory components only: "architecture/whiterun/streets" and
    // "treehousewall.nif" must not become trees because of a substring match.
    inline bool IsTreeModel(std::string_view path)
    {
        auto equals = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const char c = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + ('a' - 'A')) : a[i];
                if (c != b[i]) return false;
            }
            return true;
        };
        while (!path.empty()) {
            const auto separator = path.find_first_of("/\\");
            if (separator == path.npos) break;  // filename, not a directory
            const auto part = path.substr(0, separator);
            if (equals(part, "trees") || equals(part, "tree")) return true;
            path.remove_prefix(separator + 1);
        }
        return false;
    }

    inline Category Classify(const Object& object, float normalZ)
    {
        // Object identity wins over the slope of its individual triangles.
        if (object.form == Form::Door) return Category::Doors;
        if (object.form == Form::Tree || object.layer == Layer::Trees ||
            object.treeLOD || object.treeModel) return Category::Trees;
        if (object.layer == Layer::DoorDetection) return Category::Doors;
        if (object.form == Form::Landscape || object.layer == Layer::Terrain ||
            object.layer == Layer::Ground) return Category::Ground;

        // A table, actor, loose item or animated activator is not a floor/wall
        // just because its mesh has a horizontal/vertical face. Unknown refs
        // may still be level geometry; for those, use the collision layer.
        if (object.form != Form::Unknown && object.form != Form::Static &&
            object.form != Form::MovableStatic) return Category::Other;
        const bool structure = object.layer == Layer::Static ||
            object.layer == Layer::AnimatedStatic || object.layer == Layer::Transparent ||
            object.layer == Layer::TransparentWall || object.layer == Layer::InvisibleWall ||
            object.layer == Layer::CollisionBox || object.layer == Layer::StairHelper;
        if (!structure || !std::isfinite(normalZ)) return Category::Other;
        // World-space surface normal, pointing from the obstacle to the camera.
        // Floors/ramps/stair treads and walls/ceilings can share one static mesh.
        return normalZ >= 0.5f ? Category::Ground : Category::Walls;
    }

    constexpr const char* Name(Category category)
    {
        switch (category) {
        case Category::Ground: return "Ground";
        case Category::Doors: return "Doors";
        case Category::Trees: return "Trees";
        case Category::Walls: return "Walls";
        default: return "Other";
        }
    }
}
