#include "Camera/VanityIdleTimer.h"
#include "Camera/VanityInputActivity.h"
#include "Camera/VanityTransition.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void CheckLookActivity()
    {
        using namespace DietDrCamera;
        VanityInputActivity input;
        VanityIdleTimer timer;
        // Continuous mouse/controller look must postpone entry for a full idle
        // interval, but must leave an already active vanity profile in place.
        for (int fps : {30, 60, 120, 240}) {
            timer.Reset();
            for (int frame = 0; frame <= fps * 6; ++frame) {
                if (frame % 2 == 0) input.RecordLook();
                else input.RecordThumbstick(0.8f, -0.6f, true, "Look");
                const auto activity = input.Consume();
                Require(activity.any && !activity.endsVanity, "looking around must keep vanity active");
                Require(!timer.Tick(double(frame) / fps, 5, true, activity.any),
                        "looking around must still delay vanity entry");
            }
            for (int frame = 1; frame < fps * 5; ++frame)
                Require(!timer.Tick(6.0 + double(frame) / fps, 5, true, input.Consume().any),
                        "stopping look must start a fresh idle interval");
            Require(timer.Tick(11.01, 5, true, input.Consume().any),
                    "vanity must enter after looking stops for the configured delay");
        }

        // Action events must survive any number of look events in the same
        // camera frame, regardless of event order or controller mapping.
        for (bool actionFirst : {false, true}) {
            if (actionFirst) input.RecordAction();
            for (int i = 0; i < 100; ++i) input.RecordLook();
            if (!actionFirst) input.RecordAction();
            const auto activity = input.Consume();
            Require(activity.any && activity.endsVanity, "look must not erase a simultaneous exit action");
            Require(!input.Consume().any, "consumed activity must not replay on a later frame");
        }
        input.RecordThumbstick(0.5f, 0, false, "Move");
        Require(input.Consume().endsVanity, "movement stick must exit vanity");
        input.RecordThumbstick(0.5f, 0, false, "Look");
        Require(!input.Consume().endsVanity, "remapped left-stick look must keep vanity active");
        input.RecordThumbstick(0.5f, 0, true, "Move");
        Require(input.Consume().endsVanity, "remapped right-stick movement must exit vanity");
        input.RecordThumbstick(0.5f, 0, true, "");
        Require(!input.Consume().endsVanity, "unnamed right-stick look must keep vanity active");
        input.RecordThumbstick(0.5f, 0, false, "");
        Require(input.Consume().endsVanity, "unnamed left-stick movement must exit vanity");
        input.RecordThumbstick(0.25f, 0, false, "Move");
        input.RecordThumbstick(0, 0.1f, true, "Look");
        Require(!input.Consume().any, "controller drift must not count as input");
        input.RecordLook();
        input.RecordAction();
        input.Reset();
        const auto reset = input.Consume();
        Require(!reset.any && !reset.endsVanity, "load/reset must discard pending look and actions");
    }
}

int main()
{
    using namespace DietDrCamera;
    try {
        CheckLookActivity();
        for (int fps : {30, 60, 72, 120, 144, 240}) {
            VanityIdleTimer timer;
            Require(!timer.Tick(0, 5, true, false), "first sample cannot enter");
            for (int frame = 1; frame < fps * 5; ++frame)
                Require(!timer.Tick(double(frame) / fps, 5, true, false), "timer entered early");
            Require(timer.Tick(5.01, 5, true, false), "timer must enter at configured delay");
            Require(!timer.Tick(5.02, 5, true, true), "input must reset elapsed time");
        }
        VanityIdleTimer timer;
        for (int i = 0; i <= 49; ++i) timer.Tick(i * 0.1, 5, true, false);
        Require(std::abs(timer.Elapsed() - 4.9) < 0.001, "idle timer must track real banked time");
        Require(!timer.Tick(4.95, 5, false, false), "menu must reset timer");
        Require(timer.Elapsed() == 0, "a menu must discard banked time");
        Require(!timer.Tick(5.1, 5, true, false), "leaving menu must not bank idle time");
        Require(!timer.Tick(40, 5, true, false), "load or long stall must not trigger vanity");
        Require(!timer.Tick(1, 5, true, false), "backward clock must reset");
        Require(!timer.Tick(std::numeric_limits<double>::quiet_NaN(), 5, true, false), "nonfinite clock must reset");
        timer.Reset();
        Require(!timer.Tick(100, 5, true, false), "reset must discard timestamp");

        // Entry and return must use the identical settings source, even when
        // the receiving gameplay profile has conflicting channel overrides.
        auto vanity = CameraProfile::Default3p();
        auto gameplay = CameraProfile::Default3p();
        Require(vanity.rotation == 0 && vanity.pitchOffset == 0 && vanity.zoom == 10,
                "vanity must share the authored third-person baseline");
        vanity.SetTransitionAll(true);
        gameplay.SetTransitionAll(true);
        vanity.transitionPosition = 0.1f;
        vanity.transitionRotation = 0.2f;
        vanity.transitionPitch = 0.3f;
        vanity.transitionZoom = 0.4f;
        vanity.transitionFOV = 0.6f;
        vanity.transitionWeight = 0.7f;
        vanity.transitionLooseness = 0.8f;
        gameplay.sideOffset = -80;
        gameplay.rotation = 110;

        struct Channel {
            bool CameraProfile::* enabled;
            float CameraProfile::* value;
        };
        constexpr Channel channels[] = {
            {&CameraProfile::transitionSetPosition, &CameraProfile::transitionPosition},
            {&CameraProfile::transitionSetRotation, &CameraProfile::transitionRotation},
            {&CameraProfile::transitionSetPitch, &CameraProfile::transitionPitch},
            {&CameraProfile::transitionSetZoom, &CameraProfile::transitionZoom},
            {&CameraProfile::transitionSetFOV, &CameraProfile::transitionFOV},
            {&CameraProfile::transitionSetWeight, &CameraProfile::transitionWeight},
            {&CameraProfile::transitionSetLooseness, &CameraProfile::transitionLooseness},
        };
        VanityTransition transition;
        Require(&transition.Source(gameplay) == &gameplay, "ordinary third person must keep its own settings");
        transition.Update(true, vanity, gameplay, false);
        for (const auto channel : channels)
            Require(transition.Source(gameplay).*channel.value == vanity.*channel.value, "entry channel must use Vanity");
        // Live Quick Tune edits become the outgoing settings, not an entry-time copy.
        vanity.transitionZoom = 0.12f;
        transition.Update(true, vanity, gameplay, false);
        const auto outgoing = vanity;
        vanity.transitionZoom = 1.0f;
        transition.Update(false, vanity, gameplay, false);
        Require(transition.IsReturning(), "input exit must retain outgoing settings");
        for (const auto channel : channels)
            Require(transition.Source(gameplay).*channel.value == outgoing.*channel.value, "exit must retain every outgoing channel");
        Require(gameplay.sideOffset == -80 && gameplay.rotation == 110, "return must not overwrite the destination framing");
        for (int frame = 0; frame < 60 * 30; ++frame) transition.Update(false, vanity, gameplay, false);
        Require(transition.IsReturning(), "slow transitions must not switch speed on a timeout");
        transition.Update(false, vanity, gameplay, true);
        Require(!transition.IsReturning() && &transition.Source(gameplay) == &gameplay,
                "settled return must hand settings back to gameplay");

        // A slow close-up Vanity return must not lend its transition settings
        // to the next gameplay state. Reproduce exit -> sprint/cast/lock/indoor
        // before the original destination's channels have all settled.
        vanity.fov = 50;
        vanity.transitionFOV = 0.2f;
        gameplay.fov = 100;
        gameplay.transitionFOV = 0.5f;
        for (const float fov : {80.0f, 110.0f, 65.0f, 100.0f}) {
            auto next = gameplay;
            next.fov = fov;
            transition.Update(true, vanity, gameplay, false);
            transition.Update(false, vanity, gameplay, false);
            Require(transition.IsReturning() && transition.Source(gameplay).transitionFOV == 0.2f,
                    "initial exit must retain Vanity's configured return speed");
            transition.Update(false, vanity, next, false);
            Require(!transition.IsReturning() && &transition.Source(next) == &next,
                    "Vanity transition settings leaked into a subsequent gameplay profile");
            Require(next.fov == fov && gameplay.fov == 100 && vanity.fov == 50,
                    "interrupted Vanity return changed authored framing");
            transition.Update(false, vanity, gameplay, false);
            Require(!transition.IsReturning() && &transition.Source(gameplay) == &gameplay,
                    "returning to the old gameplay profile reactivated Vanity ownership");
        }

        // Unticked Vanity channels follow globals on BOTH legs, never a
        // receiving Sheathed/Sprint override halfway through the exit.
        for (const auto channel : channels) {
            vanity.SetTransitionAll(true);
            vanity.*channel.enabled = false;
            transition.Update(true, vanity, gameplay, false);
            Require(!(transition.Source(gameplay).*channel.enabled), "entry must preserve per-channel opt-out");
            transition.Update(false, vanity, gameplay, false);
            Require(!(transition.Source(gameplay).*channel.enabled), "exit must preserve per-channel opt-out");
        }
        vanity.SetTransitionAll(false);
        transition.Update(true, vanity, gameplay, false);
        transition.Update(false, vanity, gameplay, false);
        Require(!transition.Source(gameplay).transitionOverride, "global-only Vanity must remain global on return");

        transition.Update(true, outgoing, gameplay, false);
        Require(!transition.IsReturning() && transition.Source(gameplay).transitionZoom == outgoing.transitionZoom,
                "re-entry must replace the outgoing snapshot");
        transition.Reset();
        Require(&transition.Source(gameplay) == &gameplay, "load, menu or special camera must cancel ownership");
        transition.Update(false, vanity, gameplay, false);
        Require(!transition.IsReturning(), "reset must not synthesize another exit edge");
        std::cout << "Vanity camera checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
