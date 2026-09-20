#include "Core/FirstPersonNoise.h"
#include "Core/NoiseTransition.h"
#include "Core/EffectFrame.h"
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;
using Sources = FirstPersonNoiseSources;
static const dietdrcamera::Perlin1D x(2001), y(2002), z(2003);

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

// The two small-angle bands before conversion to NiCamera matrices. These
// are the production samples, including the real noise seeds and phase.
static std::array<float,3> Sample(const FirstPersonNoiseLayer& layer, float weight = 1)
{
    std::array<float,3> result{};
    for (const auto& band : SampleFirstPersonNoise(layer,weight,x,y,z))
        for (int i = 0; i < 3; ++i) result[i] += band.axis[i] * band.theta;
    return result;
}
static float Difference(std::array<float,3> a, std::array<float,3> b)
{
    float sum = 0;
    for (int i = 0; i < 3; ++i) sum += (a[i]-b[i])*(a[i]-b[i]);
    return std::sqrt(sum);
}

static void CheckBlockingDuringSources()
{
    float legacyPhaseChange = 0;
    for (int fps : {30,60,144,240}) {
        Sources reference, actual;
        Sources::Layers target{};
        for (auto& layer : target) layer = {0,12,1.2f,4,.7f,.6f};
        FirstPersonNoiseLayer player{47,5,4,3,0,0}; // power attack from the user's preset
        NoiseTransitionHistory<FirstPersonNoiseLayer> outgoing;
        float phase = 1;
        for (int frame = 0; frame < fps*4; ++frame) {
            // Rapid block/attack/profile changes interrupt the existing fade,
            // including while NPC, dragon, draw and event sources overlap.
            if (frame % (fps/4) == 0) {
                outgoing.Capture(player,phase,2);
                phase = 0;
                player.clock += 611.7;
                player.speed = frame % fps == 0 ? .1f : 4;
                player.rot = frame % fps == 0 ? 5 : 3;
            }
            const float dt = 1.0f/fps;
            actual.Update(target,dt,dt);
            reference.Update(target,dt,dt);
            player.clock += dt * (std::max)(.25f,player.speed);
            outgoing.Advance(dt);
            phase = (std::min)(1.0f,phase+2*dt);
            if (phase == 1) outgoing.Clear();
            for (int i = 0; i < Sources::Count; ++i) {
                Require(Difference(Sample(actual.Get()[i]),Sample(reference.Get()[i])) == 0,
                    "Blocking or an interrupted profile fade changed an ongoing external source");
                auto legacy = actual.Get()[i];
                legacy.clock = player.clock;
                legacyPhaseChange = (std::max)(legacyPhaseChange,Difference(Sample(legacy),Sample(reference.Get()[i])));
            }
        }
        // Turning off a source must not replay it from the outgoing player
        // texture. No extra crossfade-duration tail belongs to that source.
        target = {};
        actual.Update(target,0,0);
        Require(!actual.Audible(), "A cinematic source was retained by a player-profile fade");
        for (const auto& layer : actual.Get()) Require(Sample(layer) == std::array<float,3>{}, "Source retained a stale sample");
    }
    Require(legacyPhaseChange > .001f, "Fixture did not reproduce the former shared-clock disturbance");
    std::cout << "External-source phase disturbance: shared=" << legacyPhaseChange << ", independent=0\n";
}

static void CheckSourceIndependenceAndCharacter()
{
    Sources reference, mixed;
    Sources::Layers target{};
    target[Sources::NPC] = {0,12,1.2f,4,.7f,.6f};
    for (int frame = 0; frame < 360; ++frame) {
        reference.Update(target,1.0f/60,1.0f/60);
        auto others = target;
        if (frame % 60 < 30) others[Sources::Dragon] = {0,35,4,1,0,.2f};
        if (frame % 45 < 20) others[Sources::Draw] = {0,6,.1f,5,.9f,.8f};
        mixed.Update(others,1.0f/60,1.0f/60);
        Require(Sample(mixed.Get()[Sources::NPC]) == Sample(reference.Get()[Sources::NPC]),
            "Another effect changed the ongoing NPC waveform");
    }
    const auto before = Sample(mixed.Get()[Sources::NPC]);
    target[Sources::NPC] = {0,12,4,2,.1f,.2f};
    mixed.Update(target,0,0);
    Require(Sample(mixed.Get()[Sources::NPC]) == before, "A live source character change snapped the sample");
    mixed.Update(target,1.0f/240,1.0f/240);
    Require(Difference(Sample(mixed.Get()[Sources::NPC]),before) < .001f, "Source shape/tempo did not ease into motion");
}

static void CheckFrameRateAndPausedPreview()
{
    double expectedClock = 0;
    std::array<float,3> expectedSample{};
    for (int fps : {240,30,60,144}) {
        Sources source;
        Sources::Layers target{};
        target[Sources::NPC] = {0,12,.25f,4,.7f,.6f};
        source.Update(target,0,0);
        target[Sources::NPC].speed = 4;
        target[Sources::NPC].rot = 2;
        for (int frame = 0; frame < fps*2; ++frame) source.Update(target,1.0f/fps,1.0f/fps);
        const auto& layer = source.Get()[Sources::NPC];
        if (fps == 240) { expectedClock = layer.clock; expectedSample = Sample(layer); }
        Require(std::abs(layer.clock-expectedClock) < .00002 && Difference(Sample(layer),expectedSample) < .000002f,
            "Independent source phase or character depends on frame rate");
        const float heldAmp = layer.amp, heldSpeed = layer.speed;
        const double startClock = layer.clock;
        for (int frame = 0; frame < fps; ++frame) {
            const auto tick = EffectFrame::FromRealDelta(1.0f/fps,true,true);
            source.Update(target,tick.envelopeDelta,tick.noiseDelta);
        }
        Require(layer.amp == heldAmp && layer.speed == heldSpeed && layer.clock > startClock,
            "Paused Quick Tune failed to hold the envelope while continuing its texture");
        const auto held = Sample(layer);
        source.Update(target,0,0);
        Require(Sample(layer) == held, "A fully paused effect changed its sample");
    }
}

int main() try
{
    CheckBlockingDuringSources();
    CheckSourceIndependenceAndCharacter();
    CheckFrameRateAndPausedPreview();
    std::cout << "First-person source continuity checks passed.\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
