#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "Camera/ProjectileFlight.h"

namespace RE
{
    class NiCamera;
    class PlayerCharacter;
}

namespace DietDrCamera
{
    // Returns the live bow draw amount in [0, 1]. Reads the engine's
    // per-draw timer stack (PLAYER_RUNTIME_DATA.unkBA0). Shared with
    // CameraNoiseController so the 1p bow shake can ramp with draw.
    float GetLiveBowDrawAmount(RE::PlayerCharacter* a_ply);


    // Range-aware crosshair correction:
    //   1. All weapon states: per-frame raycast from camera forward — scale
    //      crosshair size by hit distance (close = 1.0, far = 0.4); on miss
    //      hide.
    //   2. Magic only: positional correction — raycast from the magic node
    //      along camera forward, project hit to screen, write screen X/Y to
    //      the HUD crosshair widget. Bows are intentionally NOT positionally
    //      corrected (without an arrow-skew engine patch the result is
    //      worse than vanilla).
    //
    // No UI surface; auto-on. SmoothCam ownership of the crosshair is
    // detected via the SmoothCamAPI handshake — when foreign-owned we
    // stand down silently.
    class CrosshairManager
    {
    public:
        [[nodiscard]] static CrosshairManager& GetSingleton();

        void Init();
        void Tick();
        void Shutdown();
        void ResetTracing();

        // Per-frame HUD overlay callback — registered once with the
        // SKSE Menu Framework's HudElement system. Renders the
        // predicted arrow trajectory as a fading line over the HUD
        // when the bow is drawn.
        void RenderTrajectoryHud();

        // Called when SmoothCam advertises its API; sets the owner cache.
        void OnSmoothCamInterface(void* a_iface, std::uint8_t a_version);

        // True while CrosshairManager is actively driving the crosshair for its
        // own trajectory reticle (archery / spell tracing). Other systems
        // (Better Third Person Selection) must not touch crosshair visibility
        // while this is set, or the two fight over it.
        [[nodiscard]] bool IsOverridingCrosshair() const {
            return crosshairOverrideActive_.load(std::memory_order_acquire);
        }

    private:
        CrosshairManager() = default;

        enum class AimMode : std::uint8_t { None, Magic, Bow };

        AimMode DetectAimMode();
        bool    EnsureBaseline();
        bool    RaycastFromCamera(float& outDist) const;
        bool    RaycastFromMagicNode(double& outScreenX, double& outScreenY,
                                     float& outDist) const;
        void    RestoreCrosshair();
        // forceVisible=true writes DisplayInfo.SetVisible(true) along with
        // the position write — necessary on prediction paths so the engine's
        // per-frame hide writes (sneak/menu/etc.) don't win. Restore paths
        // pass false so vanilla hide-on-sneak is preserved.
        void    WriteCrosshairScreenPos(double sx, double sy, bool forceVisible = false);
        void    WriteCrosshairScale(double scalePercent);
        void    WriteCrosshairScaleXY(double sx, double sy);

        // Stealth meter (sneak eye) relocation. While the player is
        // sneaking AND a bow/crossbow is drawn, the vanilla sneak eye
        // sits over the crosshair area where our predictive reticle
        // and trajectory render — visually noisy. We shift it to a
        // screen corner so the predictive reticle has the center to
        // itself, then restore the moment sneak ends or we leave bow
        // mode. Pattern lifted from SmoothCam's
        // SetStealthMeterPosition (`crosshair.cpp:516`).
        void    WriteStealthMeterPosition(double sx, double sy);
        void    WriteStealthMeterAlpha(double alpha);
        void    RestoreStealthMeter();

        // Bow / crossbow ballistic prediction. Returns true on a hit;
        // outScreen{X,Y} are stage-space pixel coordinates and outDist
        // is the fired-from-bow-to-impact distance. Uses the same
        // 128-step Verlet integrator + per-segment hkpCastRay as
        // SmoothCam (`crosshair.cpp:148` ProjectilePredictionCurve).
        // Non-const because it captures the trajectory polyline into
        // `trajectoryHud_` for the HUD overlay to render.
        bool    PredictArrowImpact(double& outScreenX, double& outScreenY,
                                   float& outDist);
        void    SetVisibility(bool visible);
        bool    ResolveCameraNi(RE::NiCamera*& outNiCam) const;
        bool    SmoothCamOwnsCrosshair();

        // Project a world point to HUDMovieBaseInstance-local coords.
        // Uses the GFxMovie's visible frame rect (not the engine's
        // niCam port) so the result is in the same coord system the
        // Crosshair display object expects: origin at HUD center, +y
        // down, with the swf's startup x/y offset baked in. Returns
        // false on behind-camera or projection failure.
        bool    ProjectWorldPointToHUD(const RE::NiPoint3& worldPt,
                                       double& outHudX, double& outHudY) const;

        struct Baseline
        {
            double x       = 0.0;
            double y       = 0.0;
            double width   = 0.0;
            double height  = 0.0;
            // Baseline _xscale/_yscale captured from the HUD swf. Skyrim's
            // HUDMenu doesn't initialize Crosshair at 100% — writing
            // scale=100 directly enlarges it. We treat these as our 100%
            // reference and multiply by our distance-based size factor.
            double xScale  = 100.0;
            double yScale  = 100.0;
            // SWF-startup _x/_y of StealthMeterInstance (raw stage
            // coords). Captured once on the first valid HUD frame so
            // we can offset relative to it and restore cleanly.
            double stealthX = 0.0;
            double stealthY = 0.0;
            bool   stealthValid = false;
            bool   valid   = false;
        };

        Baseline      base_{};
        AimMode       lastMode_           = AimMode::None;
        bool          baselineCaptured_   = false;
        bool          ownershipCheckDirty_ = true;
        bool          smoothCamOwns_      = false;
        bool          lastVisible_        = true;
        bool          haveLastWritten_    = false;
        std::uint32_t frameCounter_       = 0;
        std::uint32_t lastOwnerCheckFrame_ = 0;

        // Wall-clock seconds when the bow last entered drawn state. Used
        // to drive the reticle size taper — vanilla-sized while drawing,
        // shrinking to a tighter precision dot at full draw.
        float         bowDrawStartTime_   = 0.0f;
        bool          bowDrawTimerArmed_  = false;

        // Smoothed reticle pose. Each frame we lerp these toward the
        // current target (predicted impact while drawing, vanilla
        // center otherwise) so transitions in/out of draw don't snap.
        // `smoothedValid_` is false until the first frame the bow path
        // runs; on that frame we initialize to the target to avoid a
        // visual jolt from default-zero values.
        double        smoothedX_          = 0.0;
        double        smoothedY_          = 0.0;
        double        smoothedSize_       = 1.0;
        bool          smoothedValid_      = false;
        float         lastSmoothTime_     = 0.0f;

        // Edge detector for the fire moment (string-was-taut →
        // string-not-taut). Used by Tick to detect a fresh draw start
        // and reset smoothing.
        bool          stringWasTautLast_  = false;

        // Arrows retain their sampled release path. Spell/staff shots retain
        // a weak native identity, observed flight history and a refreshed path
        // ahead. Predicted arrival cannot start an impact/settling animation.
        struct ProjectileShot
        {
            std::vector<RE::NiPoint3> worldPolyline;
            float wallClockFireTime = 0, travelTime = 0, elapsedGameTime = 0;
            double fireEventSec = -1.0e9;
            int castingSource = -1;
            std::uint32_t projectileFormID = 0;
            RE::ObjectRefHandle projectile;
            ProjectileFlight::Flight<RE::NiPoint3> flight;
            RE::NiPoint3 acceleration{};
            float launchAge = 0, range = 8000, nextTraceAt = 0;
            float integrationStep = 1.0f/60.0f;
            std::uint32_t collisionFilter = 0;
        };
        std::vector<ProjectileShot> firedShots_;
        void UpdateTrackedShot(ProjectileShot& shot, float dt);
        int tracingView_ = -1;
        unsigned tracingModes_ = 0;
        mutable std::mutex          firedShotsMutex_;
        std::uint32_t               lastSpellFireVersion_ = 0;
        std::uint32_t               lastArrowFireVersion_ = 0;
        bool                        magicWasCharging_ = false;
        // Release-bridge: the projectile's detour fire event lags the
        // end-of-charge by 1-3 frames (the missile has to spawn and tick
        // before the detour publishes). Without a bridge the live preview
        // clears at charge-end and the fired-shot trail doesn't appear
        // until the event drains, so the trace blinks out for those
        // frames. magicLastChargeSec_ records the last frame we were
        // charging; while within kReleaseBridge of it AND no fired shot
        // exists yet, DetectAimMode keeps Magic mode alive and the
        // preview block keeps rebuilding (its own per-hand 100ms grace
        // holds the last line), so the preview bridges seamlessly into
        // the fired shot. Cleared the instant a fired shot appears.
        float                       magicLastChargeSec_ = -1000.0f;
        bool                        magicReleaseBridge_ = false;

        // Live previews — one per actively-charging caster / drawn
        // bow. Rebuilt every Tick from current state. Rendered with
        // full visibility (no fade), one trail + cursor per entry.
        // worldPolyline can be 2 points (straight magic line) or many
        // (curved spell or arrow arc).
        struct LiveSpellPreview
        {
            std::vector<RE::NiPoint3> worldPolyline;
            RE::NiPoint3 acceleration{};
            float launchSpeed{}, flightTime{};
            float integrationStep = 1.0f/60.0f;
            std::uint32_t projectileFormID{};
            bool ballistic{}, parallelAim{};
            // 0 = charge just started, 1 = fully charged. Used by
            // the reticle to scale + brighten the live cursor as the
            // user fills the charge — reactive, not constant.
            float chargeProgress = 1.0f;
            // Cached aim distance along camera-forward from the
            // hand at Tick time. When non-negative, the renderer
            // recomputes the polyline endpoint each frame from the
            // CURRENT camera forward + current hand position +
            // this distance, so the trace tracks the live camera
            // direction even when the user spins fast between Tick
            // and HUD render. Ballistic spells retain the complete path
            // collision-checked by Tick, including its exact hit endpoint.
            // Negative ⇒ use cached worldPolyline.
            float cachedDistance = -1.0f;
            // Anchor mode for the re-anchored endpoint. When true,
            // cachedDistance is measured from the CAMERA along camera-
            // forward and the live endpoint is camPos + camFwd*dist —
            // a true-aim line that converges on the impact point exactly
            // as the fired projectile does (it flies hand -> camera
            // target). When false (close-range hand-cast fallback),
            // cachedDistance is hand-relative and the endpoint is
            // hand + camFwd*dist (parallel aim), matching the engine's
            // close-quarters projectile behavior. Keeps the live charge
            // preview path identical to the eventual fired path so it
            // doesn't snap on release.
            bool anchorAtCamera = false;
            // Which hand sourced this preview. Resolved per-render
            // so we get the CURRENT magic-node world position rather
            // than the snapshot from Tick time.
            int castingSource = -1;
        };
        std::vector<LiveSpellPreview> liveSpellPreviews_;
        mutable std::mutex            liveSpellPreviewsMutex_;

        // Per-source dual-hand debounce. When a caster transiently
        // drops out of the charging-state set (state machine cycles),
        // keep showing its preview for ~100ms — otherwise one hand's
        // line blinks out of existence and back during dual-hand
        // charging. Tracked by CastingSource enum value (0..3).
        struct CasterDebounce
        {
            float        lastSeenSec  = -1000.0f;
            RE::NiPoint3 lastStartPos { 0.0f, 0.0f, 0.0f };
            float        lastCharge   = 0.0f;
        };
        CasterDebounce casterDebounce_[4]{};

        // Honest-origin release anchors. The charge-time preview used to
        // start at the LIVE magic-node position — which swings with the
        // charge animation and the walk gait, while the projectile actually
        // leaves from wherever the hand is at the RELEASE frame. So the
        // trace said one thing while charging and another at release
        // ("false trajectory while charging"), and strafing wobbled it.
        // These anchors LEARN the true launch origin from actual fires (the
        // detour's projectile spawn position), stored player-yaw-local so
        // they ride movement and turning smoothly, keyed by casting source
        // (RE enum 0..3) x sneaking x POV — the three things that change
        // the release pose. Until a source has fired once, previews fall
        // back to the live node (the old behavior). Session-local; relearns
        // in one cast, and the EMA tracks animation-set changes.
        struct ReleaseAnchor
        {
            RE::NiPoint3 offsetLocal{};   // x = right, y = forward, z = up
            // Fraction of the aim pitch the release node actually follows
            // about the shoulder pivot. The rigid model (gain 1.0) assumed
            // the arm tracks the FULL aim pitch; measured residuals
            // (2026-08-14: up≈-11u at -23° and -45°) showed the animation
            // only carries part of it, so the gain is LEARNED per slot from
            // pitched fires. 1.0 = the old rigid behavior (and the default
            // for calibration files that predate the field).
            float        pitchGain = 1.0f;
            bool         valid = false;
            // Snap confirmation: a far-from-anchor sample is only a POSE
            // CHANGE if a second far sample AGREES with it — rapid-fire
            // chains release mid-blend from scattered spots, and one-sample
            // snapping thrashed the anchor during spam casting.
            RE::NiPoint3 pendingSnap{};
            bool         pendingValid  = false;
            double       pendingSec    = 0.0;
            double       lastTeachSec  = 0.0;   // per-cast refractory
        };
        ReleaseAnchor      releaseAnchors_[4][2][2]{};   // [srcEnum][sneak][firstPerson]
        mutable std::mutex releaseAnchorMutex_;
        void LearnReleaseAnchor(int a_castingSourceEnum, const RE::NiPoint3& a_worldPos);
        // Consume the detour's pending fire events WITHOUT making trails.
        // Called from every early return in Tick() that happens before
        // DetectAimMode — the only other drain site. Without this the
        // sequence never advances while the bail is active and the whole
        // backlog turns into trails the instant it clears; see the
        // lock-off report in Tick's target-lock branch.
        void DiscardPendingFireEvents(bool a_learnAnchors);
        [[nodiscard]] bool PredictedReleaseOrigin(int a_castingSourceEnum,
                                                  RE::NiPoint3& a_out);
        // Persistence: the anchors are a CALIBRATION CACHE — measured data
        // the user never edits — auto-saved (debounced, a few seconds after
        // the last learn) to TraceCalibration.toml and loaded lazily on
        // first use, so the first cast of EVERY session is already honest.
        // Deliberately outside the preset system: presets are settings,
        // this is measurement.
        bool   anchorsLoaded_       = false;
        bool   anchorsDirty_        = false;
        double anchorsLastLearnSec_ = 0.0;
        void   EnsureReleaseAnchorsLoadedLocked();   // caller holds releaseAnchorMutex_
        void   FlushReleaseAnchorsIfDue(double a_nowSec);

        // (Spell eager push was removed 2026-05-21 — it produced a
        // duplicate trail at one-frame-later camera direction, visibly
        // misaligned with the projectile at fast spin. See
        // [[reference-spell-trail-alignment]] for the analysis. The
        // detour-event drain in DetectAimMode already runs in the same
        // Tick as the detour fire, so the trail appears in the same
        // render frame as the projectile.)

        // Per-hand release-edge tracking for the click-time camera
        // capture. When a hand transitions from charging-hostile-FoF
        // to not-charging, we publish the niCam state to
        // MissileProjectileDetour::PublishReleaseCapture so the detour
        // can use it at first-tick (which can be K frames after click
        // due to spawn-animation timing). Without this, fast-spinning
        // users see the projectile fly at the camera direction K
        // frames AFTER they pulled the trigger instead of at trigger-
        // pull direction (gun-sights analogy).
        bool perHandWasChargingHostile_[4] = { false, false, false, false };
        bool perHandSawFiringInCycle_[4]   = { false, false, false, false };

        // Bow release-edge tracking. On the rising edge of "string was
        // taut last frame, not anymore", eagerly push a shot into
        // firedShots_ using lastArrowWorldPath_ so the trail+reticle
        // appear instantly — no waiting for the detour fire event,
        // which can lag the engine's release by 1-2 frames.
        float lastBowFireWallSec_ = -1000.0f;

        // Most recent live arrow trajectory in world coords. Updated
        // each tick by PredictArrowImpact while the string is taut;
        // snapshotted into firedShots_ when ArrowPathDetour publishes
        // a fire event.
        std::vector<RE::NiPoint3> lastArrowWorldPath_;
        mutable std::mutex        lastArrowWorldPathMutex_;
        // Wall-clock anchor for per-frame game-time deltas. Each tick
        // computes (nowWall - lastTickWallSec_) * QGlobalTimeMultiplier
        // — wall-clock guarantees forward progress (BSTimer::delta was
        // observed returning 0 in some states), and the multiplier
        // makes Slow Time / Time Stop stretch shot timelines correctly.
        float                   lastTickWallSec_ = -1.0f;
        // Bow draw taper applied to the ImGui marker arm length
        // (vanilla 1.10 → 0.55 size feedback). The engine crosshair
        // is hidden during projectile-tracing modes, so this taper
        // is the marker's only size-change cue. 1.0 when not drawing
        // or in spell mode (no taper).
        float                   liveBowMarkerScale_ = 1.0f;
        // Captured trajectory in HUD coords, populated during the bow
        // path of PredictArrowImpact and consumed by RenderTrajectoryHud
        // (which fires every HUD frame, possibly on a different thread
        // than Tick). Pairs of (x, y). The mutex guards both the
        // vector and the showTrajectory_ flag so a flag-flip can't race
        // a half-written vector.
        std::vector<float> trajectoryHud_;
        std::mutex    trajectoryMutex_;
        bool          showTrajectory_     = false;
        // SKSE Menu Framework HudElement registration handle (so we
        // know we registered exactly once across hot-reloads).
        bool          hudElementRegistered_ = false;
        // Tracks whether the sneak eye is currently shifted off-center
        // by us. Avoids redundant Scaleform writes per frame and lets
        // us know whether a restore is needed.
        bool          stealthMeterRelocated_ = false;
        // Last offsets we wrote so we re-issue the SetDisplayInfo when
        // the user drags the offset sliders. Without this the slider
        // edits wouldn't take effect until sneak toggles.
        double        lastSneakOffsetX_      = 0.0;
        double        lastSneakOffsetY_      = 0.0;
        // Frame counter snapshot of the last frame we wanted the eye
        // relocated. Used to defer RestoreStealthMeter so the engine's
        // un-sneak fade-out completes before we snap the eye back to
        // baseline — without this the player sees the eye briefly pop
        // to center on stand-up.
        std::uint32_t lastRelocateFrame_     = 0;
        // Edge tracking for sneak meter diagnostic logging.
        bool          lastSneakWanted_       = false;
        bool          lastIsSneaking_        = false;
        // Frame counter snapshot of the last sneak-state change.
        // Used to mask the cursor for a brief window during the
        // engine's HUDMenu reset cascade — the only known way to
        // hide the one-frame flicker that even SmoothCam can't
        // prevent. Decompiled HUDMenu.swf shows the engine fires
        // gotoAndStop("Alert"/"Normal") on sneak transitions which
        // re-instantiates the Crosshair symbol with its authored
        // matrix, wiping our writes for one render frame. Hiding
        // the cursor briefly during this window is invisible to
        // the player and avoids the flicker entirely.
        std::uint32_t lastSneakChangeFrame_  = 0;
        bool          sneakChangePending_    = false;
        // TDM target-lock edge tracking. The lock-on branch writes
        // DisplayInfo.SetVisible(false) on the Crosshair GFx object every
        // frame; that flag persists on the display object after we stop
        // writing. For melee/magic states the engine never undoes it, so
        // the cursor stays invisible until the user sheaths. On the
        // falling edge we force visible=true once to release the cursor
        // back to the engine's normal show/hide.
        bool          lastTDMLocked_         = false;
        // Atomic gate read by RenderTrajectoryHud (HUD render thread)
        // so the echo skips re-applying the cursor pose while we're
        // in the sneak-mask window. Without this the HudElement echo
        // would un-hide the cursor mid-engine-reset, defeating the
        // mask.
        std::atomic<bool> cursorMaskActive_{false};
        // Last currentPower computed inside PredictArrowImpact, read
        // by Tick to gate trail visibility. Below 0.15 (~25% draw),
        // we hide the trail entirely — the integrator math at low
        // power produces a vertical-drop visual indistinguishable
        // from a full-draw arc to the user's eye.
        float         lastCurrentPower_      = 0.0f;
        // Crosshair pose echo for the HudElement callback. The engine
        // resets Crosshair position/scale on certain HUD transitions
        // (notably un-sneak with a bow drawn). Tick writes the
        // override AFTER the camera update but BEFORE the HUD render,
        // so the engine's reset can sneak in for one render frame.
        // RenderTrajectoryHud runs DURING HUD render — re-issuing
        // the same write there clobbers the engine's intervening
        // reset before the frame ships. Saved under poseMutex_;
        // active gate is the atomic so the HudElement fast path can
        // bail without taking the lock.
        std::atomic<bool>      crosshairOverrideActive_{false};
        mutable std::mutex     poseMutex_;
        double                 echoSx_     = 0.0;
        double                 echoSy_     = 0.0;
        double                 echoScaleX_ = 100.0;
        double                 echoScaleY_ = 100.0;
        // Same echo pattern for the relocated StealthMeter — fixes
        // the brief flicker the engine causes on sneak-entry, where
        // the meter pops at vanilla center for one frame before our
        // offset takes hold.
        std::atomic<bool>      stealthEchoActive_{false};
        double                 echoStealthSx_ = 0.0;
        double                 echoStealthSy_ = 0.0;
        // Cached SmoothCam V1+ interface pointer (or null if absent).
        // Stored as void* — header isn't included to avoid dragging the
        // SmoothCam-specific PCH defines into the rest of the project.
        void* smoothCamIface_ = nullptr;
    };
}
