#pragma once
#include "Core/CinematicViews.h"
#include "RE/N/NiFrustum.h"
#include "RE/N/NiMatrix3.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiTransform.h"

namespace DietDrCamera::CinematicViewMath
{
    inline bool Project(const RE::NiTransform& camera, const RE::NiFrustum& frustum, RE::NiPoint3 point, float& x, float& y)
    {
        const auto delta = point-camera.translate;
        const auto& r = camera.rotate;
        const float depth = delta.Dot({r.entry[0][0],r.entry[1][0],r.entry[2][0]});
        if (!std::isfinite(depth) || depth <= .01f || frustum.bOrtho || frustum.fNear <= 0 ||
            frustum.fRight <= frustum.fLeft || frustum.fTop <= frustum.fBottom) return false;
        const float side = delta.Dot({r.entry[0][2],r.entry[1][2],r.entry[2][2]})*frustum.fNear/depth;
        const float up = delta.Dot({r.entry[0][1],r.entry[1][1],r.entry[2][1]})*frustum.fNear/depth;
        x = (side-frustum.fLeft)/(frustum.fRight-frustum.fLeft);
        y = (frustum.fTop-up)/(frustum.fTop-frustum.fBottom);
        return std::isfinite(x) && std::isfinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1;
    }
}
