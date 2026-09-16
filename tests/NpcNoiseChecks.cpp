#include "Camera/NpcNoise.h"
#include "Camera/NpcArchery.h"

#include <iostream>
#include <stdexcept>

using namespace DietDrCamera::NpcNoise;
using Phase = MeleePhase;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main() try
{
    Require(!EnabledForEitherView(0, 0), "Disabled NPC sources still scan");
    Require(EnabledForEitherView(0, 1), "First-person-only NPC noise cannot start");
    Require(EnabledForEitherView(1, 0), "Third-person-only NPC noise cannot start");
    Require(!AllowMagicSource(true, 0) && AllowMagicSource(true, 1),
            "NPC summons/reanimations ignore the current POV gate");
    Require(AllowMagicSource(false, 0), "NPC control mutes the player's own spell");
    Require(ClassifyForm("WerewolfBeastRace", false) == Form::Werewolf &&
            ClassifyForm("CustomBeastRace", true) == Form::Werewolf &&
            ClassifyForm("DLC1VampireBeastRace", false) == Form::VampireLord &&
            ClassifyForm("ModVampireLordRace", false) == Form::VampireLord &&
            ClassifyForm("NordRaceVampire", false) == Form::None,
            "NPC form routing confuses ordinary vampires or misses beast races");
    Require(CombatSource(Form::None, false) == Source::Melee &&
            CombatSource(Form::None, true) == Source::Archery &&
            CombatSource(Form::Werewolf, false) == Source::Transformations &&
            CombatSource(Form::VampireLord, false) == Source::Transformations,
            "Beast attacks leak into ordinary NPC controls");
    Require(!MeleeAllowed(Form::Werewolf, true, false) &&
            !MeleeAllowed(Form::VampireLord, false, true) &&
            MeleeAllowed(Form::VampireLord, false, false) &&
            MeleeAllowed(Form::None, false, true),
            "Howls or Vampire Lord spell mode trigger claw attacks, or mixed ordinary melee is muted");
    for (const auto source : {Source::Magic, Source::Melee, Source::Archery, Source::Transformations, Source::Shouts}) {
        const float value = IntensityFor(source, 1, 2, 3, 4, 5);
        Require(value == static_cast<int>(source), "NPC source uses a neighboring control");
        Require(IntensityFor(source, 0, 0, 0, 0, 0) == 0 && EnabledForEitherView(0, value),
                "Independent first-person NPC source is muted or leaks into third person");
    }
    for (const auto form : {Form::Werewolf, Form::VampireLord}) {
        auto entering = TransformationChange(Form::None, true, form);
        auto reverting = TransformationChange(form, true, Form::None);
        Require(entering && entering->form == form && !entering->revert &&
                reverting && reverting->form == form && reverting->revert,
                "NPC form transition or revert selects the wrong form");
        Require(!TransformationChange(form, true, form), "Race event and poll double-trigger a transformation");
        Require(TransformationSlot(form, true) == TransformationSlot(form, false) + 1,
                "Reverting uses the transformation entry");
    }
    Require(TransformationSlot(Form::Werewolf, false) == 0 &&
            TransformationSlot(Form::VampireLord, false) == 2, "Beast forms share cinematic settings");
    Require(!TransformationChange(Form::None, false, Form::None), "Unknown ordinary race creates a fake revert");
    for (int fps : {30, 60, 120, 240}) {
        bool running = false;
        float lastArm = 0;
        int arms = 0;
        for (int frame = 0; frame < fps; ++frame) {
            const float now = static_cast<float>(frame) / fps;
            if (RefreshConcentration(running, now - lastArm)) { ++arms; running = true; lastArm = now; }
        }
        Require(arms >= 4 && arms <= 5, "Concentration refresh pins the envelope or leaves gaps");
    }
    Require(!RefreshConcentration(true, 0), "Paused concentration re-arms continuously");

    Require(IsNewNpcArcheryShot(true, false, true, true), "NPC arrow/bolt spawn is ignored");
    Require(!IsNewNpcArcheryShot(false, false, true, true), "Flying arrow produces repeated beats");
    Require(!IsNewNpcArcheryShot(true, true, true, true), "Player arrow is counted as NPC noise");
    Require(!IsNewNpcArcheryShot(true, false, false, true) &&
            !IsNewNpcArcheryShot(true, false, true, false), "Non-archery or source-less projectile creates a beat");
    Require(FreshArcheryShot(0.01f) && !FreshArcheryShot(0.5f) && !FreshArcheryShot(-1),
            "Delayed shots can replay after a menu/load handoff");
    ArcheryShotQueue<int, 4> queue;
    std::array<int, 4> pending{};
    Require(!queue.Push(1), "Disabled archery retains a pending shot");
    queue.SetActive(true);
    for (int i = 0; i < 4; ++i) Require(queue.Push(i), "Simultaneous NPC shots are lost");
    Require(!queue.Push(4), "Shot queue exceeds its fixed bound");
    Require(queue.Drain(pending) == 4 && pending == std::array<int, 4>{0, 1, 2, 3},
            "Multiple shots overwrite or reorder one another");
    Require(queue.Drain(pending) == 0, "Projectile flight replays an already consumed shot");
    queue.Push(5);
    queue.SetActive(false);
    queue.SetActive(true);
    Require(queue.Drain(pending) == 0, "Disabling or suppressing archery retains an old shot");
    Require(queue.Push(6, 42) && !queue.Push(6, 42), "Repeated pre-movement updates duplicate an NPC shot");
    Require(queue.Drain(pending) == 1 && pending[0] == 6 && !queue.Push(6, 42),
            "The same projectile rearms after the camera drains its first update");
    Require(queue.Push(7, 43), "Deduplication blocks a distinct projectile from the same archer");
    queue.SetActive(false); queue.SetActive(true);
    Require(queue.Push(8, 42), "A load/source reset retains projectile IDs from the previous session");

    // Hold every engine phase for multiple camera frames. Hit follows Swing
    // (and Bash); it must not count as another attack.
    for (int dwell : {1, 2, 8, 30}) {
        MeleeSwingTracker tracker;
        int beats = 0;
        for (auto phase : {Phase::Idle, Phase::Draw, Phase::Swing, Phase::Hit, Phase::Recover,
                           Phase::Draw, Phase::Swing, Phase::Hit, Phase::Recover,
                           Phase::Bash, Phase::Hit, Phase::Idle}) {
            for (int frame = 0; frame < dwell; ++frame)
                beats += tracker.Observe(phase, false, false, false).has_value();
        }
        Require(beats == 3, "Attack phases repeat/drop beats or double-count bashes");
    }
    MeleeSwingTracker combo;
    int beats = 0;
    for (auto phase : {Phase::Swing, Phase::Hit, Phase::Swing, Phase::Hit,
                       Phase::Recover, Phase::Hit})
        beats += combo.Observe(phase, false, false, false).has_value();
    Require(beats == 3, "Combo without idle or skipped swing phase was missed");

    MeleeSwingTracker cancelled;
    Require(!cancelled.Observe(Phase::Draw, true, false, true), "Windup creates a premature hit");
    Require(!cancelled.Observe(Phase::Idle, false, false, false), "Cancelled attack creates a hit");
    auto normal = cancelled.Observe(Phase::Swing, false, false, false);
    Require(normal && !normal->power && !normal->sprint, "Cancelled power attack leaks into next swing");
    cancelled = {};
    cancelled.Observe(Phase::Idle, false, false, true);
    cancelled.Observe(Phase::Draw, false, false, false);
    auto sprint = cancelled.Observe(Phase::Swing, true, false, false);
    Require(sprint && sprint->sprint && sprint->power,
            "Sprint cancellation or late power flag loses the attack stance");
    cancelled.Observe(Phase::Recover, true, false, false);
    cancelled.Observe(Phase::Draw, true, false, false);
    normal = cancelled.Observe(Phase::Swing, false, false, false);
    Require(normal && !normal->power, "Previous power flag during windup misroutes a normal combo swing");

    MeleeSwingTracker first, second;
    first.Observe(Phase::Draw, true, true, false);
    second.Observe(Phase::Draw, false, false, false);
    auto firstHit = first.Observe(Phase::Hit, true, false, false);
    auto secondHit = second.Observe(Phase::Hit, false, false, false);
    Require(firstHit && firstHit->power && firstHit->sneak &&
            secondHit && !secondHit->power && !secondHit->sneak,
            "Nearby attackers share attack state");
    for (int frame = 0; frame < 100; ++frame)
        Require(!second.Observe(Phase::Ranged, true, false, false), "Ranged phases create melee beats");
    second = {}; // Same reset used when disabled or unloaded.
    Require(!second.Observe(Phase::Idle, false, false, false), "Reset retains a pending swing");
    std::cout << "NPC noise checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
