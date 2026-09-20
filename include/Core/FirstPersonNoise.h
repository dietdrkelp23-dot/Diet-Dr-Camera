#pragma once

#include "vendor/PerlinNoise.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace DietDrCamera
{
    struct FirstPersonNoiseLayer
    {
        double clock = 0;
        float amp = 0, speed = 0, rot = 0;
        float driftJitter = .35f, roughness = .45f;
    };

    struct FirstPersonNoiseBand
    {
        std::array<float, 3> axis{};
        float theta = 0;
    };

    // Shared by the state texture and independent cinematic sources. Keep the
    // established two-band first-person waveform and gain in one sampler.
    inline std::array<FirstPersonNoiseBand, 2> SampleFirstPersonNoise(
        const FirstPersonNoiseLayer& layer, float weight,
        const dietdrcamera::Perlin1D& x, const dietdrcamera::Perlin1D& y, const dietdrcamera::Perlin1D& z)
    {
        if (weight <= 0 || layer.amp <= .0001f) return {};
        const float gain = layer.rot * 2 * layer.amp * weight;
        const double rough = std::clamp(layer.roughness, .1f, .8f);
        const float mix = std::clamp(layer.driftJitter, 0.0f, 1.0f);
        const auto sample = [rough](const auto& noise, double time) {
            return static_cast<float>(noise.SampleFractal(time, 3, rough)) * 6.2831853f;
        };
        return {{
            {{sample(x, layer.clock), sample(y, layer.clock + 7.1), sample(z, layer.clock + 15.8)},
                .00006f * gain * (1 - .5f * mix)},
            {{sample(z, layer.clock * 2.2), sample(x, layer.clock * 2.2 + 4.2), sample(y, layer.clock * 2.2 + 9.6)},
                .00001f * gain * (.5f + mix)}
        }};
    }

    // These sources already own their attack/recovery envelopes. Neither a
    // player profile change nor its texture crossfade may capture, decorrelate
    // or replay them. Each source has a persistent phase and shape follower.
    class FirstPersonNoiseSources
    {
    public:
        enum Source { Dragon, Jump, Draw, Attack, Event, NPC, Count };
        using Layers = std::array<FirstPersonNoiseLayer, Count>;

        FirstPersonNoiseSources()
        {
            for (std::size_t i = 0; i < layers.size(); ++i) layers[i].clock = 137.9 + 611.7 * i;
        }

        void Update(const Layers& targets, float envelopeDelta, float noiseDelta, bool liveEdit = false)
        {
            const float dt = std::clamp(envelopeDelta, 0.0f, .05f);
            const float shapeAlpha = -std::expm1(-8 * dt);
            for (std::size_t i = 0; i < layers.size(); ++i) {
                auto& layer = layers[i];
                const auto& target = targets[i];
                const float speed = (std::max)(.25f, target.speed);
                // At silence there is no audible shape to preserve. Seed the
                // source's own character, never the active player profile's.
                const bool seed = layer.amp <= .0001f || liveEdit;
                float averageSpeed = layer.speed;
                if (target.amp > .0001f) {
                    const float rate = speed > layer.speed ? 9.0f : 3.5f;
                    const float alpha = -std::expm1(-rate * dt);
                    averageSpeed = seed ? speed : dt > 0
                        ? speed + (layer.speed - speed) * alpha / (rate * dt) : layer.speed;
                    layer.speed = seed ? speed : layer.speed + (speed - layer.speed) * alpha;
                    layer.rot = seed ? target.rot : layer.rot + (target.rot - layer.rot) * shapeAlpha;
                    layer.driftJitter = seed ? target.driftJitter : layer.driftJitter + (target.driftJitter - layer.driftJitter) * shapeAlpha;
                    layer.roughness = seed ? target.roughness : layer.roughness + (target.roughness - layer.roughness) * shapeAlpha;
                }
                layer.amp = (std::max)(0.0f, target.amp);
                layer.clock += (std::max)(0.0f, noiseDelta) * (std::max)(.25f, averageSpeed);
                layer.clock = std::fmod(layer.clock, 65536.0);
            }
        }

        [[nodiscard]] const Layers& Get() const { return layers; }
        [[nodiscard]] bool Audible() const
        {
            for (const auto& layer : layers) if (layer.amp > .001f && layer.rot > .0001f) return true;
            return false;
        }
    private:
        Layers layers{};
    };
}
