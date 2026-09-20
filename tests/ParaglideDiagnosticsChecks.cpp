#include "Core/ParaglideDiagnostics.h"

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace DietDrCamera::ParaglideDiagnostics;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static Sample At(double time, bool gliding = true)
{
    Sample sample;
    sample.time = time;
    sample.valid = true;
    sample.gliding = gliding;
    sample.player = { static_cast<float>(time * 12000.0), 0.0f, 0.0f };
    sample.beforeFollow = { -150.0f, 0.0f, 50.0f };
    sample.afterFollow = { -1200.0f, 0.0f, 50.0f };
    return sample;
}

static void CheckTimingAndMotion()
{
    for (const double fps : { 20.0, 60.0, 144.0, 240.0 }) {
        auto recorder = std::make_unique<Recorder>();
        double time = 0.0;
        for (int i = 0; i < 2 * fps; ++i) {
            time = i / fps;
            Require(!recorder->Observe(At(time, time >= 1.0)), "must not flush in flight");
        }
        const auto stable = recorder->Samples().back();
        Require(std::abs(stable.frameMs - 1000.0 / fps) < 0.01, "raw frame interval");
        Require(stable.playerStep > 0 && stable.correctionStep == 0 && stable.afterStep == 0,
            "fast world travel must not masquerade as a follow correction");
        Require(recorder->Samples().front().time >= 1.0 - Recorder::kPreRollSeconds,
            "pre-roll duration is bounded");
        Require(recorder->Samples().front().time < 1.0, "dash before deployment is retained");

        time += 0.25;
        recorder->Observe(At(time));
        const auto hitch = recorder->Samples().back();
        Require(std::abs(hitch.frameMs - 250.0f) < 0.01f && hitch.correctionStep == 0,
            "a game/update stall must remain visible without inventing a camera snap");
        time += 1.0 / fps;
        auto snap = At(time);
        snap.afterFollow.x += 400.0f;
        snap.reset = true;
        recorder->Observe(snap);
        const auto motion = recorder->Samples().back();
        Require(std::abs(motion.frameMs - 1000.0 / fps) < 0.01 && motion.correctionStep == 400.0f && motion.reset,
            "camera-only discontinuity remains distinguishable from a frame stall");
    }
}

static void CheckFlapsAndDeferredReport()
{
    auto recorder = std::make_unique<Recorder>();
    recorder->Observe(At(0.0));
    Require(!recorder->Observe(At(0.2, false)), "short latch drop must not flush");
    Require(!recorder->Observe(At(0.3)), "redeployment must stay in same capture");
    Require(recorder->CaptureNumber() == 1, "latch flutter must not exhaust capture budget");
    for (int i = 1; i <= 15; ++i)
        Require(!recorder->Observe(At(i)), "even a completed capture waits until flight ends");
    const auto count = recorder->Samples().size();
    Require(recorder->Samples().back().time == 12.0, "stop capture at window limit");
    Require(!recorder->Observe(At(16.0, false)), "landing has a reporting grace period");
    Require(recorder->Observe(At(17.1, false)), "flush after flight ends");
    Require(recorder->Samples().size() == count, "finished buffer stays frozen while waiting");
    recorder->Acknowledge();
    recorder->Observe(At(17.2));
    Require(recorder->Samples().back().frameMs == 0, "exclude report I/O from next frame measurement");
    Require(recorder->CaptureNumber() == 2, "subsequent test flight is captured");
}

static void CheckPauseMissingCameraAndCapacity()
{
    auto recorder = std::make_unique<Recorder>();
    recorder->Observe(At(1.0));
    auto paused = At(101.0);
    paused.paused = true;
    Require(recorder->Observe(paused), "pause is a safe deferred reporting point");
    Require(recorder->Samples().back().frameMs == 0, "menu time is not a gameplay hitch");
    recorder->Acknowledge();
    auto missing = At(102.0);
    missing.valid = false;
    recorder->Observe(missing);
    recorder->Observe(At(103.0));
    Require(recorder->Samples().back().frameMs == 0, "missing-camera gap has no continuous timing sample");
    missing.time = 104.0;
    Require(recorder->Observe(missing), "losing the player/camera completes the capture");
    recorder->Acknowledge();

    for (std::size_t i = 0; i < Recorder::kCapacity + 200; ++i)
        Require(!recorder->Observe(At(105.0 + i * 0.0001)), "capacity never forces disk I/O during glide");
    Require(recorder->Samples().size() == Recorder::kCapacity, "fixed buffer cannot overflow at high FPS");
    Require(recorder->Observe(At(110.0, false)), "full buffer is reportable after landing");
    recorder->Acknowledge();
    recorder->Observe(At(111.0));
    Require(recorder->Observe(At(114.0, false)), "fourth capture reports");
    recorder->Acknowledge();
    Require(!recorder->Enabled(), "capture count is bounded per launch");
    recorder->Observe(At(115.0));
    Require(recorder->Samples().empty(), "exhausted budget does not restart");
}

int main()
{
    try {
        CheckTimingAndMotion();
        CheckFlapsAndDeferredReport();
        CheckPauseMissingCameraAndCapacity();
        std::cout << "Paraglide diagnostics checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
