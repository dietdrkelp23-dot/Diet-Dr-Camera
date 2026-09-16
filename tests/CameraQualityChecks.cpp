#include "Core/AtomicFile.h"
#include "Core/PauseTimeline.h"
#include "Core/EffectFrame.h"
#include "Core/Spring.h"

#include <Windows.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace DietDrCamera;
using namespace std::chrono_literals;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main()
{
    try {
        using Clock = PauseTimeline::Clock;
        const Clock::time_point origin{100s};
        PauseTimeline clock;
        Require(clock.Now(origin + 150ms) == origin + 150ms, "unpaused clock changed");
        clock.SetPaused(true, origin + 150ms);
        const auto held = clock.Now(origin + 150ms);
        clock.SetPaused(true, origin + 30s);
        Require(clock.Now(origin + 60s) == held, "pause advanced or re-armed");
        clock.SetPaused(false, origin + 60150ms);
        Require(clock.Now(origin + 60150ms) == held, "resume jumped");
        Require(clock.Now(origin + 60166ms) - held == 16ms, "resume caught up paused time");
        clock.SetPaused(false, origin + 60166ms);
        clock.SetPaused(true, origin + 60170ms);
        Require(clock.Now(origin + 90s) == origin + 170ms, "second pause advanced");
        clock.SetPaused(false, origin + 90170ms);
        Require(clock.Now(origin + 90186ms) == origin + 186ms, "pause accumulation incorrect");
        std::cout << "PASS pause/resume, 60-second hold, repeated pause, two pauses\n";

        // Mid-power-attack Quick Tune: its duration and crossfade remain at
        // the opening stage, while waveform sampling responds to live Speed.
        for (const int rate : {30, 60, 144, 240}) for (const bool mainMenu : {false, true}) {
            float age = 0.37f, blend = 0.42f;
            double phase = 1.25;
            const float frameTime = 1.0f / rate;
            for (int tick = 0; tick < rate * 60; ++tick) {
                const auto frame = EffectFrame::FromRealDelta(frameTime, true, !mainMenu, mainMenu);
                age += frame.envelopeDelta;
                blend += frame.envelopeDelta;
                phase += frame.noiseDelta * (tick < rate * 30 ? 1.0f : 2.0f);
            }
            Require(age == 0.37f && blend == 0.42f, "DDC menu consumed attack duration/blend");
            Require(std::abs(phase - 91.25) < 0.001, "DDC menu froze noise or ignored live speed");
            // Closing either editor while the other stays open must keep the
            // preview moving without consuming the held action's duration.
            for (const auto open : {std::array{true, true}, std::array{true, false},
                                    std::array{true, true}, std::array{false, true}}) {
                const auto handoff = EffectFrame::FromRealDelta(frameTime, true, open[0], open[1]);
                Require(handoff.envelopeDelta == 0 && handoff.noiseDelta == frameTime,
                        "Main menu/Quick Tune handoff stops preview or advances the action");
            }
            const auto resumed = EffectFrame::FromRealDelta(frameTime, false, false);
            age += resumed.envelopeDelta;
            Require(std::abs(age - (0.37f + frameTime)) < 0.00001f, "closing DDC menus caught up paused duration");
            const auto otherPause = EffectFrame::FromRealDelta(frameTime, true, false);
            Require(otherPause.envelopeDelta == 0 && otherPause.noiseDelta == 0, "unrelated paused menu animated noise");
            const auto unpaused = EffectFrame::FromRealDelta(frameTime, false, !mainMenu, mainMenu);
            Require(unpaused.envelopeDelta == frameTime && unpaused.noiseDelta == frameTime,
                    "Unpaused framework window unexpectedly holds action timing");
        }
        std::cout << "PASS both DDC menus: held action stage, moving noise, live speed, handoffs, no resume catch-up\n";

        for (const int rate : {30, 45, 60, 144, 240}) {
            float position = 0.0f;
            float velocity = 0.0f;
            for (int frame = 0; frame < rate; ++frame) {
                CriticalDampedSpringExact(position, velocity, 0.1f, 42.0f, 1.0f / rate);
                Require(std::isfinite(position) && std::isfinite(velocity), "spring non-finite");
                Require(position >= -0.00001f && position <= 0.10001f, "spring overshoot");
            }
            Require(std::abs(position - 0.1f) < 0.00001f, "spring failed to settle");
        }
        float position = 0.0f, velocity = 0.0f;
        for (const float step : {0.016f, 0.1f, 0.033f, 0.1f, 0.008f, 0.5f}) {
            CriticalDampedSpringExact(position, velocity, 0.1f, 42.0f, step);
            Require(std::isfinite(position) && position >= 0.0f && position <= 0.10001f,
                    "hitch destabilized spring");
        }
        std::cout << "PASS exact springs: 30/45/60/144/240 FPS and irregular hitches\n";

        const auto folder = std::filesystem::current_path() /
                            ("atomic-smoke-" + std::to_string(GetCurrentProcessId()));
        Require(std::filesystem::create_directory(folder), "test folder already exists");
        const auto path = folder / L"Preset-\u00e9.toml";
        std::string error;
        Require(WriteFileAtomically(path, "original", false, error), "initial save failed");
        Require(!WriteFileAtomically(path, "collision", false, error), "new save overwrote existing");
        Require(Read(path) == "original", "collision lost original");
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "could not lock target");
        const bool replacedLocked = WriteFileAtomically(path, "blocked", true, error);
        CloseHandle(locked);
        Require(!replacedLocked && Read(path) == "original", "locked replacement lost original");
        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "set read-only failed");
        const bool replacedReadonly = WriteFileAtomically(path, "blocked", true, error);
        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL), "clear read-only failed");
        Require(!replacedReadonly && Read(path) == "original", "read-only replacement lost original");
        const std::string large(3 * 1024 * 1024 + 73, 'Q');
        Require(WriteFileAtomically(path, large, true, error), "large replacement failed");
        Require(Read(path) == large, "large replacement was incomplete");
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            (void)entry;
            ++count;
        }
        Require(count == 1, "temporary files were left after failures");
        std::filesystem::remove(path);
        std::filesystem::remove(folder);
        std::cout << "PASS file saves: no-clobber, locked/read-only failures preserve original, "
                     "multi-chunk UTF-16-path replacement, temporary cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
