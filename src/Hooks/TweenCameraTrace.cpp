#include "PCH.h"
#include "Hooks/TweenCameraTrace.h"
#include "Hooks/HookManager.h"
#include "Menus/ShowPlayerInMenusController.h"
#include "Settings/SettingsManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <fmt/format.h>

namespace DietDrCamera::TweenCameraTrace
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Snapshot
        {
            Clock::time_point time;
            const char* stage = "render";
            std::uint64_t pass = 0;
            int cameraState = -1;
            int menuMask = 0;
            int guard = -1;
            int cooldown = -1;
            bool uiDrive = false;
            bool paused = false;
            bool menuOwned = false;
            bool haveNi = false;
            bool haveThirdPerson = false;
            RE::NiPoint3 player{};
            RE::NiPoint3 body{};
            RE::NiPoint3 rootLocal{};
            RE::NiPoint3 rootWorld{};
            RE::NiPoint3 rendered{};
            RE::NiPoint3 forward{};
            RE::NiPoint3 translation{};
            RE::NiPoint3 actual{};
            RE::NiPoint3 expected{};
            RE::NiPoint2 freeRotation{};
            float zoomCurrent = 0.0f;
            float zoomTarget = 0.0f;
            float fov = 0.0f;
            std::array<float, 4> animatedDelta{};
            RE::NiQuaternion animationRotation{};
            RE::NiPoint3 cameraBoneLocal{};
            RE::NiPoint3 cameraBoneWorld{};
            std::uintptr_t cameraBone = 0;
            std::uint64_t animationA0 = 0;
            std::uint32_t animationD0 = 0;
            unsigned int animationFlags = 0;
            std::array<char, 64> animatedBoneName{};
        };

        std::mutex traceMutex;
        std::array<Snapshot, 40> preRoll{};
        std::size_t preRollCount = 0;
        std::size_t preRollNext = 0;
        std::array<Snapshot, 1024> pending{};
        std::size_t pendingCount = 0;
        std::size_t totalSamples = 0;
        std::uint64_t cameraPass = 0;
        int captures = 0;
        bool active = false;
        bool selectedPass = false;
        Clock::time_point started{};
        Clock::time_point deadline{};
        Clock::time_point detailedUntil{};
        Clock::time_point lastPass{};
        Clock::time_point lastRender{};
        Clock::time_point lastFlush{};

        bool ReadSnapshot(Snapshot& snapshot, const char* stage)
        {
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!camera || !camera->cameraRoot || !player) return false;

            snapshot.time = Clock::now();
            snapshot.stage = stage;
            snapshot.pass = cameraPass;
            snapshot.uiDrive = HookManager::IsUiDriveActive();
            snapshot.cameraState = camera->currentState ? static_cast<int>(camera->currentState->id) : -1;
            snapshot.menuOwned = ShowPlayerInMenusController::GetSingleton().IsActive();
            if (auto* ui = RE::UI::GetSingleton()) {
                snapshot.paused = ui->GameIsPaused();
                constexpr std::array<const char*, 6> menus{
                    "TweenMenu", "InventoryMenu", "MagicMenu", "ContainerMenu", "FavoritesMenu", "Dialogue Menu"
                };
                for (std::size_t index = 0; index < menus.size(); ++index) {
                    if (ui->IsMenuOpen(menus[index])) snapshot.menuMask |= 1 << index;
                }
            }
            snapshot.player = player->GetPosition();
            snapshot.body = player->data.angle;
            snapshot.rootLocal = camera->cameraRoot->local.translate - snapshot.player;
            snapshot.rootWorld = camera->cameraRoot->world.translate - snapshot.player;
            snapshot.fov = camera->worldFOV;
            if (auto* root = camera->cameraRoot->AsNode(); root && !root->GetChildren().empty()) {
                if (auto* rendered = skyrim_cast<RE::NiCamera*>(root->GetChildren()[0].get())) {
                    snapshot.haveNi = true;
                    snapshot.rendered = rendered->world.translate - snapshot.player;
                    snapshot.forward = { rendered->world.rotate.entry[0][0],
                                         rendered->world.rotate.entry[1][0],
                                         rendered->world.rotate.entry[2][0] };
                }
            }
            if (auto* thirdPerson = skyrim_cast<RE::ThirdPersonState*>(
                    camera->cameraStates[RE::CameraState::kThirdPerson].get())) {
                snapshot.haveThirdPerson = true;
                snapshot.translation = thirdPerson->translation - snapshot.player;
                snapshot.actual = thirdPerson->posOffsetActual;
                snapshot.expected = thirdPerson->posOffsetExpected;
                snapshot.freeRotation = thirdPerson->freeRotation;
                snapshot.zoomCurrent = thirdPerson->currentZoomOffset;
                snapshot.zoomTarget = thirdPerson->targetZoomOffset;
                std::memcpy(snapshot.animatedDelta.data(), &thirdPerson->unkC0, sizeof(snapshot.animatedDelta));
                snapshot.animationRotation = thirdPerson->animationRotation;
                snapshot.animationA0 = thirdPerson->unkA0;
                snapshot.animationD0 = thirdPerson->unkD0;
                snapshot.animationFlags = (thirdPerson->applyOffsets ? 1u : 0u) |
                                          (thirdPerson->toggleAnimCam ? 2u : 0u) |
                                          (thirdPerson->stateNotActive ? 4u : 0u) |
                                          (thirdPerson->freeRotationEnabled ? 8u : 0u);
                std::snprintf(snapshot.animatedBoneName.data(), snapshot.animatedBoneName.size(), "%s",
                              thirdPerson->animatedBoneName.c_str() ? thirdPerson->animatedBoneName.c_str() : "");
                if (auto* bone = thirdPerson->thirdPersonCameraObj) {
                    snapshot.cameraBone = reinterpret_cast<std::uintptr_t>(bone);
                    snapshot.cameraBoneLocal = bone->local.translate;
                    snapshot.cameraBoneWorld = bone->world.translate - snapshot.player;
                }
            }
            return true;
        }

        void Flush(const char* reason)
        {
            if (!pendingCount) return;
            fmt::memory_buffer output;
            for (std::size_t index = 0; index < pendingCount; ++index) {
                const auto& sample = pending[index];
                fmt::format_to(std::back_inserter(output),
                    "[TWEEN-TRACE] t={:+.4f} pass={} stage={} ui={} cam={} menus={} paused={} owned={} ni={} tps={} "
                    "player=({:.3f},{:.3f},{:.3f}) body=({:.4f},{:.4f},{:.4f}) "
                    "rootL=({:.3f},{:.3f},{:.3f}) rootW=({:.3f},{:.3f},{:.3f}) "
                    "render=({:.3f},{:.3f},{:.3f}) trans=({:.3f},{:.3f},{:.3f}) "
                    "fwd=({:.4f},{:.4f},{:.4f}) actual=({:.3f},{:.3f},{:.3f}) expected=({:.3f},{:.3f},{:.3f}) "
                    "orbit=({:.4f},{:.4f}) zoom=({:.4f},{:.4f}) fov={:.3f} "
                    "anim=({:.4f},{:.4f},{:.4f},{:.4f}) guard={} cooldown={} "
                    "animQ=({:.4f},{:.4f},{:.4f},{:.4f}) a0={:016X} d0={:08X} flags={} "
                    "bone={:X} boneName=\"{}\" boneL=({:.3f},{:.3f},{:.3f}) boneW=({:.3f},{:.3f},{:.3f})\n",
                    std::chrono::duration<double>(sample.time - started).count(), sample.pass, sample.stage,
                    sample.uiDrive, sample.cameraState, sample.menuMask, sample.paused, sample.menuOwned,
                    sample.haveNi, sample.haveThirdPerson,
                    sample.player.x, sample.player.y, sample.player.z, sample.body.x, sample.body.y, sample.body.z,
                    sample.rootLocal.x, sample.rootLocal.y, sample.rootLocal.z,
                    sample.rootWorld.x, sample.rootWorld.y, sample.rootWorld.z,
                    sample.rendered.x, sample.rendered.y, sample.rendered.z,
                    sample.translation.x, sample.translation.y, sample.translation.z,
                    sample.forward.x, sample.forward.y, sample.forward.z,
                    sample.actual.x, sample.actual.y, sample.actual.z,
                    sample.expected.x, sample.expected.y, sample.expected.z,
                    sample.freeRotation.x, sample.freeRotation.y, sample.zoomCurrent, sample.zoomTarget, sample.fov,
                    sample.animatedDelta[0], sample.animatedDelta[1], sample.animatedDelta[2], sample.animatedDelta[3],
                    sample.guard, sample.cooldown,
                    sample.animationRotation.w, sample.animationRotation.x,
                    sample.animationRotation.y, sample.animationRotation.z,
                    sample.animationA0, sample.animationD0, sample.animationFlags,
                    sample.cameraBone, sample.animatedBoneName.data(),
                    sample.cameraBoneLocal.x, sample.cameraBoneLocal.y, sample.cameraBoneLocal.z,
                    sample.cameraBoneWorld.x, sample.cameraBoneWorld.y, sample.cameraBoneWorld.z);
            }
            spdlog::debug("[TWEEN-TRACE] capture={} batch={} samples={}\n{}", captures, reason, pendingCount, fmt::to_string(output));
            pendingCount = 0;
            lastFlush = Clock::now();
        }

        bool CheckWindow(Clock::time_point now)
        {
            if (active && (now >= deadline || now - started >= std::chrono::seconds(60) || totalSamples >= 16000)) {
                Flush("end");
                spdlog::debug("[TWEEN-TRACE] END capture={} duration={:.2f}s samples={}", captures,
                             std::chrono::duration<double>(now - started).count(), totalSamples);
                active = false;
                selectedPass = false;
            }
            return active;
        }

        void Append(Snapshot snapshot)
        {
            if (pendingCount == pending.size()) Flush("buffer");
            pending[pendingCount++] = snapshot;
            ++totalSamples;
            if (snapshot.time - lastFlush >= std::chrono::seconds(2)) Flush("periodic");
        }

        void SampleLocked(const char* stage, int guard = -1, int cooldown = -1)
        {
            Snapshot snapshot;
            if (!ReadSnapshot(snapshot, stage)) return;
            snapshot.guard = guard;
            snapshot.cooldown = cooldown;
            Append(snapshot);
        }
    }

    void OnMenuEvent(std::string_view menuName, bool opening)
    {
        if (!SettingsManager::GetSingleton().verboseLogging) return;
        std::lock_guard lock(traceMutex);
        const auto now = Clock::now();
        CheckWindow(now);
        if (!active) {
            if (menuName != "TweenMenu" || !opening || captures >= 2) return;
            active = true;
            ++captures;
            started = now;
            deadline = now + std::chrono::seconds(60);
            lastFlush = now;
            totalSamples = 0;
            const auto& entry = SettingsManager::GetSingleton().showPlayerInTween;
            spdlog::debug("[TWEEN-TRACE] BEGIN v3 capture={} custom={} unpause={} movement={} cameraControl={} "
                         "| positions are world-axis offsets from player except player itself; body/orbit are radians; "
                         "anim=(x,y,z,blend); flags bits=applyOffsets:1 toggleAnimCam:2 stateNotActive:4 freeRotation:8; "
                         "boneL is bone-local and boneW is player-relative; animQ=(w,x,y,z); "
                         "menus bits=Tween:1 Inventory:2 Magic:4 Container:8 Favorites:16 Dialogue:32; "
                         "ui marks the extra unpaused UI drive; pass identifies the most recent 3p hook entry",
                         captures, entry.enabled, entry.unpauseGame, entry.allowMovement, entry.allowCameraControl);
            for (std::size_t index = 0; index < preRollCount; ++index) {
                auto sample = preRoll[(preRollNext + preRoll.size() - preRollCount + index) % preRoll.size()];
                sample.stage = "pre-render";
                Append(sample);
            }
        }
        deadline = now + std::chrono::seconds(opening ? 60 : 20);
        detailedUntil = now + std::chrono::milliseconds(400);
        spdlog::debug("[TWEEN-TRACE] EVENT capture={} t={:+.4f} menu={} opening={}", captures,
                     std::chrono::duration<double>(now - started).count(), menuName, opening);
        SampleLocked(opening ? "menu-open-before" : "menu-close-before");
    }

    void BeginCameraPass()
    {
        if (!SettingsManager::GetSingleton().verboseLogging) return;
        std::lock_guard lock(traceMutex);
        ++cameraPass;
        const auto now = Clock::now();
        selectedPass = CheckWindow(now) && (now < detailedUntil || now - lastPass >= std::chrono::milliseconds(50));
        if (selectedPass) {
            lastPass = now;
            SampleLocked("3p-enter");
        }
    }

    void Sample(const char* stage, bool force)
    {
        if (!SettingsManager::GetSingleton().verboseLogging) return;
        std::lock_guard lock(traceMutex);
        if (CheckWindow(Clock::now()) && (selectedPass || force)) SampleLocked(stage);
    }

    void SampleGuard(bool enabled, int cooldown)
    {
        if (!SettingsManager::GetSingleton().verboseLogging) return;
        std::lock_guard lock(traceMutex);
        if (CheckWindow(Clock::now()) && selectedPass) SampleLocked("guard-after", enabled ? 1 : 0, cooldown);
    }

    RenderSample::~RenderSample()
    {
        if (!SettingsManager::GetSingleton().verboseLogging) return;
        std::lock_guard lock(traceMutex);
        if (captures >= 2 && !active) return;
        const auto now = Clock::now();
        const bool capturing = CheckWindow(now);
        if (now >= detailedUntil && now - lastRender < std::chrono::milliseconds(50)) return;
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera_ || !camera || !camera->cameraRoot) return;
        auto* root = camera->cameraRoot->AsNode();
        if (!root || root->GetChildren().empty() || root->GetChildren()[0].get() != camera_) return;
        Snapshot snapshot;
        if (!ReadSnapshot(snapshot, "render-after")) return;
        lastRender = now;
        preRoll[preRollNext] = snapshot;
        preRollNext = (preRollNext + 1) % preRoll.size();
        preRollCount = (std::min)(preRollCount + 1, preRoll.size());
        if (capturing) Append(snapshot);
    }
}
