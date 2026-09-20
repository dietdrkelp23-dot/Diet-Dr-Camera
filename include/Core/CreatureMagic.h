#pragma once

#include "Core/DamageReactionAttacks.h"
#include <optional>

namespace DietDrCamera::CreatureMagic
{
    // Serialized game enums; the runtime adapter asserts the relevant values.
    // Keeping the descriptor independent of RE also lets real record fixtures
    // exercise the same decisions as the plugin.
    struct Effect {
        std::string_view name;
        int archetype = -1, primary = -1, secondary = -1, resistance = -1;
        int casting = 0, delivery = 0, school = -1;
        bool hostile = false, detrimental = false, missile = false, continuous = false;
        bool fire = false, frost = false, shock = false;
    };
    using Kind = DamageReaction::Kind;

    inline bool ValueEffect(int archetype)
    {
        return archetype == 0 || archetype == 4 || archetype == 5 || archetype == 31 ||
               archetype == 32 || archetype == 34;
    }
    inline bool AffectsHealth(const Effect& e)
    {
        return ValueEffect(e.archetype) && (e.primary == 24 || (e.archetype == 5 && e.secondary == 24));
    }
    inline bool Control(const Effect& e)
    {
        return e.archetype == 9 || e.archetype == 21 || e.archetype == 30 ||
               e.archetype == 33 || e.archetype == 45 ||
               (ValueEffect(e.archetype) && e.primary == 30); // SpeedMult (webs/slime)
    }
    inline bool Harmful(const Effect& e)
    {
        if (!e.hostile) return false;
        if (AffectsHealth(e) || Control(e)) return true;
        if (ValueEffect(e.archetype)) return e.primary == 25 || e.primary == 26;
        // A hostile projectile may deliver scripted damage instead of an AV.
        return e.archetype == 1 && e.missile;
    }
    inline Kind EffectKind(const Effect& e)
    {
        if (Control(e) && DamageReaction::Contains(e.name, "web")) return Kind::Web;
        if (e.resistance == 41 || e.fire) return Kind::Fire;
        if (e.resistance == 43 || e.frost) return Kind::Frost;
        if (e.resistance == 42 || e.shock) return Kind::Shock;
        if (e.resistance == 40) return Kind::Poison;
        if (e.archetype == 4 || (ValueEffect(e.archetype) && !AffectsHealth(e) && (e.primary == 25 || e.primary == 26))) return Kind::Drain;
        if (e.archetype == 33 || e.archetype == 9 || e.archetype == 30 || e.archetype == 45) return Kind::Force;
        return Kind::Magic;
    }
    struct Spell {
        std::optional<Kind> impact;
        bool harmful = false, continuous = false, health = false, slowImpact = false;
        int school = -1;
        int priority = -1;

        void Include(const Effect& e)
        {
            const bool harm = Harmful(e);
            if (e.school >= 0 && e.school < 5 && (school < 0 || harm)) school = e.school;
            if (!harm) return;
            harmful = true;
            health |= AffectsHealth(e);
            continuous |= e.casting == 2 || e.continuous;
            if (school < 0) school = Control(e) ? 0 : 2; // existing alteration/destruction tuning
            if (e.casting != 1 || e.delivery == 0 || e.continuous) return;
            // Direct damage without a projectile is already observed at its
            // value application. A control effect has no health tick at all.
            if ((!e.missile || e.delivery == 1) && AffectsHealth(e)) return;
            auto kind = EffectKind(e);
            if (kind == Kind::Poison && e.missile && e.delivery != 1) kind = Kind::VenomSpit;
            const int rank = kind == Kind::Web ? 4 : AffectsHealth(e) ? 3 : kind == Kind::Drain ? 2 : 1;
            if (rank > priority) {
                impact = kind; priority = rank;
                slowImpact = ValueEffect(e.archetype) && e.primary == 30 && kind != Kind::Web;
            }
        }
        std::optional<Kind> Impact(int casting, int spellType) const
        {
            // Neither a concentration projectile nor a multi-second breath
            // becomes a train of impacts. Diseases/abilities are not impacts.
            return continuous || (health && slowImpact) || casting != 1 || spellType == 1 || spellType == 4 ? std::nullopt : impact;
        }
    };
    inline bool CastNoiseAllowed(int casting, int type, int delivery, const Spell& info)
    {
        // A fire-and-forget ice storm/beam still has one release. Projectile
        // lifetime only excludes impact jolts; the caster's type gates noise.
        return casting == 1 && type != 1 && type != 4 &&
               delivery != 1 && (!info.harmful || delivery != 0);
    }
}
