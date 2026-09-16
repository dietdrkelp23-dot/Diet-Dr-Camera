#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace DietDrCamera
{
    inline void NoiseFadeWeights(float phase, float& incoming, float& outgoing)
    {
        const float x = std::clamp(phase, 0.0f, 1.0f);
        const float s = x * x * (3.0f - 2.0f * x);
        incoming = x == 1.0f ? 1.0f : std::sin(s * 1.5707963f);
        outgoing = x == 1.0f ? 0.0f : std::cos(s * 1.5707963f);
    }

    // An interrupted blend is still a blend of real signals, not a new signal
    // made from averaged parameters. Retain its layers, clocks and fade progress.
    // Older fades continue underneath the new one, preserving both sample value
    // and weight velocity at the handoff. Completed fades discard their ancestry.
    template <class Layer>
    class NoiseTransitionHistory
    {
    public:
        void Capture(const Layer& current, float phase, float rate)
        {
            // The incoming signal is inaudible at phase zero. In particular,
            // repeated selection changes during paused Quick Tune must not
            // accumulate a stack of zero-weight profiles.
            if (phase <= 0.0f) return;
            if (phase >= 1.0f) _nodes.clear();
            _nodes.push_back({current, std::clamp(phase, 0.0f, 1.0f), rate});
        }

        void Advance(float envelopeDelta)
        {
            std::size_t lastComplete = 0;
            for (std::size_t i = 0; i < _nodes.size(); ++i) {
                auto& node = _nodes[i];
                node.phase = (std::min)(1.0f, node.phase +
                    (std::max)(0.0f, envelopeDelta) * (std::max)(0.0f, node.rate));
                if (node.phase == 1.0f) lastComplete = i;
            }
            if (lastComplete != 0) _nodes.erase(_nodes.begin(), _nodes.begin() + lastComplete);
        }

        template <class Function>
        void UpdateLayers(Function&& update)
        {
            for (auto& node : _nodes) update(node.layer);
        }

        // Newest first, matching the original incoming-then-outgoing rotation
        // composition order. The caller applies its outer fade/master weight.
        template <class Function>
        void Visit(Function&& sample) const
        {
            float remaining = 1.0f;
            for (auto it = _nodes.rbegin(); it != _nodes.rend() && remaining > 0.0f; ++it) {
                float incoming, outgoing;
                NoiseFadeWeights(it->phase, incoming, outgoing);
                if (incoming > 0.0f) sample(it->layer, remaining * incoming);
                remaining *= outgoing;
            }
        }

        void Clear() { _nodes.clear(); }
        [[nodiscard]] std::size_t Size() const { return _nodes.size(); }

    private:
        struct Node { Layer layer; float phase, rate; };
        std::vector<Node> _nodes;
    };
}
