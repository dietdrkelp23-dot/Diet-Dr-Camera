#pragma once
#include "Settings/CameraProfile.h"
#include "Camera/VanityTransition.h"
#include "Camera/TargetLockBias.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DietDrCamera
{
    class CameraController
    {
    public:
        [[nodiscard]] static CameraController& GetSingleton();

        void Update(RE::PlayerCamera* a_camera);
        void ApplyPitchOffset(RE::NiQuaternion& a_rotation);
        void ResetTransitionState();
        // Call after an edit can erase or relocate profile storage.
        static void InvalidateProfileReferences();
        // Force the camera's zoom back to Skyrim's factory-default and
        // drop the captured zoomBase so the next frame re-baselines
        // against the reset engine state. Called from
        // SettingsManager::ResetAllToVanilla so the camera actually
        // returns to vanilla zoom (otherwise it lands at zoomBase,
        // which is just whatever the engine had when the plugin first
        // started tracking — NOT necessarily zero).
        void ResetZoomBaseline();

        // Adaptive collision tightness (set by HookManager, read here)

        // Cached effective rotation for HookManager to read (previous frame)
        float cachedRotation = 0.0f;

        // Last CameraProfile pointer Update resolved as the active state's
        // profile. Read by CameraNoiseController to look up the matching
        // per-state noise profile. Updated from inside Update().
        [[nodiscard]] static CameraProfile* GetLastResolvedProfile();

        // Vanity owns the transition settings on entry AND return. Framing
        // still comes from the currently selected gameplay profile.
        [[nodiscard]] const CameraProfile* GetTransitionProfile() const
        {
            return m_vanityTransition.IsReturning() ? &m_vanityTransition.Source(targetProfile)
                                                   : GetLastResolvedProfile();
        }
        void CancelVanityTransition() { m_vanityTransition.Reset(); }

        // Last TL profile pointer Update picked for the current state +
        // sub-state — including resolution through per-weapon-type
        // overrides and specific-weapon bindings. nullptr when not locked.
        // Quick Tune reads this so its TL slider edits land on the EXACT
        // pointer the camera reads, regardless of the override / binding
        // path the picker took.
        [[nodiscard]] static CameraProfile* GetLastTLProfile();

        // When GetLastTLProfile() returned an off-table override profile (the
        // per-hand magic grid or a per-weapon-type melee grid), this is the
        // plain TLSlot-backed profile it overrides — otherwise null. Callers
        // that need to resolve a TLSlot (the enemy-override apply scopes) fall
        // back to this so those layers still reach a customized hand/weapon.
        [[nodiscard]] static CameraProfile* GetLastTLSlotAnchor();

        // Last CATEGORIES profile pointer Update resolved for the current
        // state + sub-state, with environment (indoor/outdoor) applied —
        // i.e. the value of `selected` BEFORE the target-lock swap. This
        // is the exact profile the camera would use if not locked. Quick
        // Tune's Categories box edits this directly so its target is
        // identical whether or not the player is locked on (no reverse
        // TL->Categories mapping, which can't recover per-weapon overrides
        // or the live environment variant).
        [[nodiscard]] static CameraProfile* GetLastCategoriesProfile();
        // The engine zoom offset ApplyZoom last wrote — the value the camera
        // is actually sitting at. Used by the dialogue-exit zoom ease as its
        // endpoint so the ease can't finish somewhere the camera isn't.
        [[nodiscard]] static float GetLastAppliedEngineZoom(bool& a_outValid);

        // The full posOffset + zoom this controller last left in the
        // ThirdPersonState on a steady gameplay frame. Returns false until a
        // gameplay frame has run. HookManager's weapon-draw edge guard uses it
        // to put the framing back after an engine event handler overwrites it —
        // our own writes land after _originalThirdPersonUpdate, so an engine
        // write in between would otherwise get one rendered frame to itself.
        [[nodiscard]] static bool GetLastAppliedFraming(float& a_x, float& a_y, float& a_z,
                                                        float& a_zoomTarget, float& a_zoomCurrent);
        // Drop the snapshot when it can no longer describe the live camera
        // (POV change, dialogue, menu framing) so the guard stays silent.
        static void InvalidateLastAppliedFraming();

        // Arm the mount-entry pull-back ease. MUST be called from the PRE-Update
        // hook with the engine's own posOffset.y, before CallOriginalUpdate
        // composes the first mounted frame — arming it from Update instead lets
        // one engine-composed frame at the full -300 slip through, which turned
        // a single 300-unit jump into a jump plus a 282-unit collapse. See the
        // definition for the measurement.
        static void NotifyMountEntry(float a_engineY);

        // The dismount edge, latched from the transition abort — the earliest
        // and ONLY deterministic moment DDC knows a dismount is happening. The
        // state resolver reports it one to three frames later, and in that gap
        // the y-write gate below declines to write at all, so the engine's own
        // posOffset.y (0) composes against the mounted zoom: the camera 298
        // units closer for a few frames, which is the snap. Latching here
        // closes the gap without waiting for the resolver.
        static void NotifyMountExit();

        // The world FOV ApplyFOV last resolved. Read by the kTween hold in
        // HookedUpdateCameraPost — that state doesn't tick this controller, so
        // without the hold the engine's menu camera renders its own FOV and the
        // profile FOV cuts back in on close.
        [[nodiscard]] static float GetLastAppliedWorldFov(bool& a_outValid);

        // Live power-attack direction for the resolved state (0=InPlace, 1=Fwd,
        // 2=Back, 3=Left, 4=Right; -1 = not currently a power attack), plus
        // whether the camera routed it to the BASE Power Attack profile (because
        // this direction's override is off, or a bound weapon has no directional
        // slot for it). Quick Tune reads these to tag the state "(Base)" instead
        // of the struck direction, since the base profile is what's actually in
        // play. Frozen at pause with the rest of the resolve, so it reflects the
        // state captured when the menu opened.
        [[nodiscard]] static int  GetLastPowerAttackDir();
        [[nodiscard]] static bool GetLastPowerAttackBaseRouted();
        // True when, on the locked frame, a DIRECTIONAL power attack routed to
        // the BASE Target Lock Power Attack profile because that direction's TL
        // override is off (no per-weapon binding matched either). Quick Tune's
        // Target Lock box reads this to clarify "using the base" when a direction
        // IS enabled for Categories but not for Target Lock. False when unlocked.
        [[nodiscard]] static bool GetLastTLPowerAttackBaseRouted();

        // Active enemy-override profile for the locked frame, or nullptr when
        // no override is currently splicing (not locked, target isn't a tracked
        // enemy, or the override's master toggle is off). When locked onto an
        // enemy whose override is ON, this is the profile the camera ACTUALLY
        // applies (it replaces the TL profile's fields) — so Quick Tune's
        // Target Lock box edits this instead of the base TL slot, otherwise the
        // override would silently overwrite the user's edits.
        // GetLastEnemyOverrideEnemyIdx returns the EnemyTypeIndex (0-based) of
        // that override for labelling, or -1 when none is active.
        [[nodiscard]] static CameraProfile* GetLastEnemyOverrideProfile();
        [[nodiscard]] static int            GetLastEnemyOverrideEnemyIdx();
        // When the active TL override is a player-bound CUSTOM enemy, this is its
        // index into SettingsManager::customEnemyOverrides (else -1). Lets Quick
        // Tune label the box and offer the same "Apply..." scope popup the built-in
        // enemy columns get. Mutually exclusive with a built-in EnemyIdx.
        [[nodiscard]] static int            GetLastCustomEnemyIdx();
        // Binding + slot even when only its enemy override is customized.
        [[nodiscard]] static const std::string& GetLastEnemyOverrideBindingLabel();

        // Live enemy-override aim-bias edit target for Quick Tune's Target Lock
        // box. Aim bias is NOT a CameraProfile field (the lock-aim solve reads it
        // separately), so it can't ride GetLastEnemyOverrideProfile(). These two
        // point into the same EnemyFieldOverride the solve reads; both are
        // non-null only while the lock is on an enemy whose override is active
        // this frame — i.e. exactly when GetLastEnemyOverrideProfile() is too —
        // so QT can tune precisely what's applied. Null otherwise (edit global).

        // Identity of the most recently locked-on actor, published every frame
        // a target is locked and kept until replaced (NOT cleared when the lock
        // drops) so the menu's "Bind current target" button can capture it even
        // after the lock releases on menu open. Both race and NPC-base identity
        // are captured (local form id + plugin + display name) so the user can
        // bind either granularity. valid == false until a target has been locked.
        struct LockedActorIdentity {
            bool          valid       = false;
            std::uint32_t raceFormID  = 0;     // local form id within racePlugin
            std::string   racePlugin;
            std::string   raceName;
            std::string   raceEditorID;        // for race-family substring binds
            std::uint32_t npcFormID   = 0;     // local form id within npcPlugin
            std::string   npcPlugin;
            std::string   npcName;
            // Display name of the locked actor + whether its base is flagged
            // Unique. Drive the single bind button: a generic enemy binds by exact
            // name (covers all with that name); a unique enemy binds just that one.
            std::string   enemyName;
            bool          isUnique = false;
            // Editor ids of the curated creature-type keywords this actor has
            // (subset of kCreatureKeywords) — offered as broad "bind by type".
            std::vector<std::string> keywords;
            // Member factions (rank >= 0) this actor belongs to — offered as
            // "bind by faction" (e.g. every Imperial Legion soldier). Identified
            // load-order-safely by local form id + plugin, like Race / NPC.
            struct FactionRef {
                std::uint32_t formID = 0;
                std::string   plugin;
                std::string   name;
            };
            std::vector<FactionRef> factions;
        };
        [[nodiscard]] static LockedActorIdentity GetLastLockedActorIdentity();

        // State-independent TDM yaw-control safety release. Update() only runs
        // from the third-person hook, so a lock dropped while the camera is in
        // first person (or any non-3p-ticking state) never releases the yaw the
        // lock path took -> TDM keeps owning it -> the player can't move. Call
        // this from a universal per-frame hook so the release fires in every
        // state. No-op unless the lock path took yaw and the lock is now gone.
        static void ReleaseLeakedLockYaw();

        // The two combat-lock TDM-yaw holder flags ReleaseLeakedLockYaw's
        // orphan check judges against, exposed read-only for [FREEZE]'s
        // per-holder diagnostic (HookManager.cpp) — otherwise that log can
        // only see the aggregate "do we own yaw" bit, not which of the five
        // legitimate holders it is.
        [[nodiscard]] static bool IsLockYawTaken();
        [[nodiscard]] static bool IsCombatFaceYawTaken();

        // Diagnostic counters for Quick Tune. Per-reason buckets so we
        // can tell which early-return is firing (or whether Update is
        // running through to the engine writes).
        [[nodiscard]] static std::uint32_t GetUpdateCallCount();
        [[nodiscard]] static std::uint32_t GetUpdateEarlyReturnCount();
        [[nodiscard]] static std::uint32_t GetUpdateRetSuspend();
        [[nodiscard]] static std::uint32_t GetUpdateRetNotThird();
        [[nodiscard]] static std::uint32_t GetUpdateRetNullPlayer();
        [[nodiscard]] static std::uint32_t GetUpdateRetNullRoot();
        [[nodiscard]] static std::uint32_t GetUpdateRetMenuBlock();

        // Live internal-state accessors for Quick Tune diagnostic.
        [[nodiscard]] float GetCurrentSideOffset() const;
        [[nodiscard]] float GetCurrentHeight()     const;
        [[nodiscard]] float GetCurrentZoom()       const;
        [[nodiscard]] float GetTargetSideOffset()  const;

        // Late-frame capture sink for dialogue-stutter diagnosis. Called
        // from HookedUpdateCameraPost (after the engine pipeline writes
        // cameraNI->world.rotate) and from HookedNiCameraUpdateWorldData
        // (after the scene-graph recompose, i.e. truly-final pre-render
        // state). Both writes target the LATEST row in m_diagDlgPerFrameBuf
        // — last write wins, lateWriteCount tracks how many late writes
        // landed for the row (expect 2 per frame when both hooks fire).
        // No-op outside the dialogue capture window or when no row is
        // pending. Safe to call every frame.
        enum class DlgLateCaptureSite : int {
            kUpdateCameraPost       = 1,
            kNiCameraUpdateWorldData = 2,
        };
        void CaptureDlgPerFrameLate(RE::PlayerCamera* a_camera, DlgLateCaptureSite a_site);

        // The Pitch Offset the camera is applying THIS frame (already through
        // the transition spring and any location/enemy override). Exposed for
        // the [PITCHX] probe so the capture reports what the camera actually
        // used, not what a slider says.
        [[nodiscard]] float LivePitchOffset() const { return currentPitchOffset; }

    private:
        CameraController() = default;

        void ApplyFOV(RE::PlayerCamera* a_camera, float fov);
        // dragonMode bundles two behaviors needed only for dragon riding:
        //   - permits a negative final target (no first-person fallback risk)
        //   - skips scroll-delta absorption (the dragon target-lock rewrites
        //     targetZoomOffset, which would otherwise be misread as a user
        //     scroll and permanently inflate zoomBase)
        void ApplyZoom(RE::ThirdPersonState* a_tps, float zoom, bool dragonMode = false);
        void UpdateForwardVector(RE::PlayerCharacter* a_player);

        RE::NiPoint3 playerPos{};
        RE::NiPoint3 forwardDir{};
        RE::NiPoint3 rightDir{};
        RE::NiPoint3 upDir{};

        float lastFOV  = 90.0f;
        float lastZoom = 0.0f;
        float zoomBase = 0.0f;
        bool  zoomBaseInitialized = false;
        float lastAppliedZoomOffset = 0.0f;
        float lastWrittenTargetZoom = 0.0f;
        int   zoomSettleFrames = 0;  // suppress scroll detection after re-baseline
        float originalFOV = -1.0f;
        float currentPitchOffset = 0.0f;

        // Critical-damped spring transition state. currentProfile holds
        // the smoothed-toward-target values; targetProfile holds the
        // resolved per-frame target. Each channel has its own velocity
        // so re-targeting mid-flight stays continuous (no snap to a
        // fresh lerp). Stiffness is derived from settings.transitionStyle.
        CameraProfile  targetProfile{};
        VanityTransition m_vanityTransition;
        CameraProfile  currentProfile{};
        CameraProfile* lastSelectedProfile = nullptr;  // kept only as a "first run" sentinel
        // Set when Update early-exits because the current camera state isn't
        // ThirdPerson/Mount/Dragon (e.g. player entered 1p). The next time
        // we re-enter 3p, currentProfile is reset to neutral instead of
        // snapped to the target profile, so the spring can smoothly ramp
        // the shoulder offset/zoom/height up from the engine's just-transitioned
        // pose instead of popping to the full slider values instantly.
        bool returningToThirdPerson = false;

        // Dialogue-exit smoothing: the dialogue path writes a non-zero
        // posOffsetExpected.y (CombatAddY) to zoom the camera; the non-
        // dialogue path never touches .y, so on dialogue close the value
        // would snap from whatever the last dialogue frame wrote back to
        // whatever the engine then renders. We decay it toward 0 over
        // ~0.4s after dialogue closes so the transition is imperceptible.
        float dialogueLastY           = 0.0f;
        float dialogueExitDecayTime   = 0.0f;
        float velSideOffset  = 0.0f;
        float velHeight      = 0.0f;
        float velZoom        = 0.0f;
        float velFOV         = 0.0f;
        float velRotation    = 0.0f;
        float velPitchOffset = 0.0f;

        // Spring-with-impulse transition system. The user explicitly said
        // this felt better than every curve we iterated through; the only
        // residual issue at the time ("slightly moving to the left on
        // sprint stop") was the engine's sheathed-sprint FOV pulse, which
        // we now suppress in HookManager.cpp for any sprint state.
        struct ChannelMotion {
            float position = 0.0f;
            float velocity = 0.0f;
            float target   = 0.0f;
            // (Glide ease state removed 2026-08-17 with the personalities —
            // the duration-based S-curve was the only shape that needed more
            // than the (position, velocity) pair.)
            // Move-size ("Weight") omega scale, LATCHED at the retarget edge.
            // A spring settles in ~4/omega no matter how far it has to go, so
            // a 4-unit nudge and a 90-unit pull take identical wall-clock time
            // — the tell that a machine is doing the moving. Weight scales
            // omega by (refSpan/span)^k so bigger moves take longer.
            //
            // LATCHED, never per-frame: a continuously-moving target shows a
            // tiny span every frame, which would drive the scale to its
            // ceiling and make tracking rigid. Sampled once when a DISCRETE
            // retarget arms (same at-rest edge that gates the impulse) and
            // held for that move; in-flight tracking keeps the plain slider
            // omega. 1.0 = no scaling, which is also the Weight-off value.
            float spanOmegaScale = 1.0f;
        };
        ChannelMotion mSide{}, mHeight{}, mZoom{}, mFOV{}, mRotation{}, mPitch{};

        // The mount's ~300-unit pull-back, easing out on dismount. A REAL
        // channel, grouped with side/height/zoom/pitch, because it is a
        // DISTANCE channel and shares the screen with the zoom.
        //
        // It used to be a bare (float, float) pair driven by the first-order
        // `springStep` helper while every other channel ran
        // CriticalDampedSpringExact through stepMotion. First-order starts at
        // MAXIMUM velocity; critically-damped starts at zero and accelerates.
        // So on a dismount the carry rocketed 300 units toward the player while
        // the zoom channel had barely begun, put the camera on the back of the
        // player's head, and the zoom then eased outward behind it — which is
        // word-for-word what the user reported for a dozen rounds. Matching the
        // rate was not enough; the CURVE SHAPE was the mismatch.
        ChannelMotion mMountY{};


        // Target-lock biased-aim yaw, spring-smoothed so entering and
        // (critically) exiting lock doesn't snap the camera. Unit: radians.
        float currentLockAimYaw = 0.0f;
        TargetLockBias::Motion lockProximityBias{};
        float velLockAimYaw     = 0.0f;

        // For-Honor lateral side-offset shrink, eased with the SAME spring as
        // the lock-aim yaw so it glides back to full (1.0) on lock-off instead
        // of snapping the camera sideways (the lock-off lateral jerk). Persists
        // across frames.
        float easedLockSideScale = 1.0f;
        float velLockSideScale   = 0.0f;

        // Target-switch latch for the For-Honor framing springs (lock-aim yaw +
        // side-scale). While active they run at the switch stiffness so they
        // settle together with the main yaw swing instead of trailing it as a
        // separate "fit into slot" adjustment at the end.
        bool  m_lockSwitching = false;
        float m_lockSwitchT   = 0.0f;
        // Distance-driven switch DURATION, computed once on each switch edge from
        // the swing angle (old heading -> new target bearing) divided by the
        // Target Switch Speed rate. Shared by the body-ease and the framing latch
        // so both settle over the same, angle-scaled window. Replaces the old
        // fixed Target Switch Time slider.
        float m_lockSwitchDur = 0.30f;

        // Shout-return transition latch. While a Shout sub-state is active we
        // cache the shout profile's transition-speed override; on the edge back
        // out of the shout we keep applying that override to the swing back to the
        // parent state, so a shout's transition speed governs BOTH the entry and
        // the return (not just the entry). Cleared once the framing settles or
        // after a short cap.
        bool  m_prevSubWasShout     = false;
        bool  m_shoutReturnActive   = false;
        float m_shoutReturnT        = 0.0f;
        bool  m_shoutReturnOverride = false;
        float m_shoutReturnRotation = 0.5f;
        float m_shoutReturnPitch    = 0.5f;
        float m_shoutReturnPosition = 0.5f;
        float m_shoutReturnZoom     = 0.5f;
        float m_shoutReturnFOV      = 0.5f;
        // Which channels the shout actually overrode (2026-08-19 per-setting
        // toggles). A shout that only set Zoom must hand Rotation/Pitch/
        // Position/FOV back to the globals on the return, exactly as it did on
        // the way in — without these the latch replayed all five.
        bool  m_shoutReturnSetRotation = false;
        bool  m_shoutReturnSetPitch    = false;
        bool  m_shoutReturnSetPosition = false;
        bool  m_shoutReturnSetZoom     = false;
        bool  m_shoutReturnSetFOV      = false;

        // Combat enter / exit pulse state. m_combatPulseT counts down
        // from `combatPulseDuration` to 0 after a combat edge fires;
        // m_combatPulseEnter is true for the entry pulse (positive
        // FOV/negative zoom impulse), false for exit (opposite, half mag).
        bool  m_lastInCombat   = false;
        float m_combatPulseT   = 0.0f;
        bool  m_combatPulseEnter = false;

        // Dialogue entry posOffset lockstep timer. During the cubic
        // face-lock blend (first 0.22s), write posOffsetActual in
        // lockstep with posOffsetExpected so the camera position
        // arrives in sync with the pitch instead of lagging via the
        // engine's slow 2.7%/frame posOffsetActual lerp (the "down
        // then up" entry feel). After the timer expires, switch to
        // write-expected-only — engine's slow lerp absorbs the small
        // per-frame face-lock pitch deltas without amplifying them
        // (per reference_dialogue_posoffset_lockstep_root_cause.md).
        bool  m_dialogueEntryLockstepActive = false;
        float m_dialogueEntryLockstepT      = 0.0f;

        // "Look At The Player" reverse shot — how far through the half-turn
        // the rig is. 0 = orbiting the player looking at the speaker, 1 = the
        // exact mirror, in between = swept part way round the arc between
        // them. A single scalar rather than a cached offset because both ends
        // of the shot are recomputed from live positions every frame (either
        // party can walk), and because HookManager's aim has to sweep its
        // target through the SAME angle for the two halves to stay a
        // conversation-width apart.
        float m_dlgReverseBlend = 0.0f;
        // Last frame's value, so a snap between the two ends can be detected
        // and the position filter re-seeded rather than smearing the cut.
        bool  m_prevDialogueActiveForLockstep = false;

        // Was the camera in first person on the PREVIOUS Update frame.
        // Tracked every frame (before the non-3p early-return) so the
        // dialogue branch can tell a fresh 3p dialogue open from a
        // mid-dialogue 1p->3p POV switch (the frame we first reach the 3p
        // dialogue branch after being in 1p).
        bool  m_prevFramePovFirst = false;
        // Last STEADY engine 3p posOffset.y + zoom offsets, cached every
        // gameplay (non-dialogue) third-person frame where the engine owns
        // those fields. Used as the dialogue exit/zoom baseline when dialogue
        // is entered via a mid-dialogue 1p->3p POV switch: the engine resets
        // those fields and animates the transition, so the switch-frame value
        // is transient and the open-edge capture would make the exit blend
        // snap to a stale spot. The cached steady value is correct instead.
        bool  m_gameplay3pBaselineValid   = false;
        float m_gameplay3pPosY            = 0.0f;
        float m_gameplay3pTargetZoom      = 0.0f;
        float m_gameplay3pCurrentZoom     = 0.0f;

        // One-shot diagnostic capture for dialogue entry/exit edges.
        // Set at edge detection, populated through the frame, emitted
        // once from the dialogue branch where tps is in scope. Fires
        // exactly once per edge (not hot-path).
        struct DiagDlgEdge {
            bool  pending             = false;
            bool  isEntry             = false;
            float sidePosBefore = 0.0f, sidePosAfter = 0.0f, sideTarget = 0.0f, sideVelAfter = 0.0f;
            float heightPosBefore = 0.0f, heightPosAfter = 0.0f, heightTarget = 0.0f, heightVelAfter = 0.0f;
            float zoomPosBefore       = 0.0f;
            float zoomTargetBefore    = 0.0f;
            float zoomPosAfter        = 0.0f;
            float zoomTargetAfter     = 0.0f;
            float zoomVelAfter        = 0.0f;
            float fovPosBefore = 0.0f, fovPosAfter = 0.0f, fovTarget = 0.0f, fovVelAfter = 0.0f;
            float pitchPosBefore = 0.0f, pitchPosAfter = 0.0f, pitchTarget = 0.0f, pitchVelAfter = 0.0f;
            float dt                  = 0.0f;
            bool  lockstepActiveAfter = false;
            float lockstepDuration    = 0.0f;
        };
        DiagDlgEdge m_diagDlgEdge{};

        // Per-channel arrival sampler. After dialogue entry, fires one
        // log line at each milestone in kDiagDlgSampleTimes (see .cpp)
        // showing per-channel |delta to target| and velocity. Lets us
        // see which channel is the laggard. At most 5 logs per dialogue.
        int   m_diagDlgSampleIdx     = 0;
        float m_diagDlgSampleElapsed = 0.0f;
        bool  m_diagDlgSampleActive  = false;

        // Per-frame deferred capture for dialogue-entry stutter
        // diagnosis. Buffered in-process during the capture window so
        // we don't fire synchronous spdlog I/O on every dialogue-entry
        // frame (per [no-log-in-hot-path]: that would add stutter on
        // top of whatever we're trying to measure). One flush at end.
        //
        // Two capture sites per frame:
        //   1. EARLY — at the end of CC::Update's dialogue branch, after
        //      we write posOffsetExpected/Actual and player->data.angle.x.
        //      Captures channel-spring state + our own write values.
        //   2. LATE  — from HookedUpdateCameraPost and HookedNiCameraUpdate-
        //      WorldData via CaptureDlgPerFrameLate(). Captures engine-
        //      composed cameraRoot/niCamera matrices and translations (i.e.
        //      what the renderer will actually use). Last-write-wins so the
        //      final values reflect the truly-last engine state before
        //      render. If cameraRoot writes are smooth but niCamera world
        //      values staircase, the engine recompose between them is the
        //      stutter source.
        struct DiagDlgPerFrame {
            float elapsed;
            float dt;
            // Channel-spring positions (post-stepMotion / post-blend)
            float sidePos, sideTgt;
            float heightPos, heightTgt;
            float zoomPos, zoomTgt;
            float fovPos, fovTgt;
            float pitchPos, pitchTgt;
            float blendT, blendS;
            bool  blendActive;
            bool  lockstepActive;
            // Engine state at end of dialogue frame (after our writes)
            float tpsExpectedX, tpsExpectedY, tpsExpectedZ;
            float tpsActualX,   tpsActualY,   tpsActualZ;
            float tpsCurZoom, tpsTgtZoom;
            float worldFOV;
            float dataAngleX, dataAngleZ;
            float freeRotX,   freeRotZ;
            float compY,       effZoom,     dlgY;
            // LATE capture — engine-composed scene state. Captured from
            // HookedUpdateCameraPost (after our +0x1A6 hook) and then
            // overwritten by HookedNiCameraUpdateWorldData (truly-last
            // pre-render state). cRoot* = cameraRoot. ni* = first child of
            // cameraRoot, i.e. the world-render NiCamera. Euler in radians
            // via NiMatrix3::ToEulerAnglesXYZ. lateWriteCount = how many
            // times CaptureDlgPerFrameLate ran for this row (expect 2:
            // one per hook); 0 means no late capture fired = early-return.
            float crLocalTx,   crLocalTy,   crLocalTz;
            float crWorldTx,   crWorldTy,   crWorldTz;
            float crEulerX,    crEulerY,    crEulerZ;
            float niLocalTx,   niLocalTy,   niLocalTz;
            float niWorldTx,   niWorldTy,   niWorldTz;
            float niEulerX,    niEulerY,    niEulerZ;
            float playerPosX,  playerPosY,  playerPosZ;
            int   cameraStateId;
            int   lateWriteCount;
        };
        static constexpr int kDiagDlgPerFrameCap = 200;
        DiagDlgPerFrame m_diagDlgPerFrameBuf[kDiagDlgPerFrameCap]{};
        int   m_diagDlgPerFrameCount   = 0;
        float m_diagDlgPerFrameElapsed = 0.0f;
        bool  m_diagDlgPerFrameActive  = false;

        // Always-on snapshot of the last rendered camera state, updated
        // every frame by CaptureDlgPerFrameLate (even during gameplay).
        // Logged on the dialogue entry edge as [DLG-HANDOFF] so we can see
        // the LAST GAMEPLAY frame's rendered position/rotation and compare
        // it to dialogue frame +#00 — the one transition the per-frame
        // buffer can't capture (it starts at the edge). A handoff snap
        // shows up as a large delta between this snapshot and +#00.
        struct DlgLastRendered {
            float crWorldTx = 0.0f, crWorldTy = 0.0f, crWorldTz = 0.0f;
            float niEulerX  = 0.0f, niEulerY  = 0.0f, niEulerZ  = 0.0f;
            float worldFOV  = 0.0f;
            // The PLAYER's world position sampled at the same instant as the
            // camera above. Without this, camera world position alone cannot
            // answer "did the camera zoom into the player" — the player moves
            // too (hard, during a dismount animation), so a smooth camera
            // track and a collapsing camera-to-player distance look identical.
            float plWorldX = 0.0f, plWorldY = 0.0f, plWorldZ = 0.0f;
            // The CHILD niCamera's world translate. cameraRoot is the PIVOT —
            // the pull-back distance lives on the child, so a distance computed
            // from the root measures the wrong point and hides the zoom
            // entirely. Measured the hard way. [[a-probe-that-cannot-see-its-subject]]
            float niWorldX = 0.0f, niWorldY = 0.0f, niWorldZ = 0.0f;
            int   cameraStateId = 0;
            bool  valid = false;
        };
        DlgLastRendered m_dlgLastRendered{};

        // Per-channel impulse caps passed to stepMotion. With the exact
        // (closed-form) spring these only bound the retarget IMPULSE spike so a
        // huge delta can't lurch in one frame — the spring itself paces the rest
        // and is framerate-independent. 600 matches kMaxChannelSpeed (rotation);
        // the old 60 was a documented-too-low value that throttled large zoom/
        // height/FOV transitions so they never completed within a shout/power-
        // attack window (the dialogue path bypasses these via m_dlgChanBlend).
        float m_chanCapSide   = 600.0f;
        float m_chanCapHeight = 600.0f;
        float m_chanCapZoom   = 600.0f;
        float m_chanCapFov    = 600.0f;
        float m_chanCapPitch  = 600.0f;

        // Dialogue channel blend. During the entry/exit lockstep window,
        // all five channels (side/height/zoom/fov/pitch) are driven by
        // a SHARED smoothstep over the lockstep duration instead of the
        // per-channel spring. Each channel's percent-complete is equal
        // at every frame, so they all start and arrive together — no
        // "cutting in line." Spring resumes after the window for any
        // mid-dialogue retargets and post-blend tracking. Captured on
        // every dlgEdge or dlgExitEdge; targets cached at capture so
        // mid-blend target shifts don't disrupt the lerp.
        struct DialogueChannelBlend {
            bool  active   = false;
            bool  isExit   = false;  // true when this blend is the post-dialogue exit
            float elapsed  = 0.0f;
            float duration = 0.0f;
            float startSide = 0.0f,   targetSide = 0.0f;
            float startHeight = 0.0f, targetHeight = 0.0f;
            float startZoom = 0.0f,   targetZoom = 0.0f;
            float startFov = 0.0f,    targetFov = 0.0f;
            float startPitch = 0.0f,  targetPitch = 0.0f;
            // Velocity each channel was travelling at when this blend armed
            // (units per elapsed-clock second). Zero on a fresh entry; on a
            // MID-FLIGHT re-arm (rapid preset switches — the log showed
            // bursts 50-70ms apart) it carries the old blend's analytic
            // velocity so the camera never stops dead and re-accelerates:
            // a smootherstep restarted at t=0 has zero velocity, and that
            // stop-start was the reported "snappy transitions". The blend
            // evaluates a quintic HERMITE (endpoint accelerations zero);
            // with zero start velocity it reduces exactly to the old
            // smootherstep, so fresh entries are bit-identical.
            float startVelSide = 0.0f, startVelHeight = 0.0f, startVelZoom = 0.0f;
            float startVelFov = 0.0f,  startVelPitch = 0.0f;
            // Exit-only: smoothstep Y AND Z back to baseline over the same
            // duration as the other 5 channels. Both Y and Z get pitch-
            // compensated during dialogue (compY = dlgY*cp - h*sp;
            // compZ = dlgY*sp + h*cp) — the rotation that keeps camera at
            // a fixed world position regardless of player pitch. On exit
            // the dialogue branch stops running and the normal branch
            // writes raw effectiveHeight to Z and engine restores Y, so
            // BOTH lose pitch compensation in one frame. With non-zero
            // pitch, the camera visibly snaps (Z jumps ~+8 units up at
            // typical dialogue pitch 0.13rad, Y jumps ~-40 units back).
            // Smoothstep both fields over the chanBlend window so the
            // pitch comp fades out gradually with the other channel
            // transitions.
            float startExpectedY = 0.0f;
            float startActualY   = 0.0f;
            float targetY        = 0.0f;  // = sSavedEngineY at exit capture
            float startExpectedZ = 0.0f;
            float startActualZ   = 0.0f;
            float targetZ        = 0.0f;  // = post-dialogue effectiveHeight at exit
            // The six Y/Z fields above are filled by the CLOSE-EDGE block, not
            // by the capture that sets `isExit`. Those two do not always fire
            // on the same frame: leaving dialogue straight into another menu
            // (pick "let's trade" and the Barter Menu opens as the Dialogue
            // Menu closes) arms an exit blend while the close-edge block sits
            // behind the menu-blocking early return. Without this flag the
            // blend then lerps Y and Z between whatever the PREVIOUS exit left
            // in those fields — a hard jump on a frame the player did nothing
            // to deserve, which is the occasional dialogue-exit snap. False
            // means "no Y/Z capture belongs to this blend; leave those axes
            // alone".
            bool  yzValid = false;
        };
        DialogueChannelBlend m_dlgChanBlend{};


    public:
        float GetLockAimYaw() const { return currentLockAimYaw; }

        // Snapshots used by the dialogue-camera override in HookedGetTranslation.
        // currentProfile holds the spring-smoothed profile values (including
        // dialogueProfile when the dialogue override is active); playerPos is
        // the player's root translation updated at the top of Update().
        const CameraProfile& GetCurrentProfile() const { return currentProfile; }
        // Owned copy remains valid across preset reloads, including before the
        // next controller tick. Useful when comparing requested and rendered views.
        const CameraProfile& GetTargetProfile() const { return targetProfile; }
        const RE::NiPoint3&  GetPlayerPos() const { return playerPos; }
        const RE::NiPoint3&  GetForwardDir() const { return forwardDir; }
        const RE::NiPoint3&  GetRightDir() const { return rightDir; }

        // Side-offset aspect correction (currentAspect / 16:9). Exposed so
        // anything that has to reason about where a profile PUTS the camera
        // scales side offsets the same way the camera itself does, from one
        // implementation.
        static float AspectCorrection();

        // How far through the "Look At The Player" half-turn the dialogue rig
        // is (0 = forward shot, 1 = reverse). Read by the face-lock so it can
        // sweep its aim target through the same angle the anchor is sweeping
        // through, which is what keeps the camera and what it is looking at a
        // conversation-width apart for the whole move.
        [[nodiscard]] float GetDialogueReverseBlend() const { return m_dlgReverseBlend; }

    private:

        // EMA-smoothed delta time (QueryPerformanceCounter based)
        int64_t lastPerfCount = 0;
        double  perfFreqInv   = 0.0;
        float   smoothDt      = 1.0f / 60.0f;

        // Lock-on yaw acquire spring. Active across a lock session, re-inits
        // on each lock acquire from the player's current heading so the ease
        // starts where the player is. Driven by `targetLockTrackSeconds`.
        float lockYawSpring       = 0.0f;
        float lockYawSpringVel    = 0.0f;
        bool  lockYawSpringActive = false;
        // Time-based body-ease (acquire + target switch). lockYawSpringActive
        // doubles as "ease in progress". We drive the BODY (SetPlayerYaw) with a
        // smoothstep from lockEaseStart to the target over lockEaseT (0..1) that
        // SETTLES at 1; HookManager glues the camera to this eased body so the
        // two never diverge (the divergence is the spin).
        float lockEaseStart = 0.0f;
        float lockEaseT     = 0.0f;
        // Post-ease SETTLE: after the ease hands the body back to TDM, the body
        // is not yet on the target bearing (while moving the gap can be ~1 rad).
        // Keep the camera GLUED to the body (fr=0) while TDM rotates the body
        // onto the target, so the gap closes with camera+body in lockstep
        // (no divergence = no spin) instead of the spring snapping it shut.
        bool  lockGlueSettling = false;
        float lockSettleT      = 0.0f;
        // freeRotation.x residual that smoothly decays to 0 over the same
        // duration. TDM's LookAtTarget snaps freeRotation.x toward the aim
        // value on lock-acquire — without smoothing, the camera angle jumps
        // by the user's pre-lock free-orbit amount in one frame. HookManager
        // reads this via GetLockFreeRotResidual() and adds it on top of the
        // normal per-frame freeRotation.x write.
        float lockFreeRotResidual = 0.0f;
    public:
        float GetLockFreeRotResidual() const { return lockFreeRotResidual; }
        // True while the lock body-ease (acquire/switch) is running OR settling
        // afterward — HookManager glues the camera yaw to the body during this
        // whole window so camera and body never diverge (the divergence = spin).
        bool  IsLockBodyEasing() const { return lockYawSpringActive || lockGlueSettling; }
        // Ease-out blend (0..1) of the active body-ease. HookManager uses it to
        // decay the pre-lock free-look offset in sync with the body sweep, so a
        // lock-on while looking off-target glides to frame instead of snapping
        // the camera straight onto the body (the "doesn't cleanly adjust" jerk).
        // Settling (or no ease) returns 1 -> fully glued.
        float GetLockEaseBlend() const {
            if (!lockYawSpringActive) return 1.0f;
            const float inv = 1.0f - lockEaseT;
            return 1.0f - inv * inv * inv;
        }
    };
}
