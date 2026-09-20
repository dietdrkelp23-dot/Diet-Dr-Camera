#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace DietDrCamera::CinematicViews
{
    // A coarse collision hull can cover visible geometry on a different
    // reference (word effects against rock, for example). Check that hull's
    // triangles before treating it as an obstruction, then trace again with
    // only that reference excluded. A real wall behind it must still block.
    // trace returns {valid, hit, fraction, ref}; meshDistance returns an
    // optional distance, infinity for a mesh miss, or no value if unavailable.
    template <class Ref, class Trace, class IsSubject, class MeshDistance>
    bool ClearSightline(float distance, Trace&& trace, IsSubject&& isSubject, MeshDistance&& meshDistance)
    {
        if (!std::isfinite(distance) || distance < 0) return false;
        std::array<Ref,4> excluded{};
        for (std::size_t count=0; count<=excluded.size(); ++count) {
            const auto ignored = std::span<const Ref>{excluded.data(),count};
            const auto hit = trace(ignored);
            if (!hit.valid) return false;
            if (!hit.hit) return true;
            if (!std::isfinite(hit.fraction) || hit.fraction < 0 || hit.fraction > 1) return false;
            if (distance*(1-hit.fraction) <= 12 || isSubject(hit.ref)) return true;
            if (!hit.ref || count == excluded.size() || std::find(ignored.begin(),ignored.end(),hit.ref) != ignored.end()) return false;
            const auto visual = meshDistance(hit.ref);
            if (!visual || std::isnan(*visual) || *visual < 0 || *visual < distance-12) return false;
            excluded[count] = hit.ref;
        }
        return false;
    }
}
