#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace DietDrCamera::ParaglideDiagnostics
{
    struct Point
    {
        float x{}, y{}, z{};
    };

    inline float Distance(Point a, Point b)
    {
        return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    inline Point Difference(Point a, Point b)
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    struct Sample
    {
        double time{};
        float frameMs{}, chainMs{}, noiseMs{};
        bool valid{}, paused{}, gliding{}, whirlwind{}, suspended{};
        int cameraState = -1;
        std::uint32_t cell{};
        Point player{}, beforeFollow{}, afterFollow{};
        Point noiseTranslation{};
        // Current and requested framing: side, height, zoom, pitch, FOV.
        std::array<float, 5> current{}, target{};
        bool followed{}, reset{}, windSampled{}, windGuard{};
        float looseness{}, lag{}, rate{}, flee{}, ground{}, windAltitude{}, windSpeed{};
        float playerStep{}, beforeStep{}, afterStep{}, correctionStep{};
    };

    // Main camera thread only. No allocation, locks, formatting or I/O while
    // collecting. A fixed buffer retains every update around deployment, plus
    // pre-roll, and stops after 12 seconds (or capacity at unusually high FPS).
    class Recorder
    {
    public:
        static constexpr std::size_t kCapacity = 4096;
        static constexpr unsigned kCaptureLimit = 4;
        static constexpr double kPreRollSeconds = 0.75;
        static constexpr double kWindowSeconds = 12.0;
        static constexpr double kExitSeconds = 2.0;

        bool Observe(Sample sample)
        {
            sample.frameMs = sample.playerStep = sample.beforeStep =
                sample.afterStep = sample.correctionStep = 0.0f;
            if (previous_.valid && sample.valid && !previous_.paused && !sample.paused &&
                sample.time > previous_.time) {
                // Deliberately unclamped: a long update gap is the evidence.
                sample.frameMs = static_cast<float>((sample.time - previous_.time) * 1000.0);
                sample.playerStep = Distance(sample.player, previous_.player);
                sample.beforeStep = Distance(sample.beforeFollow, previous_.beforeFollow);
                sample.afterStep = Distance(sample.afterFollow, previous_.afterFollow);
                sample.correctionStep = Distance(
                    Difference(sample.afterFollow, sample.beforeFollow),
                    Difference(previous_.afterFollow, previous_.beforeFollow));
            }

            const bool glide = sample.valid && !sample.paused && sample.gliding;
            if (!active_ && captures_ < kCaptureLimit && glide && !wasGliding_) {
                active_ = true;
                ++captures_;
                started_ = sample.time;
                count_ = 0;
                for (std::size_t i = 0; i < preCount_; ++i) {
                    const auto& pre = preRoll_[(preNext_ + preRoll_.size() - preCount_ + i) % preRoll_.size()];
                    if (sample.time - pre.time <= kPreRollSeconds) samples_[count_++] = pre;
                }
            }

            if (active_) {
                if (glide) lastGlide_ = sample.time;
                if (!finished_) {
                    samples_[count_++] = sample;
                    finished_ = count_ == samples_.size() ||
                        sample.time - started_ >= kWindowSeconds || sample.paused || !sample.valid ||
                        (!sample.gliding && sample.time - lastGlide_ >= kExitSeconds);
                }
            }

            previous_ = sample;
            wasGliding_ = glide;
            if (sample.valid && !sample.paused) {
                preRoll_[preNext_] = sample;
                preNext_ = (preNext_ + 1) % preRoll_.size();
                preCount_ = (std::min)(preCount_ + 1, preRoll_.size());
            } else {
                preCount_ = preNext_ = 0;
            }

            // Never write a completed buffer while still gliding. A pause or
            // two seconds out of glide is the safe reporting point. Brief latch
            // flaps stay in the same capture instead of consuming its budget.
            return active_ && finished_ &&
                (sample.paused || !sample.valid || (!sample.gliding && sample.time - lastGlide_ >= kExitSeconds));
        }

        void Acknowledge()
        {
            active_ = finished_ = false;
            count_ = 0;
            preCount_ = preNext_ = 0;
            // Do not count the time spent formatting/flushing as a flight hitch.
            previous_.valid = false;
        }

        [[nodiscard]] bool Enabled() const { return active_ || captures_ < kCaptureLimit; }
        [[nodiscard]] unsigned CaptureNumber() const { return captures_; }
        [[nodiscard]] double Started() const { return started_; }
        [[nodiscard]] std::span<const Sample> Samples() const { return { samples_.data(), count_ }; }

    private:
        std::array<Sample, kCapacity> samples_{};
        std::array<Sample, 256> preRoll_{};
        std::size_t count_{}, preCount_{}, preNext_{};
        Sample previous_{};
        double started_{}, lastGlide_{};
        unsigned captures_{};
        bool active_{}, finished_{}, wasGliding_{};
    };
}
