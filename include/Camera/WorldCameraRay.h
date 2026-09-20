#pragma once
#include "Camera/RayHitFilter.h"
#include "RE/Skyrim.h"
#include "RE/H/hkpClosestRayHitCollector.h"
#include <algorithm>
#include <span>

namespace DietDrCamera
{
    struct CameraRayHit {
        bool valid = false, hit = false;
        float fraction = 1;
        RE::TESObjectREFR* ref = nullptr;
    };
    struct IgnoreCameraRayHit {
        bool actors = false;
        std::span<RE::TESObjectREFR* const> excluded{};
        bool operator()(const RE::hkpCdBody& body) const
        {
            const auto* root = &body;
            while (root->parent) root = root->parent;
            const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*static_cast<const RE::hkpCollidable*>(root));
            return ref && (ref == RE::PlayerCharacter::GetSingleton() || ref->As<RE::Projectile>() || (actors && ref->As<RE::Actor>()) ||
                std::find(excluded.begin(),excluded.end(),ref) != excluded.end());
        }
    };
    // Geometry visibility, independent of actor detection cones or screen FOV.
    // Keep hit references local to the call; never retain a raw reference pointer.
    inline CameraRayHit TraceCameraRay(RE::PlayerCharacter* player, RE::NiPoint3 from, RE::NiPoint3 to, bool ignoreActors = false,
        std::span<RE::TESObjectREFR* const> excluded = {})
    {
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* world = cell ? cell->GetbhkWorld() : nullptr;
        if (!world || !world->GetWorld1()) return {};
        if ((to-from).Length() < .01f) return {true};
        RayHitFilter<RE::hkpClosestRayHitCollector,RE::hkpCdBody,
            RE::hkpShapeRayCastCollectorOutput,IgnoreCameraRayHit> collector(IgnoreCameraRayHit{ignoreActors,excluded});
        RE::bhkPickData pick{};
        const float scale = RE::bhkWorld::GetWorldScale();
        pick.rayInput.from.quad = _mm_setr_ps(from.x*scale,from.y*scale,from.z*scale,0);
        pick.rayInput.to.quad = _mm_setr_ps(to.x*scale,to.y*scale,to.z*scale,0);
        pick.rayInput.filterInfo.filter = 0x40122716;
        pick.rayInput.enableShapeCollectionFilter = true;
        pick.ray.quad = _mm_setzero_ps();
        pick.closestRayHitCollector = &collector;
        world->PickObject(pick);
        if (!collector.HasHit()) return {true};
        const auto& hit = collector.rayHit;
        return {true,true,std::clamp(hit.hitFraction,0.0f,1.0f),hit.rootCollidable ?
            RE::TESHavokUtilities::FindCollidableRef(*hit.rootCollidable) : nullptr};
    }
}
