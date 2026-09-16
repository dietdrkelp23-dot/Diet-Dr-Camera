#include "Camera/CameraCollision.h"
#include "Camera/CollisionContactFilter.h"
#include <RE/H/hkpCdPoint.h>
#include <RE/H/hkpCdPointCollector.h>
#include <RE/H/hkpShapePhantom.h>
#include <RE/H/hkpWorldLinearCaster.h>
#include <RE/T/TESHavokUtilities.h>
#include <array>
#include <bit>

// CommonLib declares this resource-free Havok interface without C++ method
// definitions. Our stack-owned adapter needs its base destructor/reset. Engine
// collectors keep their original game vtables; these definitions do not patch it.
namespace RE
{
    hkpCdPointCollector::~hkpCdPointCollector() = default;
    void hkpCdPointCollector::Reset() { earlyOutDistance = std::bit_cast<float>(0x7F7FFFEEu); }
}

namespace DietDrCamera::CameraCollision
{
    namespace
    {
        thread_local ScopedFilter* activeFilter = nullptr;
        thread_local unsigned queryDepth = 0;

        const RE::hkpCollidable* Root(const RE::hkpCdBody* body)
        {
            for (unsigned depth = 0; body && depth < 64; ++depth) {
                if (!body->parent) return static_cast<const RE::hkpCollidable*>(body);
                body = body->parent;
            }
            return nullptr;
        }

        CollisionPolicy::Layer Layer(RE::COL_LAYER layer)
        {
            using L = CollisionPolicy::Layer;
            switch (layer) {
            case RE::COL_LAYER::kStatic: return L::Static;
            case RE::COL_LAYER::kAnimStatic: return L::AnimatedStatic;
            case RE::COL_LAYER::kTransparent: return L::Transparent;
            case RE::COL_LAYER::kTrees: return L::Trees;
            case RE::COL_LAYER::kTerrain: return L::Terrain;
            case RE::COL_LAYER::kGround: return L::Ground;
            case RE::COL_LAYER::kTransparentSmall: return L::TransparentWall;
            case RE::COL_LAYER::kInvisibleWall: return L::InvisibleWall;
            case RE::COL_LAYER::kCollisionBox: return L::CollisionBox;
            case RE::COL_LAYER::kStairHelper: return L::StairHelper;
            case RE::COL_LAYER::kDoorDetection: return L::DoorDetection;
            default: return L::Other;
            }
        }

        CollisionPolicy::Object Describe(const RE::hkpCollidable& body, RE::TESBoundObject*& base)
        {
            CollisionPolicy::Object result;
            result.layer = Layer(body.GetCollisionLayer());
            auto* ref = RE::TESHavokUtilities::FindCollidableRef(body);
            base = ref ? ref->GetBaseObject() : nullptr;
            if (!base) return result;
            using F = CollisionPolicy::Form;
            switch (base->GetFormType()) {
            case RE::FormType::Door: result.form = F::Door; break;
            case RE::FormType::Tree: result.form = F::Tree; break;
            case RE::FormType::Static:
            case RE::FormType::StaticCollection: result.form = F::Static; break;
            case RE::FormType::MovableStatic: result.form = F::MovableStatic; break;
            case RE::FormType::Land: result.form = F::Landscape; break;
            default: result.form = F::Other; break;
            }
            if (const auto* stat = base->As<RE::TESObjectSTAT>()) result.treeLOD = stat->HasTreeLOD();
            if (const auto* model = base->As<RE::TESModel>()) {
                if (const auto* path = model->GetModel()) result.treeModel = CollisionPolicy::IsTreeModel(path);
            }
            return result;
        }

        using FilteredCollector = CollisionContactFilter<RE::hkpCdPointCollector,
            RE::hkpCdPoint, ScopedFilter, const RE::hkpCollidable*>;

        struct QueryGuard
        {
            QueryGuard() { ++queryDepth; activeFilter->OnQuery(); }
            ~QueryGuard() { --queryDepth; }
        };

        using PositionCast = void(*)(RE::hkpShapePhantom*, const RE::hkVector4&,
            const RE::hkpLinearCastInput&, RE::hkpCdPointCollector&, RE::hkpCdPointCollector*);
        using TransformCast = void(*)(RE::hkpShapePhantom*, const RE::hkTransform&,
            const RE::hkpLinearCastInput&, RE::hkpCdPointCollector&, RE::hkpCdPointCollector*);
        using ClosestPoints = void(*)(RE::hkpShapePhantom*, RE::hkpCdPointCollector&, const RE::hkpCollisionInput*);
        std::array<REL::Relocation<PositionCast>, 2> positionCasts;
        std::array<REL::Relocation<TransformCast>, 2> transformCasts;
        std::array<REL::Relocation<ClosestPoints>, 2> closestPoints;

        using WorldCast = float(*)(RE::hkpWorldLinearCaster*, const RE::hkpBroadPhaseHandle*, std::int32_t);
        REL::Relocation<WorldCast> worldCast;

        float WorldCastHook(RE::hkpWorldLinearCaster* caster, const RE::hkpBroadPhaseHandle* handle, std::int32_t index)
        {
            if (!activeFilter || queryDepth) return worldCast(caster, handle, index);
            QueryGuard guard;
            static bool loggedWorld = false;
            if (!loggedWorld) {
                loggedWorld = true;
                spdlog::info("[COLLISION] native world sweep filtering active");
            }
            ScopedCollisionCollectors<RE::hkpCdPointCollector, RE::hkpCdPoint,
                ScopedFilter, const RE::hkpCollidable*> collectors(
                    caster->castCollector, caster->startPointCollector, *activeFilter, caster->collidableA);
            return worldCast(caster, handle, index);
        }

        template <unsigned Index>
        void PositionHook(RE::hkpShapePhantom* phantom, const RE::hkVector4& position,
            const RE::hkpLinearCastInput& input, RE::hkpCdPointCollector& cast, RE::hkpCdPointCollector* start)
        {
            if (!activeFilter || queryDepth) return positionCasts[Index](phantom, position, input, cast, start);
            QueryGuard guard;
            FilteredCollector filteredCast(&cast, *activeFilter, &phantom->collidable), filteredStart(start, *activeFilter, &phantom->collidable);
            positionCasts[Index](phantom, position, input, filteredCast, start ? &filteredStart : nullptr);
        }

        template <unsigned Index>
        void TransformHook(RE::hkpShapePhantom* phantom, const RE::hkTransform& transform,
            const RE::hkpLinearCastInput& input, RE::hkpCdPointCollector& cast, RE::hkpCdPointCollector* start)
        {
            if (!activeFilter || queryDepth) return transformCasts[Index](phantom, transform, input, cast, start);
            QueryGuard guard;
            FilteredCollector filteredCast(&cast, *activeFilter, &phantom->collidable), filteredStart(start, *activeFilter, &phantom->collidable);
            transformCasts[Index](phantom, transform, input, filteredCast, start ? &filteredStart : nullptr);
        }

        template <unsigned Index>
        void ClosestHook(RE::hkpShapePhantom* phantom, RE::hkpCdPointCollector& collector, const RE::hkpCollisionInput* input)
        {
            if (!activeFilter || queryDepth) return closestPoints[Index](phantom, collector, input);
            QueryGuard guard;
            FilteredCollector filtered(&collector, *activeFilter, &phantom->collidable);
            closestPoints[Index](phantom, filtered, input);
        }

        template <unsigned Index>
        void InstallPhantom(REL::VariantID table)
        {
            REL::Relocation<std::uintptr_t> vtable{table};
            positionCasts[Index] = vtable.write_vfunc(0xF, &PositionHook<Index>);
            transformCasts[Index] = vtable.write_vfunc(0x10, &TransformHook<Index>);
            closestPoints[Index] = vtable.write_vfunc(0x11, &ClosestHook<Index>);
        }
    }

    void Install()
    {
        InstallPhantom<0>(RE::VTABLE_hkpSimpleShapePhantom[0]);
        InstallPhantom<1>(RE::VTABLE_hkpCachingShapePhantom[0]);
        REL::Relocation<std::uintptr_t> worldTable{RE::VTABLE_hkpWorldLinearCaster[0]};
        worldCast = worldTable.write_vfunc(0x1, &WorldCastHook);
        spdlog::info("CameraCollision: world/phantom sweep contact filters installed (Ground/Doors/Trees/Walls)");
    }

    ScopedFilter::ScopedFilter(CollisionPolicy::Selection selection) : _selection(selection), _previous(activeFilter)
    {
        activeFilter = this;
    }

    ScopedFilter::~ScopedFilter() { activeFilter = _previous; }

    bool ScopedFilter::Keep(const RE::hkpCdPoint& contact, const RE::hkpCollidable* camera)
    {
        const auto* a = Root(contact.cdBodyA);
        const auto* b = Root(contact.cdBodyB);
        const RE::hkpCollidable* obstacle = nullptr;
        float sign = 1.0f;
        if (a == camera) obstacle = b;
        else if (b == camera) { obstacle = a; sign = -1.0f; }
        else {
            static thread_local bool loggedMismatch = false;
            if (!loggedMismatch) {
                loggedMismatch = true;
                spdlog::warn("[COLLISION] contact roots did not match the queried camera shape");
            }
            return false;
        }
        if (!obstacle || obstacle == camera) return false;

        float normal[4];
        _mm_storeu_ps(normal, contact.contact.separatingNormal.quad);
        RE::TESBoundObject* base = nullptr;
        const auto object = Describe(*obstacle, base);
        const auto category = CollisionPolicy::Classify(object, normal[2] * sign);
        const bool keep = _selection.Keeps(category);

        // One line per distinct base/category/decision/layer, bounded per session.
        // This makes modded static trees or unusual doors identifiable in a test.
        struct Logged { std::uint32_t form; RE::COL_LAYER layer; CollisionPolicy::Category category; bool keep; };
        static thread_local std::array<Logged, 128> logged{};
        static thread_local unsigned count = 0;
        const Logged key{base ? base->GetFormID() : 0, obstacle->GetCollisionLayer(), category, keep};
        if (count < logged.size()) {
            bool known = false;
            for (unsigned i = 0; i < count; ++i) {
                const auto& old = logged[i];
                if (old.form == key.form && old.layer == key.layer && old.category == key.category && old.keep == key.keep) {
                    known = true; break;
                }
            }
            if (!known) {
                logged[count++] = key;
                const auto* model = base ? base->As<RE::TESModel>() : nullptr;
                const auto* path = model ? model->GetModel() : nullptr;
                spdlog::info("[COLLISION] {} {} base={:08X} formType={} layer={} normalZ={:.3f} treeLOD={} treeModel={} model={}",
                    keep ? "keep" : "pass", CollisionPolicy::Name(category), key.form,
                    base ? static_cast<int>(base->GetFormType()) : -1, static_cast<int>(key.layer),
                    normal[2] * sign, object.treeLOD, object.treeModel, path ? path : "<none>");
            }
        }
        return keep;
    }
}
