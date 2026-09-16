#pragma once

#include <string_view>

namespace RE { class NiCamera; }

namespace DietDrCamera::TweenCameraTrace
{
    void OnMenuEvent(std::string_view menuName, bool opening);
    void BeginCameraPass();
    void Sample(const char* stage, bool force = false);
    void SampleGuard(bool enabled, int cooldown);

    class StageSample
    {
    public:
        explicit StageSample(const char* stage) : stage_(stage) {}
        ~StageSample() { Sample(stage_); }
        StageSample(const StageSample&) = delete;
        StageSample& operator=(const StageSample&) = delete;

    private:
        const char* stage_;
    };

    class RenderSample
    {
    public:
        explicit RenderSample(RE::NiCamera* camera) : camera_(camera) {}
        ~RenderSample();
        RenderSample(const RenderSample&) = delete;
        RenderSample& operator=(const RenderSample&) = delete;

    private:
        RE::NiCamera* camera_;
    };
}
