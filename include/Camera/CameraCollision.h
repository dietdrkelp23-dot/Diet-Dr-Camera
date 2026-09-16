#pragma once

#include "Camera/CollisionPolicy.h"

namespace RE { class hkpCollidable; struct hkpCdPoint; }

namespace DietDrCamera::CameraCollision
{
    void Install();

    // Stack/thread-local context around the original camera sweep only. Other
    // physics queries and the world's collision filters are unaffected.
    class ScopedFilter
    {
    public:
        explicit ScopedFilter(CollisionPolicy::Selection selection);
        ~ScopedFilter();
        ScopedFilter(const ScopedFilter&) = delete;
        ScopedFilter& operator=(const ScopedFilter&) = delete;

        bool Keep(const RE::hkpCdPoint& contact, const RE::hkpCollidable* camera);
        void OnQuery() { ++_queries; }
        unsigned Queries() const { return _queries; }

    private:
        CollisionPolicy::Selection _selection;
        ScopedFilter* _previous{};
        unsigned _queries{};
    };
}
