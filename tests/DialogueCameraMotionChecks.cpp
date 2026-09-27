#include "Dialogue/DialogueCameraMotion.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(float a, float b, float tolerance = 0.00002f)
    {
        return std::abs(a - b) <= tolerance;
    }
}

int main()
{
    using DietDrCamera::StepDialogueReverse;
    using DietDrCamera::DialogueSideExit;

    // A 180-degree turn must accelerate out of rest. The previous first-order
    // blend moved almost nine degrees on the first 60 Hz frame.
    float blend = 0.0f, velocity = 0.0f;
    StepDialogueReverse(blend, velocity, true, 1.0f, 1.0f / 60.0f);
    Require(blend * 180.0f < 1.0f, "reverse shot kicked on its first frame");
    Require(blend > 0.0f && velocity > 0.0f, "reverse shot did not start");

    // Frame rate must not change the shot or its settle time. Both directions
    // cover ~95% in the requested second, while slower settings remain slower.
    float reference = 0.0f;
    for (int fps : { 30, 60, 72, 144 }) {
        blend = velocity = 0.0f;
        float previous = blend;
        for (int i = 0; i < fps; ++i) {
            StepDialogueReverse(blend, velocity, true, 1.0f, 1.0f / fps);
            Require(blend >= previous && blend <= 1.0f, "sweep overshot or reversed");
            previous = blend;
        }
        Require(blend > 0.94f && blend < 0.96f, "forward sweep settle time changed");
        if (reference > 0.0f) Require(Near(blend, reference), "sweep depends on frame rate");
        reference = blend;
        blend = 1.0f; velocity = 0.0f;
        for (int i = 0; i < fps; ++i)
            StepDialogueReverse(blend, velocity, false, 1.0f, 1.0f / fps);
        Require(Near(blend, 1.0f - reference), "return sweep has different timing");
        blend = velocity = 0.0f;
        for (int i = 0; i < fps; ++i)
            StepDialogueReverse(blend, velocity, true, 2.0f, 1.0f / fps);
        Require(blend < 0.70f && blend > 0.65f, "slower dialogue speed was ignored");
    }

    // A skipped line/rapid response can reverse the destination mid-sweep.
    // Its first instant must retain the old motion, then turn around smoothly.
    blend = velocity = 0.0f;
    for (int i = 0; i < 15; ++i)
        StepDialogueReverse(blend, velocity, true, 1.0f, 1.0f / 60.0f);
    const float before = blend, beforeVelocity = velocity;
    StepDialogueReverse(blend, velocity, false, 1.0f, 0.0f);
    Require(blend == before && velocity == beforeVelocity, "retarget reset live motion");
    StepDialogueReverse(blend, velocity, false, 1.0f, 0.001f);
    Require(blend > before && velocity > beforeVelocity * 0.95f,
        "retarget reversed velocity instantly");
    for (int i = 0; i < 1200; ++i) {
        StepDialogueReverse(blend, velocity, (i / 6) % 2 == 0, 1.0f, 1.0f / 60.0f);
        Require(std::isfinite(blend) && blend >= 0.0f && blend <= 1.0f,
            "rapid speaker changes destabilized the rig");
    }
    for (int i = 0; i < 600; ++i)
        StepDialogueReverse(blend, velocity, false, 1.0f, 1.0f / 60.0f);
    Require(blend == 0.0f && velocity == 0.0f, "sweep never settled");
    StepDialogueReverse(blend, velocity, true, 1.0f, 10.0f);
    Require(blend < 0.1f, "long frame jumped across the reverse shot");

    // Closing from either side or partway through a turn must preserve the
    // engine's actual lateral position on the first gameplay frame. The profile
    // may already have advanced before capture; its value is not the old shot.
    for (float displacement : { -210.0f, -35.0f, 0.0f, 190.0f }) {
        for (int fps : { 30, 60, 144 }) {
            DialogueSideExit exit;
            const float baseline = 42.0f;
            exit.Capture(baseline + displacement, baseline + displacement - 3.0f,
                baseline, 1.0f);
            const auto first = exit.Step(baseline, 1.0f / fps);
            Require(Near(first.expected, baseline + displacement) &&
                    Near(first.actual, baseline + displacement - 3.0f),
                "dialogue close discarded lateral framing");
            // Walking/sprinting changes the underlying profile during exit.
            // This must not restart the independent carry or resurrect it.
            float previousExtra = first.expected - baseline;
            for (int i = 1; i <= fps + 2; ++i) {
                const float liveBaseline = baseline - i * 0.2f;
                const auto sample = exit.Step(liveBaseline, 1.0f / fps);
                const float extra = sample.expected - liveBaseline;
                Require(std::abs(extra) <= std::abs(previousExtra) + 0.0002f,
                    "profile retarget restarted the exit displacement");
                previousExtra = extra;
            }
            const auto done = exit.Step(30.0f, 1.0f / fps);
            Require(Near(done.expected, 30.0f) && Near(done.actual, 30.0f),
                "reverse-shot offset leaked into gameplay");
        }
    }
    DialogueSideExit exit;
    exit.Capture(200.0f, 180.0f, 30.0f, 1.0f);
    const auto paused1 = exit.Step(30.0f, 0.0f);
    const auto paused2 = exit.Step(30.0f, 0.0f);
    Require(paused1.actual == paused2.actual, "paused exit moved");
    exit.Reset(); // new conversation, another menu, POV switch or game load
    const auto fresh = exit.Step(30.0f, 1.0f / 60.0f);
    Require(fresh.expected == 30.0f && fresh.actual == 30.0f,
        "old conversation offset survived reset");
    std::cout << "Dialogue camera motion checks passed\n";
}
