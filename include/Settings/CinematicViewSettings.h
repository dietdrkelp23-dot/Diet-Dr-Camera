#pragma once
#include "Core/CinematicViews.h"
#include <toml++/toml.hpp>
#include <unordered_set>
#include <vector>

namespace DietDrCamera::CinematicViews
{
    inline toml::table WriteIdentity(const ItemBindings::FormIdentity& form)
    {
        return toml::table{{"plugin",form.plugin},{"local_id",static_cast<std::int64_t>(form.localID)}};
    }
    inline toml::array WritePoint(Point p) { return toml::array{p.x,p.y,p.z}; }
    template<class WriteCamera>
    inline toml::array WriteViews(const std::vector<View>& views, WriteCamera writeCamera)
    {
        toml::array array;
        for (const auto& value : views) {
            const auto v = Sanitize(value);
            if (!Valid(v)) continue;
            toml::table row{
                {"id",v.id},{"name",v.name},{"space",WriteIdentity(v.space)},{"exterior",v.exterior},
                {"enabled",v.enabled},
                {"point",WritePoint(v.point)},{"target_offset",WritePoint(v.targetOffset)},
                {"radius",v.radius},{"idle_delay",v.idleDelay},{"lock_on_tightness",v.lockOnTightness},
                {"camera",writeCamera(v.profile)}};
            if (v.target.Valid()) row.insert("target",WriteIdentity(v.target));
            if (v.triggerPoint) row.insert("trigger_point",WritePoint(*v.triggerPoint));
            array.push_back(std::move(row));
        }
        return array;
    }
    template<class ReadCamera>
    inline bool ReadViews(toml::node_view<const toml::node> node, std::vector<View>& result, ReadCamera readCamera)
    {
        if (!node) return true;
        const auto* rows = node.as_array();
        if (!rows || rows->size() > 128) return false;
        std::vector<View> parsed;
        std::unordered_set<std::string> ids;
        const auto identity = [](auto n, ItemBindings::FormIdentity& out) {
            const auto plugin = n["plugin"].template value<std::string>();
            const auto local = n["local_id"].template value<std::int64_t>();
            if (!plugin || plugin->empty() || plugin->size() > 260 || !local || *local <= 0 || *local > 0xFFFFFF) return false;
            out = {*plugin,static_cast<std::uint32_t>(*local)};
            return true;
        };
        const auto point = [](auto n, Point& out) {
            const auto* values = n.as_array();
            if (!values || values->size() != 3) return false;
            const auto x = (*values)[0].template value<float>(), y = (*values)[1].template value<float>(), z = (*values)[2].template value<float>();
            if (!x || !y || !z) return false;
            out = {*x,*y,*z};
            return out.Finite();
        };
        for (const auto& item : *rows) {
            if (!item.is_table()) return false;
            const auto row = toml::node_view<const toml::node>{&item};
            View v;
            v.id = row["id"].value_or(std::string{});
            v.name = row["name"].value_or(std::string{});
            if (!identity(row["space"],v.space) || !point(row["point"],v.point)) return false;
            if (row["target"] && !identity(row["target"],v.target)) return false;
            if (row["target_offset"] && !point(row["target_offset"],v.targetOffset)) return false;
            if (row["trigger_point"]) {
                Point trigger;
                if (!point(row["trigger_point"],trigger)) return false;
                v.triggerPoint = trigger;
            }
            v.exterior = row["exterior"].value_or(v.exterior);
            v.enabled = row["enabled"].value_or(v.enabled);
            v.radius = row["radius"].value_or(row["range"].value_or(v.radius));
            v.idleDelay = row["idle_delay"].value_or(v.idleDelay);
            v.lockOnTightness = row["lock_on_tightness"].value_or(v.lockOnTightness);
            // Preserve bindings and native profiles. Prototype trigger, timing
            // and repeat controls are retired in favor of first radius entry
            // followed by this entry's independent idle timer.
            if (row["camera"]) {
                const auto* camera = row["camera"].as_table();
                if (!camera) return false;
                readCamera(*camera,v.profile);
            }
            v = Sanitize(v);
            if (!Valid(v) || !ids.insert(v.id).second) return false;
            parsed.push_back(std::move(v));
        }
        result = std::move(parsed);
        return true;
    }
}
