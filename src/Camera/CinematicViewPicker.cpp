#include "PCH.h"
#include "Camera/CinematicViewPicker.h"
#include "Camera/CinematicViewMath.h"
#include "Camera/WorldCameraRay.h"
#include "Core/CinematicSubject.h"
#include "RE/N/NiPick.h"
#include <queue>
#include <unordered_set>

namespace DietDrCamera::CinematicViewPicker
{
    namespace
    {
        using namespace CinematicViews;
        Point PointOf(RE::NiPoint3 p) { return {p.x,p.y,p.z}; }
        ItemBindings::FormIdentity Identity(RE::TESForm* form)
        {
            if (!form || form->IsDynamicForm()) return {};
            auto* file = form->GetFile(0);
            return file ? ItemBindings::FormIdentity{std::string{file->GetFilename()},form->GetLocalFormID()} : ItemBindings::FormIdentity{};
        }
        bool VisibleGeometry(RE::NiAVObject* object, unsigned depth = 0)
        {
            if (!object || object->GetAppCulled() || depth > 32) return false;
            if (object->AsGeometry() || object->AsNiGeometry()) return true;
            if (auto* node = object->AsNode()) for (const auto& child : node->GetChildren())
                if (VisibleGeometry(child.get(),depth+1)) return true;
            return false;
        }
        void Project(Subject& subject, const RE::NiTransform& camera, const RE::NiFrustum& frustum)
        {
            const auto p = subject.view.point;
            subject.onScreen = CinematicViewMath::Project(camera,frustum,{p.x,p.y,p.z},subject.screenX,subject.screenY);
        }
        struct SceneryHit {
            std::optional<RE::NiPoint3> point;
            float distance = CinematicSubject::kSceneryRange;
            const char* source = "none";
            unsigned nodes = 0, queries = 0;
            bool complete = true;
        };
        SceneryHit PickScenery(RE::TES* tes, RE::NiPick* pick, RE::NiPoint3 origin, RE::NiPoint3 forward)
        {
            SceneryHit hit;
            if (!tes || !pick) { hit.complete = false; return hit; }
            struct Candidate {
                RE::NiPointer<RE::NiAVObject> object;
                float entry;
                unsigned depth;
                const char* source;
            };
            const auto farther = [](const Candidate& a, const Candidate& b) { return a.entry > b.entry; };
            std::priority_queue<Candidate,std::vector<Candidate>,decltype(farther)> pending(farther);
            std::unordered_set<RE::NiAVObject*> visited;
            constexpr unsigned maxNodes = 8192, maxQueries = 512;
            const auto add = [&](RE::NiAVObject* object, unsigned depth, const char* source) {
                if (!hit.complete || !object || object->GetAppCulled() || visited.contains(object)) return;
                const auto& bound = object->worldBound;
                float entry = 0;
                if (bound.radius > 0 && std::isfinite(bound.radius) && PointOf(bound.center).Finite()) {
                    if (!CinematicSubject::RayMeetsBound(PointOf(origin),PointOf(forward),PointOf(bound.center),bound.radius,hit.distance)) return;
                    entry = (std::max)(0.0f,(bound.center-origin).Length()-bound.radius);
                }
                if (visited.size() >= maxNodes || depth > 64) { hit.complete = false; return; }
                visited.insert(object);
                pending.push({RE::NiPointer<RE::NiAVObject>{object},entry,depth,source});
            };
            add(tes->lodLandRoot,0,"terrain");
            add(tes->objLODWaterRoot,0,"water");
            add(tes->objRoot,0,"scene");
            while (!pending.empty() && hit.complete) {
                const auto current = pending.top(); pending.pop();
                if (current.entry > hit.distance) continue;
                ++hit.nodes;
                auto* object = current.object.get();
                if (auto* ref = object->GetUserData(); ref &&
                    (ref->IsDeleted() || ref->IsDisabled() || ref->As<RE::Actor>() || ref->As<RE::Projectile>())) continue;
                if (auto* node = object->AsNode()) {
                    for (const auto& child : node->GetChildren()) {
                        add(child.get(),current.depth+1,current.source);
                        if (!hit.complete) break;
                    }
                }
                if (!object->AsGeometry() && !object->AsNiGeometry()) continue;
                if (hit.queries >= maxQueries) { hit.complete = false; break; }
                ++hit.queries;
                pick->root.reset(object);
                if (!pick->PickObjects(origin,forward,false)) continue;
                const auto count = (std::min)(pick->pickResults.resultsCount,static_cast<std::uint32_t>(pick->pickResults.capacity()));
                for (std::uint32_t i=0; i<count; ++i) {
                    const auto* result = pick->pickResults[static_cast<std::uint16_t>(i)];
                    if (!result || !result->object) continue;
                    const auto distance = CinematicSubject::ForwardHitDistance(PointOf(origin),PointOf(forward),PointOf(result->intersect),hit.distance);
                    if (distance) { hit.point = result->intersect; hit.distance = *distance; hit.source = current.source; }
                }
            }
            // A truncated walk cannot establish that the saved point is the nearest surface.
            if (!hit.complete) hit.point.reset();
            return hit;
        }
    }

    Result Scan(RE::PlayerCharacter* player, const RE::NiTransform& camera, const RE::NiFrustum& frustum)
    {
        Result result;
        if (!player || !player->GetParentCell()) { result.status = "Load a game first."; return result; }
        auto* world = player->GetWorldspace();
        const auto space = Identity(world ? static_cast<RE::TESForm*>(world) : player->GetParentCell());
        if (!space.Valid()) { result.status = "Location unavailable."; return result; }
        const RE::NiPoint3 forward{camera.rotate.entry[0][0],camera.rotate.entry[1][0],camera.rotate.entry[2][0]};
        if (!PointOf(camera.translate).Finite() || !PointOf(forward).Finite() ||
            forward.Length() < .99f || forward.Length() > 1.01f) {
            result.status = "Close the menu and aim, then refresh.";
            return result;
        }
        const auto makeView = [&](RE::NiPoint3 point) {
            View view;
            view.space = space; view.exterior = world != nullptr; view.point = PointOf(point);
            view.triggerPoint = PointOf(player->GetPosition());
            return view;
        };
        struct Loaded {
            RE::ObjectRefHandle handle;
            Subject subject;
            float rayEntry = 0;
            bool intersects = false;
        };
        std::vector<Loaded> loaded;
        if (auto* tes = RE::TES::GetSingleton()) tes->ForEachReference([&](RE::TESObjectREFR* ref) {
            if (!ref || ref == player || ref->IsDeleted() || ref->IsDisabled() || ref->As<RE::Projectile>()) return RE::BSContainer::ForEachResult::kContinue;
            if (world ? ref->GetWorldspace() != world : ref->GetParentCell() != player->GetParentCell()) return RE::BSContainer::ForEachResult::kContinue;
            auto* node = ref->Get3D();
            if (!node) return RE::BSContainer::ForEachResult::kContinue;
            const auto& bound = node->worldBound;
            const float distance = (bound.center-camera.translate).Length();
            if (!std::isfinite(distance) || !std::isfinite(bound.radius) || bound.radius <= 0 ||
                distance-bound.radius > CinematicSubject::kRange || !VisibleGeometry(node)) return RE::BSContainer::ForEachResult::kContinue;
            Subject subject;
            subject.view = makeView(bound.center);
            subject.view.target = Identity(ref);
            subject.view.targetOffset = subject.view.target.Valid() ? PointOf(bound.center-ref->GetPosition()) : Point{};
            subject.key = fmt::format("reference-{:08X}",ref->GetFormID());
            subject.distance = (bound.center-player->GetPosition()).Length();
            auto* baseObject = ref->GetBaseObject();
            const auto* model = baseObject ? skyrim_cast<RE::TESModel*>(baseObject) : nullptr;
            const std::string path = model && model->GetModel() ? model->GetModel() : "";
            const auto* name = ref->GetName();
            subject.view.name = name && *name ? name : CinematicSubject::ModelName(path);
            const auto modelName = CinematicSubject::ModelName(path);
            subject.detail = subject.view.target.Valid() ? "Placed object" : "Temporary object - binds a fixed point";
            if (subject.view.target.Valid()) subject.detail += " | " + subject.view.target.plugin;
            if (modelName != subject.view.name && !path.empty()) subject.detail += " | " + modelName;
            subject.searchText = CinematicSubject::Fold(subject.view.name + " " + subject.detail + " " + path);
            Project(subject,camera,frustum);
            const bool intersects = CinematicSubject::RayMeetsBound(PointOf(camera.translate),PointOf(forward),PointOf(bound.center),bound.radius);
            loaded.push_back({ref->CreateRefHandle(),std::move(subject),(std::max)(0.0f,distance-bound.radius),intersects});
            return RE::BSContainer::ForEachResult::kContinue;
        });
        // Broad-phase bounds find even very large scenery whose origin is far
        // from the aim point. Exact mesh picks resolve overlapping references.
        std::sort(loaded.begin(),loaded.end(),[](const Loaded& a, const Loaded& b) {
            if (a.intersects != b.intersects) return a.intersects;
            return a.rayEntry < b.rayEntry;
        });
        auto pick = RE::NiPick::Create(8,8);
        if (pick) {
            pick->pickType = RE::NiPick::PickType::FIND_ALL;
            pick->sortType = RE::NiPick::SortType::SORT;
            pick->intersectType = RE::NiPick::IntersectType::TRIANGLE_INTERSECT;
            pick->coordinateType = RE::NiPick::CoordinateType::WORLD_COORDINATES;
            pick->frontOnly = false; pick->observeAppCullFlag = true;
            pick->returnNormal = pick->returnSmoothNormal = pick->returnTexture = pick->returnColor = false;
        }
        float nearest = CinematicSubject::kRange;
        Loaded* aimed = nullptr;
        unsigned queries = 0;
        for (auto& entry : loaded) {
            if (!entry.intersects || !pick || queries >= 128 || entry.rayEntry > nearest) break;
            const auto ref = entry.handle.get();
            auto* node = ref ? ref->Get3D() : nullptr;
            if (!node) continue;
            ++queries;
            pick->root.reset(node);
            if (!pick->PickObjects(camera.translate,forward,false)) continue;
            const auto count = (std::min)(pick->pickResults.resultsCount,static_cast<std::uint32_t>(pick->pickResults.capacity()));
            for (std::uint32_t i=0; i<count; ++i) {
                const auto* hit = pick->pickResults[static_cast<std::uint16_t>(i)];
                if (!hit || !hit->object || !PointOf(hit->intersect).Finite()) continue;
                const float distance = (hit->intersect-camera.translate).Length();
                if (distance <= .01f || distance >= nearest || (hit->intersect-camera.translate).Dot(forward) <= 0) continue;
                nearest = distance; aimed = &entry;
                entry.subject.view.point = PointOf(hit->intersect);
                entry.subject.view.targetOffset = entry.subject.view.target.Valid() ? PointOf(hit->intersect-ref->GetPosition()) : Point{};
            }
        }
        if (aimed) { aimed->subject.aimed = true; Project(aimed->subject,camera,frustum); }
        const auto scenery = world ? PickScenery(RE::TES::GetSingleton(),pick.get(),camera.translate,forward) : SceneryHit{};
        const auto physics = TraceCameraRay(player,camera.translate,camera.translate+forward*CinematicSubject::kRange,true);
        // Physics supplies a surface anchor for terrain and merged scenery;
        // it does not veto an exact visual pick with oversized collision.
        if (physics.valid && physics.hit && std::isfinite(physics.fraction)) {
            Subject point;
            point.key = "surface";
            point.view = makeView(camera.translate+forward*(CinematicSubject::kRange*physics.fraction));
            point.view.name = "Aimed Surface";
            point.detail = "Fixed point - useful for terrain and large structures";
            point.searchText = "aimed surface terrain landscape landmark fixed point";
            point.distance = (RE::NiPoint3{point.view.point.x,point.view.point.y,point.view.point.z}-player->GetPosition()).Length();
            point.aimed = aimed == nullptr;
            Project(point,camera,frustum);
            result.subjects.push_back(std::move(point));
            if (!aimed && physics.ref) for (auto& entry : loaded) {
                const auto ref = entry.handle.get();
                if (ref.get() != physics.ref) continue;
                entry.subject.aimed = true;
                entry.subject.view.point = result.subjects.front().view.point;
                if (entry.subject.view.target.Valid()) entry.subject.view.targetOffset = entry.subject.view.point-PointOf(ref->GetPosition());
                Project(entry.subject,camera,frustum);
                break;
            }
        }
        if (scenery.point) {
            Subject subject;
            subject.key = "scenery";
            subject.view = makeView(*scenery.point);
            subject.view.name = "Aimed Scenery";
            subject.distance = (std::max)(0.0f,(*scenery.point-player->GetPosition()).Length());
            subject.aimed = !aimed && scenery.distance <= CinematicSubject::kSceneryRange;
            // Exact visible geometry wins over a coarse physics surface for the fixed-point row.
            std::erase_if(result.subjects,[](const auto& item) { return item.key == "surface"; });
            Project(subject,camera,frustum);
            result.subjects.push_back(std::move(subject));
        }
        for (auto& entry : loaded) result.subjects.push_back(std::move(entry.subject));
        std::sort(result.subjects.begin(),result.subjects.end(),[](const Subject& a, const Subject& b) {
            if (a.aimed != b.aimed) return a.aimed;
            const bool aPoint = a.key == "surface" || a.key == "scenery";
            const bool bPoint = b.key == "surface" || b.key == "scenery";
            if (aPoint != bPoint) return aPoint;
            if (a.onScreen != b.onScreen) return a.onScreen;
            return a.distance < b.distance;
        });
        // Keep the surface fallback even in a dense city.
        if (result.subjects.size() > 256) {
            auto surface = std::find_if(result.subjects.begin()+255,result.subjects.end(),[](const auto& s) { return s.key == "surface" || s.key == "scenery"; });
            if (surface != result.subjects.end()) std::swap(result.subjects[255],*surface);
            result.subjects.resize(256);
        }
        result.status = result.subjects.empty() ? "No subjects nearby. Move closer and refresh." : "";
        spdlog::info("Cinematic Views: scanned {} loaded objects, {} mesh queries, {} selectable subjects; scenery nodes={} queries={} complete={} source={} distance={:.1f}",
            loaded.size(),queries,result.subjects.size(),scenery.nodes,scenery.queries,scenery.complete,scenery.source,scenery.point ? scenery.distance : 0);
        return result;
    }
}
