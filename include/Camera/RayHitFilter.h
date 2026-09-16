#pragma once

#include <utility>

namespace DietDrCamera
{
    // A rejected hit must not shorten the query. Let Havok keep visiting
    // overlapping and farther bodies on the original, uninterrupted ray.
    template <class Collector, class Body, class Hit, class Ignore>
    class RayHitFilter final : public Collector
    {
    public:
        explicit RayHitFilter(Ignore ignore) : ignore_(std::move(ignore)) {}

        void AddRayHit(const Body& body, const Hit& hit) override
        {
            if (!ignore_(body)) Collector::AddRayHit(body, hit);
        }

    private:
        Ignore ignore_;
    };
}
