#pragma once

#include <optional>
#include <array>
#include <string_view>
#include <cctype>

namespace DietDrCamera::NpcNoise
{
    enum class Source { None, Magic, Melee, Archery, Transformations, Shouts };
    enum class Form { None, Werewolf, VampireLord };
    enum class Action { Attack, PowerAttack, SprintPowerAttack, Roar, Concentration, FireAndForget };

    inline bool ContainsCI(std::string_view text, std::string_view word)
    {
        for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
            bool match = true;
            for (std::size_t j = 0; j < word.size(); ++j)
                if (std::tolower(static_cast<unsigned char>(text[i + j])) !=
                    std::tolower(static_cast<unsigned char>(word[j]))) { match = false; break; }
            if (match) return true;
        }
        return false;
    }

    inline Form ClassifyForm(std::string_view raceEditorID, bool werewolfKeyword)
    {
        if (werewolfKeyword || ContainsCI(raceEditorID, "werewolf")) return Form::Werewolf;
        if (ContainsCI(raceEditorID, "vampirebeast") || ContainsCI(raceEditorID, "vampirelord"))
            return Form::VampireLord;
        return Form::None;
    }

    inline Source CombatSource(Form form, bool archery)
    {
        return form != Form::None ? Source::Transformations : archery ? Source::Archery : Source::Melee;
    }

    inline bool MeleeAllowed(Form form, bool voiceActive, bool handSpellEquipped)
    {
        if (form == Form::Werewolf) return !voiceActive;
        if (form == Form::VampireLord) return !handSpellEquipped;
        return true;
    }

    struct FormChange { Form form; bool revert; };
    inline std::optional<FormChange> TransformationChange(Form previous, bool known, Form current)
    {
        if (known && previous == current) return {};
        if (current != Form::None) return FormChange{current, false};
        if (known && previous != Form::None) return FormChange{previous, true};
        return {};
    }

    inline int TransformationSlot(Form form, bool revert)
    {
        return (form == Form::VampireLord ? 2 : 0) + (revert ? 1 : 0);
    }

    inline bool RefreshConcentration(bool running, float elapsed)
    {
        // Per-frame rearming would pin the envelope at its previous value.
        return !running || elapsed >= 0.2f;
    }

    inline float IntensityFor(Source source, float magic, float melee, float archery, float transformations, float shouts)
    {
        switch (source) {
        case Source::Magic: return magic;
        case Source::Melee: return melee;
        case Source::Archery: return archery;
        case Source::Transformations: return transformations;
        case Source::Shouts: return shouts;
        default: return 1.0f; // Player-owned event: no NPC multiplier/gate.
        }
    }

    inline std::array<std::string_view, 3> TransformationKeys(Form form, Action action)
    {
        if (form == Form::Werewolf) {
            switch (action) {
            case Action::SprintPowerAttack: return {"transformations.werewolf.sprint_power_attack", "transformations.werewolf.power_attack", "transformations.werewolf.attack"};
            case Action::PowerAttack: return {"transformations.werewolf.power_attack", "transformations.werewolf.attack", ""};
            case Action::Attack: return {"transformations.werewolf.attack", "", ""};
            case Action::Roar: return {"transformations.werewolf.roar", "", ""};
            default: return {};
            }
        }
        if (form == Form::VampireLord) {
            switch (action) {
            case Action::SprintPowerAttack:
            case Action::PowerAttack: return {"transformations.vampire_lord.melee.power_attack", "transformations.vampire_lord.melee.attack", ""};
            case Action::Attack: return {"transformations.vampire_lord.melee.attack", "", ""};
            case Action::Concentration: return {"transformations.vampire_lord.concentration", "transformations.vampire_lord.magic", ""};
            case Action::FireAndForget: return {"transformations.vampire_lord.fire_and_forget", "transformations.vampire_lord.magic", ""};
            default: return {};
            }
        }
        return {};
    }

    inline bool EnabledForEitherView(float thirdPerson, float firstPerson)
    {
        return thirdPerson > 0.0001f || firstPerson > 0.0001f;
    }

    inline bool AllowMagicSource(bool npcSource, float viewIntensity)
    {
        return !npcSource || viewIntensity > 0.0001f;
    }

    enum class MeleePhase { Idle, Draw, Swing, Hit, Recover, Bash, Ranged };

    struct MeleeAttack
    {
        bool power = false;
        bool sneak = false;
        bool sprint = false;
    };

    // One beat per swing, including combos which never return to idle. Draw
    // captures the stance; it is not a hit. No actor/settings pointers retained.
    class MeleeSwingTracker
    {
    public:
        std::optional<MeleeAttack> Observe(MeleePhase phase, bool power, bool sneak, bool sprint)
        {
            const bool drawing = phase == MeleePhase::Draw;
            const bool swing = phase == MeleePhase::Swing && previous != MeleePhase::Swing;
            const bool hit = phase == MeleePhase::Hit && previous != MeleePhase::Hit &&
                             previous != MeleePhase::Swing && previous != MeleePhase::Bash;
            const bool bash = phase == MeleePhase::Bash && previous != MeleePhase::Bash;
            if ((drawing && previous != MeleePhase::Draw) || ((swing || hit || bash) && !prepared)) {
                attack = {power, sneak || previousSneak, sprint || previousSprint};
                prepared = true;
            }
            std::optional<MeleeAttack> result;
            if (swing || hit || bash) {
                // Classify at the swing, when attackData describes this hit;
                // a windup may still expose the previous attack's power flag.
                attack.power = power;
                result = attack;
                prepared = false;
            }
            if (phase == MeleePhase::Idle || phase == MeleePhase::Recover || phase == MeleePhase::Ranged)
                prepared = false;
            previous = phase;
            previousSneak = sneak;
            previousSprint = sprint;
            return result;
        }

    private:
        MeleePhase previous = MeleePhase::Idle;
        MeleeAttack attack{};
        bool prepared = false;
        bool previousSneak = false;
        bool previousSprint = false;
    };
}
