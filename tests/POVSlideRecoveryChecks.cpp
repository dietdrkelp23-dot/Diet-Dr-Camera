#include "Input/POVSlideRecovery.h"

#include <cstdlib>
#include <iostream>

using namespace DietDrCamera;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    for (const int fps : {30, 60, 144, 240}) {
        const float dt = 1.0f / fps;
        POVSlideRecovery recovery;
        int repairs = 0;
        bool slide = true;
        for (int frame = 0; frame < fps; ++frame) {
            if (recovery.Update(dt, slide, true, POVButtonState::Released)) {
                Check(frame * dt >= 0.15f && frame * dt < 0.25f, "Recovery delay changed with FPS");
                slide = false;  // the runtime clears the latch after repair
                ++repairs;
            }
        }
        Check(repairs == 1, "Released stale latch was not repaired once");

        for (const auto button : {POVButtonState::Held, POVButtonState::Unknown}) {
            for (int frame = 0; frame < fps * 10; ++frame)
                Check(!recovery.Update(dt, true, true, button), "Real/unknown hold was cancelled");
        }
        for (int frame = 0; frame < fps * 10; ++frame) {
            Check(!recovery.Update(dt, true, false, POVButtonState::Released),
                  "Menu, ordinary idle, or disabled gameplay triggered a repair");
            Check(!recovery.Update(dt, false, true, POVButtonState::Released),
                  "Unrelated movement stall triggered a repair");
        }
    }

    for (int interruption = 0; interruption < 4; ++interruption) {
        POVSlideRecovery recovery;
        for (int i = 0; i < 3; ++i)
            Check(!recovery.Update(0.05f, true, true, POVButtonState::Released), "Repaired too early");
        Check(!recovery.Update(0.05f, interruption != 0, interruption != 1,
                               interruption == 2 ? POVButtonState::Held :
                               interruption == 3 ? POVButtonState::Unknown : POVButtonState::Released),
              "Interrupting the evidence triggered repair");
        for (int i = 0; i < 3; ++i)
            Check(!recovery.Update(0.05f, true, true, POVButtonState::Released),
                  "Evidence survived a pause/repress/unknown read");
        Check(recovery.Update(0.05f, true, true, POVButtonState::Released), "Fresh evidence failed");
    }
    POVSlideRecovery hitch;
    Check(!hitch.Update(10.0f, true, true, POVButtonState::Released), "One long frame counted as confirmation");
    std::cout << "POV slide recovery checks passed\n";
}
