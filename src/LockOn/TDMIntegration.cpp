#include "PCH.h"
#include <Windows.h>
#include "LockOn/TDMIntegration.h"
#include "vendor/TrueDirectionalMovementAPI.h"

namespace DietDrCamera
{
    TDMIntegration& TDMIntegration::GetSingleton()
    {
        static TDMIntegration instance;
        return instance;
    }

    void TDMIntegration::OnPostLoad()
    {
        // Negotiate the highest interface this TDM build supports, newest first.
        // The 8 core methods we depend on live in IVTDM1-3 and have been stable
        // for years; V4 (IsTargetLockBehindTarget) and V5 (disable lock reticle)
        // are OPTIONAL extras only newer TDM exposes.
        //
        // We MUST fall back: TDM's RequestPluginAPI returns nullptr for an
        // interface version it doesn't know, so a bare V5 request against an
        // older TDM (e.g. base 2.2.6, which only knows V1-V3) would strand us
        // with NO integration at all. Requesting V5 -> V4 -> V3 keeps the core
        // path working everywhere and lights up the extras only when present.
        // Every version returns the same singleton, so the wider-typed pointers
        // just alias `api` at a richer interface.
        if (auto* raw = TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V5)) {
            apiV5 = static_cast<TDM_API::IVTDM5*>(raw);
            apiV4 = apiV5;  // upcast IVTDM5* -> IVTDM4*
            api   = apiV5;  // upcast IVTDM5* -> IVTDM3*
            spdlog::info("TDM: IVTDM5 acquired (core + behind-target query + reticle-disable)");
            return;
        }
        if (auto* raw = TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V4)) {
            apiV4 = static_cast<TDM_API::IVTDM4*>(raw);
            api   = apiV4;  // upcast IVTDM4* -> IVTDM3*
            spdlog::info("TDM: IVTDM4 acquired (core + behind-target query; no reticle-disable)");
            return;
        }
        if (auto* raw = TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V3)) {
            api = static_cast<TDM_API::IVTDM3*>(raw);
            spdlog::info("TDM: IVTDM3 acquired (core integration only)");
            return;
        }
        spdlog::info("TDM: not detected (TrueDirectionalMovement.dll not loaded or API request failed)");
    }

    bool TDMIntegration::IsTargetLocked() const
    {
        if (!api || !api->GetTargetLockState()) return false;
        // TDM can leave the lock state set for a frame or more after the locked
        // target dies (with a null or dead handle). Treat "locked with no living
        // target" as unlocked so the camera RELEASES (smooth release spring)
        // instead of staying centered behind the player and turning with
        // movement. Symptom otherwise: kill an enemy -> camera glues directly
        // behind the player and follows character movement.
        //
        // IsDead() lags the killing blow by a frame or more (lifeState only
        // flips to kDead after death processing/animation starts), so also
        // treat <=0 health as no-longer-a-valid-lock to close that window.
        auto target = api->GetCurrentTarget().get();
        if (!target || target->IsDead()) return false;
        if (auto* avo = target->AsActorValueOwner();
            avo && avo->GetActorValue(RE::ActorValue::kHealth) <= 0.0f) {
            return false;
        }
        return true;
    }

    bool TDMIntegration::GetRawTargetLockState() const
    {
        return api && api->GetTargetLockState();
    }

    RE::ActorHandle TDMIntegration::GetCurrentTarget() const
    {
        if (!api) return {};
        return api->GetCurrentTarget();
    }

    RE::NiPoint2 TDMIntegration::GetMovementInput() const
    {
        if (!api) return {};
        return api->GetActualMovementInput();
    }

    bool TDMIntegration::RequestYawControl(float speedMul)
    {
        if (!api) return false;
        if (yawOwned) return true;
        const auto handle = SKSE::GetPluginHandle();
        const auto res = api->RequestYawControl(handle, speedMul);
        yawOwned = (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::AlreadyGiven);
        if (!yawOwned) {
            spdlog::warn("TDM: RequestYawControl failed (result={})", static_cast<int>(res));
        } else {
            spdlog::debug("TDM: yaw control acquired (result={})", static_cast<int>(res));
        }
        return yawOwned;
    }

    void TDMIntegration::LogMovementState(bool a_reportWarning) const
    {
        const auto level = a_reportWarning ? spdlog::level::warn : spdlog::level::debug;
        if (!spdlog::should_log(level)) return;
        if (!api) {
            spdlog::log(level, "[FREEZE-STATE] TDM unavailable");
            return;
        }
        spdlog::log(level, "[FREEZE-STATE] TDM mode={} enabled={} dmOwner={} ddcHandle={} "
                     "yawMirror={} dmMirror={} thread={}/{}",
                     static_cast<int>(api->GetDirectionalMovementMode()),
                     api->GetDirectionalMovementState(), api->GetDisableDirectionalMovementOwner(),
                     SKSE::GetPluginHandle(), yawOwned, dmDisabled,
                     GetCurrentThreadId(), api->GetTDMThreadId());
    }

    void TDMIntegration::ReleaseYawControl()
    {
        if (!api || !yawOwned) return;
        const auto res = api->ReleaseYawControl(SKSE::GetPluginHandle());
        // Only drop OUR ownership flag when TDM confirms it actually let go.
        //   OK       -> released, clear.
        //   NotOwner -> TDM agrees we hold nothing, safe to clear.
        //   anything else (e.g. BadThread) -> TDM STILL owns it on its side,
        //     so DO NOT clear yawOwned. If we cleared it here, HasYawControl()
        //     would report false and the per-frame ReleaseLeakedLockYaw()
        //     watchdog would go blind while TDM keeps driving the body yaw and
        //     blocking movement input (the "can't move, body rotates with the
        //     camera" bug). Keeping the flag true makes the watchdog re-issue
        //     the release every frame until it sticks.
        if (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::NotOwner) {
            yawOwned = false;
            spdlog::debug("TDM: yaw control released (result={})", static_cast<int>(res));
        } else {
            spdlog::warn("TDM: ReleaseYawControl returned {} — keeping claim for watchdog retry",
                         static_cast<int>(res));
        }
    }

    bool TDMIntegration::SetPlayerYaw(float radians)
    {
        if (!api || !yawOwned) return false;
        return api->SetPlayerYaw(SKSE::GetPluginHandle(), radians) == TDM_API::APIResult::OK;
    }

    bool TDMIntegration::RequestDisableDirectionalMovement()
    {
        if (!api) return false;
        if (dmDisabled) return true;
        const auto handle = SKSE::GetPluginHandle();
        const auto res = api->RequestDisableDirectionalMovement(handle);
        dmDisabled = (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::AlreadyGiven);
        if (!dmDisabled) {
            spdlog::warn("TDM: RequestDisableDirectionalMovement failed (result={})", static_cast<int>(res));
        } else {
            spdlog::debug("TDM: directional movement disabled (result={})", static_cast<int>(res));
        }
        return dmDisabled;
    }

    void TDMIntegration::ReleaseDisableDirectionalMovement()
    {
        if (!api || !dmDisabled) return;
        const auto res = api->ReleaseDisableDirectionalMovement(SKSE::GetPluginHandle());
        // Same guard as ReleaseYawControl: only clear our flag when TDM
        // confirms the release (OK / NotOwner). On any other result TDM still
        // has directional movement disabled on its side, so keep the flag so
        // the watchdog retries rather than stranding the player unable to move.
        if (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::NotOwner) {
            dmDisabled = false;
            spdlog::debug("TDM: directional movement re-enabled (result={})", static_cast<int>(res));
        } else {
            spdlog::warn("TDM: ReleaseDisableDirectionalMovement returned {} — keeping claim for watchdog retry",
                         static_cast<int>(res));
        }
    }

    bool TDMIntegration::IsTargetLockBehindTarget() const
    {
        // V4-only query. Absent on older TDM -> report "not behind" so callers
        // degrade to their normal (front/side) assumption rather than misbehave.
        if (!apiV4) return false;
        return apiV4->IsTargetLockBehindTarget();
    }

    bool TDMIntegration::RequestDisableTargetLock()
    {
        // V5-only. Silently no-op on older TDM (there is no such claim to
        // make there); report false so callers can tell and degrade.
        if (!apiV5) return false;
        if (tlForceDisabled) return true;
        const auto res = apiV5->RequestDisableTargetLock(SKSE::GetPluginHandle());
        tlForceDisabled = (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::AlreadyGiven);
        if (!tlForceDisabled) {
            spdlog::warn("TDM: RequestDisableTargetLock failed (result={})", static_cast<int>(res));
        } else {
            spdlog::debug("TDM: target lock force-disabled (result={})", static_cast<int>(res));
        }
        return tlForceDisabled;
    }

    void TDMIntegration::ReleaseDisableTargetLock()
    {
        if (!apiV5 || !tlForceDisabled) return;
        const auto res = apiV5->ReleaseDisableTargetLock(SKSE::GetPluginHandle());
        // Same watchdog-safe guard as the other releases: only drop our flag
        // when TDM confirms (OK / NotOwner). On any other result TDM still owns
        // the suppression on its side, so keep the flag so a retry re-issues it.
        if (res == TDM_API::APIResult::OK || res == TDM_API::APIResult::NotOwner) {
            tlForceDisabled = false;
            spdlog::debug("TDM: target lock re-enabled (result={})", static_cast<int>(res));
        } else {
            spdlog::warn("TDM: ReleaseDisableTargetLock returned {} — keeping claim for retry",
                         static_cast<int>(res));
        }
    }
}
