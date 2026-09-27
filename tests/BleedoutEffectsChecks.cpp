#include "Core/BleedoutEffects.h"
#include "Camera/BleedoutCameraPolicy.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace DietDrCamera::BleedoutEffects;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

static void SlowMotionChecks()
{
    for (const auto event : {Event::Death, Event::Ragdoll}) {
        for (bool disableDeath : {false, true}) for (bool disableRagdoll : {false, true}) {
            const bool disabled = DietDrCamera::BleedoutCameraPolicy::Disabled(
                event == Event::Death, disableDeath, disableRagdoll, false);
            SlowMotion effect;
            Check(effect.Enter(event, 60, 4), "First event was not armed");
            float speed = 1;
            for (int frame = 0; frame < 60; ++frame) {
                // Every frame can contain another native camera entry request.
                Check(!effect.Enter(event, 60, 4), "Repeated camera request restarted slow motion");
                speed = *effect.Advance(1.0f/60, false);
            }
            Check(std::abs(speed - .4f) < .00001f, disabled ?
                "Disabled camera lost its slow motion" : "Native camera lost its slow motion");
            for (int frame = 0; frame < 3600; ++frame)
                Check(*effect.Advance(1.0f/60, true) == speed, "Menu pause advanced the envelope");
            for (int frame = 0; frame < 240; ++frame) effect.Advance(1.0f/60, false);
            Check(!effect.Active() && !effect.Advance(.1f, false), "Completed effect kept owning global time");
            Check(!effect.Enter(event, 60, 4) && !effect.Active(), "Camera/menu return replayed a finished effect");
            effect.Reset();
            Check(effect.Enter(event, 60, 4) && effect.Active(), "Next knockdown/death did not start fresh");
        }
    }

    for (int fps : {30, 60, 120, 144, 240}) {
        SlowMotion effect;
        effect.Enter(Event::Death, 80, 4);
        float minimum = 1;
        float last = 1;
        for (int frame = 0; frame < fps * 5; ++frame) {
            if (const auto value = effect.Advance(1.0f/fps, false)) {
                Check(*value >= .1f && *value <= 1, "Envelope escaped safe speed bounds");
                minimum = (std::min)(minimum, *value);
                last = *value;
            }
            if (frame == fps * 3) Check(effect.Active(), "Effect duration changed with frame rate");
        }
        Check(std::abs(minimum - .2f) < .00001f && last == 1 && !effect.Active(),
            "Envelope peak or completion changed with frame rate");
    }

    // Fade during onset, the plateau, or the normal release must never deepen
    // the slowdown. Holding/repeating the hotkey must not extend the release.
    for (int beforeFade : {1, 40, 210}) {
        SlowMotion effect;
        effect.Enter(Event::Ragdoll, 90, 4);
        float previous = 1;
        for (int frame = 0; frame < beforeFade; ++frame) previous = *effect.Advance(1.0f/60, false);
        effect.RequestFade();
        for (int frame = 0; frame < 120; ++frame)
            Check(*effect.Advance(.1f, true) == previous, "Paused fade changed game speed");
        for (int frame = 0; frame < 70; ++frame) {
            effect.RequestFade();
            if (const auto speed = effect.Advance(1.0f/60, false)) {
                Check(*speed + .000001f >= previous, "Fade initially made slow motion stronger");
                previous = *speed;
            }
        }
        Check(!effect.Active() && previous == 1, "Fade never released normal time");
    }

    SlowMotion effect;
    effect.Enter(Event::Ragdoll, 80, 15);
    for (int frame = 0; frame < 30; ++frame) effect.Advance(.1f, false);
    Check(effect.Enter(Event::Death, 25, 1), "Death during ragdoll did not change effect context");
    for (int frame = 0; frame < 3; ++frame) effect.Advance(.1f, false);
    Check(std::abs(*effect.Advance(.1f, false) - .75f) < .00001f,
        "Death inherited the ragdoll's strength");
    effect.Enter(Event::None, 0, 0);
    Check(!effect.Active() && !effect.Advance(.1f, false), "Recovery retained effect ownership");
    for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        effect.Reset(); effect.Enter(Event::Death, invalid, 4);
        Check(!effect.Active(), "Invalid/off strength armed slow motion");
        effect.Reset(); effect.Enter(Event::Death, 50, invalid);
        Check(!effect.Active(), "Invalid/off duration armed slow motion");
    }
    effect.Reset(); effect.Enter(Event::Ragdoll, 50, 4);
    Check(*effect.Advance(std::numeric_limits<float>::quiet_NaN(), false) == 1,
        "Non-finite time contaminated the envelope");
    effect.Advance(60, false);
    Check(effect.Active(), "A blocked update consumed the entire effect duration");
}

int main()
{
    SlowMotionChecks();
    std::cout << "Independent death/ragdoll slow motion passed\n";
}
