#include "Camera/MagicCast.h"

#include <cstdlib>
#include <iostream>

using namespace DietDrCamera;

namespace
{
    enum class Casting { kConstantEffect, kConcentration, kFireAndForget, kScroll };
    enum class Delivery { kSelf, kAimed, kTargetActor, kTargetLocation, kTouch };
    struct BaseEffect {
        unsigned skill = 100;
        unsigned GetMinimumSkillLevel() const { return skill; }
    };
    struct Effect {
        const BaseEffect* baseEffect;
        unsigned area = 25;
        unsigned GetArea() const { return area; }
    };
    // Exercise the production MagicItem classifier without an engine vtable.
    // Spells and staff enchantments supply this same interface.
    struct MagicItem {
        const Effect* effect;
        Casting casting = Casting::kFireAndForget;
        Delivery delivery = Delivery::kSelf;
        Casting GetCastingType() const { return casting; }
        Delivery GetDelivery() const { return delivery; }
        const Effect* GetCostliestEffectItem() const { return effect; }
    };
    void Check(bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; std::exit(1); }
    }
}

int main()
{
    BaseEffect base;
    Effect effect{&base};
    MagicItem item{&effect};
    // Preserve the existing hand-spell classification, including concentration
    // spells and zero-area self buffs, independently of the expanded staff rule.
    Check(ResolveMagicCastType(&item) == CastType::Ritual, "Master self-area spell was not a ritual");
    item.casting = Casting::kConcentration;
    Check(ResolveMagicCastType(&item) == CastType::Concentration, "Hand concentration spell changed category");
    item.casting = Casting::kFireAndForget;
    item.delivery = Delivery::kAimed;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Hand aimed spell changed category");
    item.delivery = Delivery::kTargetLocation;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Hand target-location spell changed category");
    item.delivery = Delivery::kSelf;
    base.skill = 75;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Lower-tier area effect became a ritual");
    base.skill = 100;
    effect.area = 0;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Self buff became a ritual");
    effect.area = 25;
    effect.baseEffect = nullptr;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Missing base effect was not handled");
    item.effect = nullptr;
    Check(ResolveMagicCastType(&item) == CastType::FireAndForget, "Missing effect was not handled");
    item.casting = Casting::kConstantEffect;
    Check(ResolveMagicCastType(&item) == CastType::None, "Constant enchantment became a cast");
    item.casting = Casting::kScroll;
    Check(ResolveMagicCastType(&item) == CastType::None, "Unsupported casting type became a cast");
    Check(ResolveMagicCastType<MagicItem>(nullptr) == CastType::None, "Null magic item was not handled");

    // Installed MysticismMagic.esp: MAG_LightningStormStaffEnch (local CF9FCC),
    // ENIT concentration + aimed, MAG_ShockDamageConcAimed100 minimum skill 100,
    // EFIT area 0. This was incorrectly routed to Destruction Concentration.
    effect.baseEffect = &base;
    effect.area = 0;
    base.skill = 100;
    item = {&effect, Casting::kConcentration, Delivery::kAimed};
    Check(ResolveMagicCastType(&item, MagicCastSource::Staff) == CastType::Ritual,
          "Lightning Storm staff did not select Ritual");
    Check(ResolveMagicCastType(&item) == CastType::Concentration,
          "Staff correction changed hand Lightning Storm classification");

    // All delivery shapes can be supplied by staff mods: beams, aimed or
    // placed summons, touch casts, self-area spells and zero-area self buffs.
    // None of the ordinary novice-through-expert staff casts may become Ritual.
    for (const auto casting : {Casting::kConcentration, Casting::kFireAndForget}) {
        item.casting = casting;
        for (const auto delivery : {Delivery::kSelf, Delivery::kAimed,
                 Delivery::kTargetActor, Delivery::kTargetLocation, Delivery::kTouch}) {
            item.delivery = delivery;
            for (const auto area : {0u, 25u}) {
                effect.area = area;
                for (const auto skill : {0u, 25u, 50u, 75u, 99u, 100u, 125u}) {
                    base.skill = skill;
                    const auto expected = skill >= 100 ? CastType::Ritual :
                        casting == Casting::kConcentration ? CastType::Concentration :
                        CastType::FireAndForget;
                    Check(ResolveMagicCastType(&item, MagicCastSource::Staff) == expected,
                          "Staff tier/delivery/casting regression");
                }
            }
        }
    }
    for (const auto casting : {Casting::kConcentration, Casting::kFireAndForget}) {
        item.casting = casting;
        const auto expected = casting == Casting::kConcentration
            ? CastType::Concentration : CastType::FireAndForget;
        effect.baseEffect = nullptr;
        Check(ResolveMagicCastType(&item, MagicCastSource::Staff) == expected,
              "Staff missing base effect lost native cast type");
        item.effect = nullptr;
        Check(ResolveMagicCastType(&item, MagicCastSource::Staff) == expected,
              "Staff missing effect lost native cast type");
        item.effect = &effect;
    }
    effect.baseEffect = &base;
    for (const auto casting : {Casting::kConstantEffect, Casting::kScroll}) {
        item.casting = casting;
        Check(ResolveMagicCastType(&item, MagicCastSource::Staff) == CastType::None,
              "Unsupported master enchantment became a staff cast");
    }
    Check(ResolveMagicCastType<MagicItem>(nullptr, MagicCastSource::Staff) == CastType::None,
          "Null staff enchantment was not handled");

    for (const auto lower : {CastType::Concentration, CastType::FireAndForget}) {
        Check(PreferMagicCast(CastType::Ritual, lower), "Second-hand ritual lost priority");
        Check(!PreferMagicCast(lower, CastType::Ritual), "Second-hand ordinary cast replaced ritual");
    }
    Check(PreferMagicCast(CastType::FireAndForget, CastType::Concentration), "Staff priority differs from spells");
    Check(!PreferMagicCast(CastType::None, CastType::None), "Idle caster replaced active staff data");
    std::cout << "Spell/staff ritual classification and hand priority checks passed\n";
}
