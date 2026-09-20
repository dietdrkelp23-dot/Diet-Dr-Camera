#include "PCH.h"
#include "Hooks/ParaglideTrace.h"
#include "Core/ParaglideDiagnostics.h"
#include "Camera/CameraController.h"
#include "Camera/StateResolver.h"
#include "Settings/SettingsManager.h"

namespace DietDrCamera::ParaglideTrace
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace ParaglideDiagnostics;
        Recorder recorder;
        Sample sample;
        bool collecting = false;
        RE::NiPoint3 preNoiseTranslation{};

        Point PointOf(const RE::NiPoint3& point) { return { point.x, point.y, point.z }; }
        double Seconds(Clock::time_point time) { return std::chrono::duration<double>(time.time_since_epoch()).count(); }
        float Milliseconds(Clock::time_point a, Clock::time_point b)
        {
            return std::chrono::duration<float, std::milli>(a - b).count();
        }

        void Flush()
        {
            const auto rows = recorder.Samples();
            std::array<bool, Recorder::kCapacity> selected{};
            float maxFrame{}, maxChain{}, maxNoise{}, maxCorrection{};
            unsigned hitches{}, resets{}, guards{}, glideEdges{};
            unsigned eventRows{};
            double lastSelected = -1.0e30;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& row = rows[i];
                maxFrame = (std::max)(maxFrame, row.frameMs);
                maxChain = (std::max)(maxChain, row.chainMs);
                maxNoise = (std::max)(maxNoise, row.noiseMs);
                maxCorrection = (std::max)(maxCorrection, row.correctionStep);
                hitches += row.frameMs > 50.0f;
                resets += row.reset;
                guards += row.windGuard;
                const bool edge = i && row.gliding != rows[i - 1].gliding;
                glideEdges += edge;
                const bool event = (edge || row.reset) && eventRows < 24;
                if (event) ++eventRows;
                if (row.time - lastSelected >= 0.1 || i + 1 == rows.size() || event) {
                    selected[i] = true;
                    lastSelected = row.time;
                }
            }
            // Retain neighbours of the worst timing gaps and camera-correction
            // changes even when those fall between the 100 ms context rows.
            for (int metric = 0; metric < 3; ++metric) {
                std::array<std::size_t, 3> worst{};
                std::array<float, 3> peaks{};
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const auto& row = rows[i];
                    const float value = metric == 0 ? row.frameMs :
                        metric == 1 ? row.correctionStep : row.chainMs + row.noiseMs;
                    for (std::size_t rank = 0; rank < peaks.size(); ++rank) {
                        if (value <= peaks[rank]) continue;
                        for (std::size_t n = peaks.size() - 1; n > rank; --n) {
                            peaks[n] = peaks[n - 1]; worst[n] = worst[n - 1];
                        }
                        peaks[rank] = value; worst[rank] = i;
                        break;
                    }
                }
                for (auto index : worst) {
                    if (index) selected[index - 1] = true;
                    selected[index] = true;
                    if (index + 1 < rows.size()) selected[index + 1] = true;
                }
            }

            fmt::memory_buffer output;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (!selected[i]) continue;
                const auto& r = rows[i];
                fmt::format_to(std::back_inserter(output),
                    "t={:+.4f} frameMs={:.2f} chainMs={:.2f} noiseMs={:.2f} "
                    "valid={} pause={} glide={} ww={} suspend={} state={} cell={:08X} "
                    "playerStep={:.1f} preStep={:.1f} postStep={:.1f} correctionStep={:.1f} "
                    "follow={} reset={} loose={:.3f} lag={:.1f} rate={:.4f} flee={:.3f} "
                    "pre=({:.1f},{:.1f},{:.1f}) post=({:.1f},{:.1f},{:.1f}) "
                    "current=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) target=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) "
                    "wind={} ground={:.0f} alt={:.3f} speed={:.3f} guard={} noisePos=({:.2f},{:.2f},{:.2f})\n",
                    r.time - recorder.Started(), r.frameMs, r.chainMs, r.noiseMs,
                    r.valid, r.paused, r.gliding, r.whirlwind, r.suspended, r.cameraState, r.cell,
                    r.playerStep, r.beforeStep, r.afterStep, r.correctionStep,
                    r.followed, r.reset, r.looseness, r.lag, r.rate, r.flee,
                    r.beforeFollow.x, r.beforeFollow.y, r.beforeFollow.z,
                    r.afterFollow.x, r.afterFollow.y, r.afterFollow.z,
                    r.current[0], r.current[1], r.current[2], r.current[3], r.current[4],
                    r.target[0], r.target[1], r.target[2], r.target[3], r.target[4],
                    r.windSampled, r.ground, r.windAltitude, r.windSpeed, r.windGuard,
                    r.noiseTranslation.x, r.noiseTranslation.y, r.noiseTranslation.z);
            }
            spdlog::info("[PARAGLIDE-TRACE] capture={}/{} samples={} span={:.2f}s "
                "peakFrameMs={:.2f} over50ms={} peakChainMs={:.2f} peakNoiseMs={:.2f} "
                "peakCorrectionStep={:.1f} resets={} windGuards={} glideEdges={}\n"
                "CPU camera-update intervals; chain includes engine and chained hooks, noise is DDC's following pass. "
                "pre/post are player-relative positions immediately before/after DDC follow, not GPU presents. "
                "current/target=(side,height,zoom,pitch,FOV). Wind speed is an amplitude multiplier.\n{}",
                recorder.CaptureNumber(), Recorder::kCaptureLimit, rows.size(),
                rows.empty() ? 0.0 : rows.back().time - rows.front().time,
                maxFrame, hitches, maxChain, maxNoise, maxCorrection, resets, guards, glideEdges,
                fmt::to_string(output));
            recorder.Acknowledge();
        }
    }

    Frame::Frame(RE::TESCamera* camera)
    {
        // The hook is shared by other TESCamera instances. Do not sample those
        // or let a nested update overwrite the player's in-progress sample.
        enabled_ = recorder.Enabled() && !collecting && camera &&
            camera == RE::PlayerCamera::GetSingleton();
        if (!enabled_) return;
        camera_ = camera;
        start_ = Clock::now();
        chainEnd_ = start_;
        sample = {};
        sample.time = Seconds(start_);
        collecting = true;
    }

    void Frame::AfterChain()
    {
        if (!enabled_) return;
        chainEnd_ = Clock::now();
        if (camera_->cameraRoot) {
            preNoiseTranslation = camera_->cameraRoot->local.translate;
            if (!sample.followed) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    sample.beforeFollow = sample.afterFollow =
                        PointOf(camera_->cameraRoot->world.translate - player->GetPosition());
                }
            }
        }
    }

    Frame::~Frame()
    {
        if (!enabled_) return;
        const auto end = Clock::now();
        collecting = false;
        sample.chainMs = Milliseconds(chainEnd_, start_);
        sample.noiseMs = Milliseconds(end, chainEnd_);
        auto* player = RE::PlayerCharacter::GetSingleton();
        sample.valid = player && camera_->cameraRoot && camera_->currentState;
        if (auto* ui = RE::UI::GetSingleton()) sample.paused = ui->GameIsPaused();
        auto& resolver = StateResolver::GetSingleton();
        sample.gliding = resolver.IsParagliding();
        sample.whirlwind = resolver.GetActiveShoutId() == ShoutId::WhirlwindSprint;
        sample.suspended = SettingsManager::GetSingleton().diagnosticSuspendOverrides;
        if (sample.valid) {
            sample.player = PointOf(player->GetPosition());
            if (auto* cell = player->GetParentCell()) sample.cell = cell->GetFormID();
            sample.cameraState = static_cast<int>(camera_->currentState->id);
            sample.noiseTranslation = PointOf(camera_->cameraRoot->local.translate - preNoiseTranslation);
            const auto& controller = CameraController::GetSingleton();
            const auto& current = controller.GetCurrentProfile();
            const auto& target = controller.GetTargetProfile();
            sample.current = { current.sideOffset, current.height, current.zoom, current.pitchOffset, current.fov };
            sample.target = { target.sideOffset, target.height, target.zoom, target.pitchOffset, target.fov };
        }
        if (recorder.Observe(sample)) Flush();
    }

    void Follow(const RE::NiPoint3& engine, const RE::NiPoint3& rendered,
        float looseness, float lag, float rate, float flee, bool reset)
    {
        if (!collecting) return;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            sample.beforeFollow = PointOf(engine - player->GetPosition());
            sample.afterFollow = PointOf(rendered - player->GetPosition());
            sample.followed = true;
            sample.looseness = looseness;
            sample.lag = lag;
            sample.rate = rate;
            sample.flee = flee;
            sample.reset = reset;
        }
    }

    void Wind(float ground, float altitude, float speed, bool teleportGuard)
    {
        if (!collecting) return;
        sample.windSampled = true;
        sample.ground = ground;
        sample.windAltitude = altitude;
        sample.windSpeed = speed;
        sample.windGuard = teleportGuard;
    }
}
