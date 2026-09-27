#pragma once

#include <RE/P/Projectile.h>

namespace DietDrCamera::ProjectileImpact
{
    // The engine consumes the returned ImpactData pointer at virtual slot BD.
    // CommonLib's void declaration is incomplete for flat SE/AE: the arrow,
    // missile, cone and beam implementations all return this result. Keep the
    // installed callback and displaced function types identical, and preserve
    // the pointer across any observer calls. Never dereference it afterward.
    using Result = RE::Projectile::ImpactData*;
    using Function = Result (*)(RE::Projectile*, RE::TESObjectREFR*,
        const RE::NiPoint3&, const RE::NiPoint3&, RE::hkpCollidable*, std::int32_t, std::uint32_t);
}
