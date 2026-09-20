#pragma once

#include "Core/CreatureMagic.h"

namespace DietDrCamera::CreatureMagic
{
    inline Effect Describe(const RE::EffectSetting* base)
    {
        if (!base) return {};
        static_assert(static_cast<int>(RE::ActorValue::kHealth) == 24);
        static_assert(static_cast<int>(RE::ActorValue::kSpeedMult) == 30);
        static_assert(static_cast<int>(RE::EffectArchetype::kStagger) == 33);
        static_assert(static_cast<int>(RE::MagicSystem::CastingType::kConcentration) == 2);
        Effect e;
        if (const auto* id = base->GetFormEditorID()) e.name = id;
        e.archetype = static_cast<int>(base->data.archetype);
        e.primary = static_cast<int>(base->data.primaryAV);
        e.secondary = static_cast<int>(base->data.secondaryAV);
        e.resistance = static_cast<int>(base->data.resistVariable);
        e.casting = static_cast<int>(base->data.castingType);
        e.delivery = static_cast<int>(base->data.delivery);
        const auto skill = base->GetMagickSkill();
        e.school = skill == RE::ActorValue::kAlteration ? 0 : skill == RE::ActorValue::kConjuration ? 1 :
            skill == RE::ActorValue::kDestruction ? 2 : skill == RE::ActorValue::kIllusion ? 3 :
            skill == RE::ActorValue::kRestoration ? 4 : -1;
        e.hostile = base->IsHostile(); e.detrimental = base->IsDetrimental();
        if (const auto* projectile = base->data.projectileBase) {
            e.missile = projectile->IsMissile();
            e.continuous = projectile->IsCone() || projectile->IsFlamethrower() || projectile->IsBarrier() ||
                projectile->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kContinuousUpdate);
        }
        e.fire = base->HasKeywordString("MagicDamageFire");
        e.frost = base->HasKeywordString("MagicDamageFrost");
        e.shock = base->HasKeywordString("MagicDamageShock");
        return e;
    }
    inline Spell Describe(const RE::MagicItem* magic)
    {
        Spell result;
        if (magic) for (const auto* effect : magic->effects)
            if (effect && effect->baseEffect) result.Include(Describe(effect->baseEffect));
        return result;
    }
    inline std::optional<Kind> Impact(RE::MagicItem* magic)
    {
        return magic ? Describe(magic).Impact(static_cast<int>(magic->GetCastingType()),
            static_cast<int>(magic->GetSpellType())) : std::nullopt;
    }
}
