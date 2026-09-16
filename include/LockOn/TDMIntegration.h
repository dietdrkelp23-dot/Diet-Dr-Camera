#pragma once

namespace TDM_API { class IVTDM3; class IVTDM4; class IVTDM5; }

namespace DietDrCamera
{
    class TDMIntegration
    {
    public:
        [[nodiscard]] static TDMIntegration& GetSingleton();

        // Call from SKSE PostLoad messaging callback.
        void OnPostLoad();

        [[nodiscard]] bool IsAvailable() const { return api != nullptr; }
        [[nodiscard]] bool IsTargetLocked() const;
        // TDM's lock state verbatim — no dead-target override. True while
        // IsTargetLocked() is false means TDM is holding a stale lock.
        [[nodiscard]] bool GetRawTargetLockState() const;
        [[nodiscard]] RE::ActorHandle GetCurrentTarget() const;
        // Diagnostic: TDM's actual movement-input vector (x,y) this frame.
        [[nodiscard]] RE::NiPoint2 GetMovementInput() const;
        void LogMovementState(bool a_reportWarning = false) const;

        // Request / release ownership of player yaw. speedMul = 0.0 for instant.
        bool RequestYawControl(float speedMul);
        void ReleaseYawControl();
        bool SetPlayerYaw(float radians);

        // Fully disable TDM's directional-movement system (which includes
        // target-lock camera tracking). Stronger than RequestYawControl —
        // suppresses TDM's freeRotation.x writes too, not just body yaw.
        // Lock target is preserved across enable→disable→enable.
        bool RequestDisableDirectionalMovement();
        void ReleaseDisableDirectionalMovement();

        [[nodiscard]] bool HasYawControl() const { return yawOwned; }
        [[nodiscard]] bool HasDirectionalMovementDisabled() const { return dmDisabled; }

        // --- Optional capabilities (newer TDM only) ---------------------------
        // The core integration above lives in IVTDM1-3 and is always available
        // when TDM is present. The two below need a TDM that negotiated to V4 /
        // V5 (TDM 2.2.6.2+ / 2026-02 build). They are inert no-ops on older TDM,
        // so callers can use them unconditionally — but check the Supports*
        // queries if you want to branch on availability.
        [[nodiscard]] bool SupportsBehindTargetQuery() const { return apiV4 != nullptr; }
        [[nodiscard]] bool SupportsReticleControl() const { return apiV5 != nullptr; }

        // V4: true iff TDM has a lock AND its camera target is behind the locked
        // actor. False on older TDM or when not locked. Read-only query.
        [[nodiscard]] bool IsTargetLockBehindTarget() const;

        // V5: hold a claim that forces TDM's target lock OFF.
        //
        // TDM's API header comments call this "disable target lock reticle",
        // which is what the wrapper used to be named after. Its SOURCE says
        // otherwise: DirectionalMovementHandler::UpdateTargetLock() calls
        // ToggleTargetLock(false) whenever GetForceDisableTargetLock() is set,
        // so the claim doesn't hide a lock — it stops one being kept. Acquiring
        // is not gated, so a lock can flicker on for a frame before TDM's own
        // update drops it; hold the claim across the whole window you want
        // lock-free rather than reacting to a lock appearing.
        //
        // No-op (returns false) on TDM older than V5. Mirrors the
        // Request/Release DirectionalMovement watchdog-safe ownership pattern.
        bool RequestDisableTargetLock();
        void ReleaseDisableTargetLock();
        [[nodiscard]] bool HasTargetLockDisabled() const { return tlForceDisabled; }

    private:
        TDMIntegration() = default;

        // `api` always points at the negotiated interface upcast to IVTDM3*
        // (the core surface). apiV4 / apiV5 are non-null only when TDM supports
        // that version; they alias the same singleton at a wider type.
        TDM_API::IVTDM3* api              = nullptr;
        TDM_API::IVTDM4* apiV4            = nullptr;
        TDM_API::IVTDM5* apiV5            = nullptr;
        bool             yawOwned         = false;
        bool             dmDisabled       = false;
        bool             tlForceDisabled  = false;
    };
}
