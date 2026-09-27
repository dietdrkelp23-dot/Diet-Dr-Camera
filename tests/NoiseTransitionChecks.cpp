#include "Core/NoiseTransition.h"
#include "Core/EffectFrame.h"
#include "vendor/PerlinNoise.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Layer
{
    double clock;
    float amplitude, speed, roughness, jitter;
};

static const dietdrcamera::Perlin1D noise(12345);

// Exercise the real Perlin implementation at both views' time scales. The
// transition must preserve a signal, not just the nominal amplitude of a profile.
static double Sample(const Layer& layer, bool firstPerson)
{
    if (firstPerson) {
        return layer.amplitude * (noise.SampleFractal(layer.clock, 3, layer.roughness) *
            (1.0 - 0.5 * layer.jitter) + noise.SampleFractal(layer.clock * 2.2 + 4.2, 3, layer.roughness) *
            (0.5 + layer.jitter) / 6.0);
    }
    return layer.amplitude * (noise.SampleFractal(layer.clock * 0.22, 2, layer.roughness) *
        (1.0 - layer.jitter) + noise.SampleFractal(layer.clock * 5.0 + 100.0, 2, layer.roughness) * layer.jitter);
}

struct Blend
{
    Layer current{3.71, 1.0f, 0.4f, 0.35f, 0.2f};
    NoiseTransitionHistory<Layer> outgoing;
    float phase = 1.0f, rate = 1.0f / 0.8f;

    void Change(Layer layer, float duration = 0.8f)
    {
        outgoing.Capture(current, phase, rate);
        current = layer;
        phase = 0.0f;
        rate = 1.0f / duration;
    }

    void Tick(float envelopeDelta, float noiseDelta)
    {
        outgoing.Advance(envelopeDelta);
        phase = std::min(1.0f, phase + envelopeDelta * rate);
        if (phase == 1.0f) outgoing.Clear();
        current.clock += noiseDelta * current.speed;
        outgoing.UpdateLayers([&](Layer& layer) { layer.clock += noiseDelta * layer.speed; });
    }

    double Value(bool firstPerson) const
    {
        float incoming, old;
        NoiseFadeWeights(phase, incoming, old);
        double value = incoming * Sample(current, firstPerson);
        outgoing.Visit([&](const Layer& layer, float weight) { value += old * weight * Sample(layer, firstPerson); });
        return value;
    }

    double Power() const
    {
        float incoming, old;
        NoiseFadeWeights(phase, incoming, old);
        double result = incoming * incoming;
        outgoing.Visit([&](const Layer&, float weight) { result += old * old * weight * weight; });
        return result;
    }
};

static void CheckInterruptedSignal(bool firstPerson)
{
    double worstOldJump = 0, worstNewJump = 0;
    for (float interruption : {0.05f, 0.25f, 0.5f, 0.75f, 0.95f}) {
        Blend original;
        const Layer a = original.current;
        const Layer b{615.41, 2.7f, 3.2f, 0.65f, 0.8f};
        original.Change(b);
        original.Tick(interruption * 0.8f, interruption * 0.8f);
        const double before = original.Value(firstPerson);

        // The former collapse roughly preserves energy but changes phase and
        // direction. Keep a measured regression example, not just identity math.
        float wc, wp;
        NoiseFadeWeights(original.phase, wc, wp);
        const float total = wc + wp;
        const Layer collapsed{original.current.clock,
            std::sqrt(wc * wc * b.amplitude * b.amplitude + wp * wp * a.amplitude * a.amplitude),
            (wc * b.speed + wp * a.speed) / total,
            (wc * b.roughness + wp * a.roughness) / total,
            (wc * b.jitter + wp * a.jitter) / total};
        worstOldJump = std::max(worstOldJump, std::abs(Sample(collapsed, firstPerson) - before));

        Blend interrupted = original;
        interrupted.Change({1227.11, 1.3f, 0.8f, 0.45f, 0.1f});
        const double newJump = std::abs(interrupted.Value(firstPerson) - before);
        worstNewJump = std::max(worstNewJump, newJump);
        Require(newJump < 1e-7, "Interruption changed the sampled signal at the handoff");

        // Compare the new trajectory with uninterrupted continuation. Its
        // difference must begin quadratically, not with a displacement/velocity cut.
        for (float h : {0.001f, 0.002f, 0.004f}) {
            auto continued = original;
            auto changed = interrupted;
            continued.Tick(h, h);
            changed.Tick(h, h);
            const double derivativeError = std::abs(changed.Value(firstPerson) - continued.Value(firstPerson)) / h;
            Require(derivativeError < 30.0 * h, "Interruption introduced a first-order velocity change");
        }
    }
    Require(worstOldJump > 0.02, "Fixture did not reproduce the old parameter-collapse jump");
    std::cout << (firstPerson ? "1p" : "3p") << " handoff max sample discontinuity: old="
              << worstOldJump << ", retained=" << worstNewJump << '\n';
}

static void CheckOrdinaryFadeAndCompositionOrder()
{
    Blend blend;
    const auto old = blend.current;
    blend.Change({615.41, 2.7f, 3.2f, 0.65f, 0.8f});
    for (int step = 0; step <= 80; ++step) {
        const float t = step / 100.0f;
        auto frame = blend;
        frame.Tick(t, t);
        float incoming, outgoing;
        NoiseFadeWeights(frame.phase, incoming, outgoing);
        auto oldAdvanced = old;
        oldAdvanced.clock += t * old.speed;
        for (bool firstPerson : {false, true}) {
            const double expected = incoming * Sample(frame.current, firstPerson) + outgoing * Sample(oldAdvanced, firstPerson);
            Require(std::abs(frame.Value(firstPerson) - expected) < 1e-7, "Ordinary two-layer fade changed");
        }
    }
    NoiseTransitionHistory<int> history;
    history.Capture(1, 1.0f, 1.0f);
    history.Capture(2, 0.3f, 1.0f);
    history.Capture(3, 0.6f, 1.0f);
    std::array<int, 3> order{};
    std::size_t index = 0;
    history.Visit([&](int layer, float) { order.at(index++) = layer; });
    Require(order == std::array{3, 2, 1}, "Retained rotations changed composition order");
}

static void CheckChainsAndEnergy()
{
    for (int fps : {30, 60, 144, 240}) {
        Blend blend;
        const float dt = 1.0f / fps;
        std::size_t maximum = 0;
        for (int frame = 0; frame < fps * 10; ++frame) {
            // Deliberately harsher than gameplay: a new texture every frame,
            // all with the maximum ordinary adaptive fade duration.
            const double before = blend.Value(true);
            blend.Change({frame * 611.7, 1.0f, 0.25f + frame % 5, 0.45f, 0.35f}, 1.2f);
            // Reassociation of nested float weights can differ by a few ULPs.
            Require(std::abs(blend.Value(true) - before) < 1e-6, "A chained interruption cut the old composite");
            blend.Tick(dt, dt);
            Require(std::abs(blend.Power() - 1.0) < 2e-5, "Chained fades inflated or lost noise power");
            maximum = std::max(maximum, blend.outgoing.Size());
            Require(blend.outgoing.Size() <= static_cast<std::size_t>(fps * 1.2f) + 2,
                "Completed fades accumulated history indefinitely");
        }
        blend.Tick(1.21f, 0.0f);
        Require(blend.outgoing.Size() == 0, "A finished transition retained obsolete layers");
        std::cout << fps << " Hz every-frame interruption: max history=" << maximum << '\n';
    }
}

static void CheckQuickTuneAndSilence(bool mainMenu)
{
    Blend blend;
    blend.Change({615.41, 2.7f, 3.2f, 0.65f, 0.8f});
    blend.Tick(0.2f, 0.2f);
    blend.Change({1227.11, 1.3f, 0.8f, 0.45f, 0.1f});
    blend.Tick(0.1f, 0.1f);
    const float heldPhase = blend.phase;
    const double heldPower = blend.Power(), startValue = blend.Value(false), startValueFp = blend.Value(true);
    double motion = 0, motionFp = 0;
    for (int frame = 0; frame < 3600; ++frame) {
        const auto tick = EffectFrame::FromRealDelta(1.0f / 60.0f, true, !mainMenu, mainMenu);
        blend.Tick(tick.envelopeDelta, tick.noiseDelta);
        Require(blend.phase == heldPhase && blend.Power() == heldPower, "DDC menu advanced a held fade");
        motion = std::max(motion, std::abs(blend.Value(false) - startValue));
        motionFp = std::max(motionFp, std::abs(blend.Value(true) - startValueFp));
    }
    Require(motion > 0.01 && motionFp > 0.01, "DDC menu froze the retained noise waveform in a POV");
    const auto resumed = EffectFrame::FromRealDelta(1.0f / 60.0f, false, !mainMenu, mainMenu);
    blend.Tick(resumed.envelopeDelta, resumed.noiseDelta);
    Require(std::abs(blend.phase - heldPhase - resumed.envelopeDelta * blend.rate) < 1e-6,
        "Resuming DDC menu counted the paused time");
    blend.Change({0, 0, 1, 0.45f, 0.35f});
    const auto size = blend.outgoing.Size();
    for (int i = 0; i < 10000; ++i) blend.Change({i * 611.7, 0, 1, 0.45f, 0.35f});
    Require(blend.outgoing.Size() == size, "Paused selection changes accumulated inaudible layers");
    Require(std::abs(blend.Value(true)) > 1e-5, "Silent incoming profile cut the outgoing tail");
    blend.Tick(0.81f, 0.81f);
    Require(blend.Value(true) == 0 && blend.Value(false) == 0 && blend.outgoing.Size() == 0,
        "Fade to silence failed to settle and retire its layers");
}

static void CheckChainedShoutRestart()
{
    double worstOldJump = 0.0, worstNewJump = 0.0;
    // The same enabled Whirlwind Sprint profile remains selected throughout a
    // chain. Only the envelope falls from the previous word tier to the next
    // windup: the old profile-signature guard therefore never armed a fade.
    for (float previousEnvelope : {0.70f, 1.20f, 2.40f, 3.70f}) {
        for (float phase : {0.0f, 0.2f, 0.65f, 1.0f}) {
            Blend original;
            original.Change({615.41, 5.0f * previousEnvelope, 2.0f * previousEnvelope, 1.0f, 0.0f}, 1.0f);
            original.Tick(phase, phase);
            const double before = original.Value(false);
            auto incoming = original.current;
            incoming.amplitude = 5.0f;  // unchanged profile, fresh 1.0x windup
            incoming.speed = 2.0f;

            auto legacy = original;
            legacy.current = incoming;
            worstOldJump = std::max(worstOldJump, std::abs(legacy.Value(false) - before));

            ShoutNoiseRestart restart;
            Require(!restart.Observe(true, 100), "First shout was treated as a restart");
            Require(!restart.Observe(true, 100), "Held shout repeatedly armed a fade");
            auto restarted = original;
            if (restart.Observe(true, 200)) {
                incoming.clock += 611.7;
                restarted.Change(incoming, 1.0f);
            } else {
                restarted.current = incoming;
            }
            const double jump = std::abs(restarted.Value(false) - before);
            worstNewJump = std::max(worstNewJump, jump);
            Require(jump < 1e-6, "Chained shout cut its live noise signal when windup restarted");
            Require(!restart.Observe(true, 200), "Charge/release polling retriggered the same cast");
            for (float h : {0.001f, 0.002f, 0.004f}) {
                auto continued = original;
                auto changed = restarted;
                continued.Tick(h, h);
                changed.Tick(h, h);
                const double derivativeError = std::abs(changed.Value(false) - continued.Value(false)) / h;
                Require(derivativeError < 150.0 * h, "Chained shout introduced a first-order noise velocity cut");
            }
        }
    }
    Require(worstOldJump > 0.1, "Fixture did not reproduce the unblended shout restart");
    std::cout << "3p chained-shout sample discontinuity: old=" << worstOldJump
              << ", retained=" << worstNewJump << '\n';

    ShoutNoiseRestart edges;
    Require(!edges.Observe(false, 0), "Idle armed a shout fade");
    Require(!edges.Observe(true, 0), "Missing cast identity armed a fade");
    Require(!edges.Observe(true, 100), "First valid cast after missing identity was a restart");
    for (int i = 0; i < 120; ++i)
        Require(!edges.Observe(true, 100), "Paused/repeated observation restarted the shout");
    Require(!edges.Observe(false, 100), "Shout exit was classified as a restart");
    Require(!edges.Observe(true, 200), "A separated shout was treated as an overlapping cast");
    Require(edges.Observe(true, 300), "A fresh overlapping cast was not detected");

    for (int fps : {30, 60, 120, 144, 240}) {
        Blend chain;
        ShoutNoiseRestart restart;
        std::uint64_t cast = 100;
        restart.Observe(true, cast);
        const float dt = 1.0f / fps;
        const int interval = std::max(1, fps / 5); // deliberately faster than a full charge
        double maximumJump = 0.0;
        for (int frame = 0; frame < fps * 10; ++frame) {
            const bool fresh = frame % interval == 0;
            if (fresh) ++cast;
            const double before = chain.Value(false);
            if (restart.Observe(true, cast)) {
                chain.Change({chain.current.clock + 611.7, 5.0f, 2.0f, 1.0f, 0.0f}, 1.0f);
                maximumJump = std::max(maximumJump, std::abs(chain.Value(false) - before));
                Require(std::abs(chain.Value(false) - before) < 1e-6, "Rapid chain discarded a still-visible shout tail");
            }
            chain.Tick(dt, dt);
            Require(chain.outgoing.Size() <= 7, "Chained shout history failed to retire");
        }
        restart.Observe(false, cast);
        chain.Change({0, 0, 1, 1, 0}, 1.0f);
        chain.Tick(1.01f, 1.01f);
        Require(chain.Value(false) == 0.0 && chain.outgoing.Size() == 0,
            "Completed shout chain kept stale noise running");
        std::cout << fps << " Hz shout chain: max handoff jump=" << maximumJump << '\n';
    }
}

int main()
{
    try {
        CheckInterruptedSignal(false);
        CheckInterruptedSignal(true);
        CheckOrdinaryFadeAndCompositionOrder();
        CheckChainedShoutRestart();
        CheckChainsAndEnergy();
        CheckQuickTuneAndSilence(false);
        CheckQuickTuneAndSilence(true);
        std::cout << "Noise transition checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
