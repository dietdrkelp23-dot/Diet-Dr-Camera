#pragma once

namespace DietDrCamera
{
    enum class CastType
    {
        None,
        Concentration,
        FireAndForget,
        Ritual
    };

    enum class MagicCastSource { Spell, Staff };

    // Staff enchantments do not carry a spell's two-handed ritual animation.
    // Use the costliest effect's master tier for their Ritual profile, including
    // aimed beams (Lightning Storm), summons and self buffs. Keep the existing
    // hand-spell rule; profile selection must not change native cast timing.
    // No plugin names or form IDs are involved.
    template <class MagicItem>
    CastType ResolveMagicCastType(const MagicItem* item,
                                 MagicCastSource source = MagicCastSource::Spell)
    {
        if (!item) return CastType::None;
        using EngineCast = decltype(item->GetCastingType());
        using Delivery = decltype(item->GetDelivery());
        const auto type = item->GetCastingType();
        if (type != EngineCast::kConcentration &&
            type != EngineCast::kFireAndForget) return CastType::None;

        const auto* effect = item->GetCostliestEffectItem();
        const auto* base = effect ? effect->baseEffect : nullptr;
        if (base && base->GetMinimumSkillLevel() >= 100 &&
            (source == MagicCastSource::Staff ||
             (type == EngineCast::kFireAndForget &&
              item->GetDelivery() == Delivery::kSelf && effect->GetArea() > 0))) {
            return CastType::Ritual;
        }
        return type == EngineCast::kConcentration
                   ? CastType::Concentration : CastType::FireAndForget;
    }

    // A second staff must not replace an active ritual with a lower-priority
    // concentration/ordinary cast just because its hand was visited last.
    constexpr bool PreferMagicCast(CastType candidate, CastType current)
    {
        return candidate != CastType::None &&
            static_cast<int>(candidate) >= static_cast<int>(current);
    }
}
