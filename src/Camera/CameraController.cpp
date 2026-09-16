#include "PCH.h"
#include "Camera/VanityCamera.h"
#define NOMINMAX
#include <Windows.h>
#include "Camera/AnimationCameraController.h"
#include "Camera/CameraController.h"
#include "Camera/StateResolver.h"
#include "Core/Spring.h"
#include "Dialogue/DialogueLookPicker.h"
#include "Hooks/HookManager.h"
#include "LockOn/EnemyDetector.h"
#include "LockOn/TDMIntegration.h"
#include "Menus/ShowPlayerInMenusController.h"
#include "Settings/SettingsManager.h"
#include "Settings/EquippedItemBinding.h"
#include "Settings/Defaults.h"
#include "UI/MenuUI.h"
#include "Unpause/UnpauseManager.h"

namespace DietDrCamera
{
    namespace
    {
        // Resolve the TL magic profile and, when the per-hand grid wins, record
        // the plain TL slot it hung off in a_anchor. Everything slot-keyed
        // (enemy overrides and the menu's apply scopes) reads the anchor, so a
        // customized hand no longer hides the state from those layers.
        CameraProfile* PickTLMagicAnchored(SettingsManager& a_settings,
                                           CameraProfile*& a_anchor, CameraProfile*& a_anchorFor,
                                           MagicSchool a_school, CastType a_cast, bool a_sneak)
        {
            CameraProfile* picked = a_settings.PickTLMagicProfile(a_school, a_cast, a_sneak);
            CameraProfile* slot   = a_settings.PickTLMagicProfile(a_school, a_cast, a_sneak,
                                                                  /*a_ignoreHandOverride=*/true);
            if (picked != slot) { a_anchor = slot; a_anchorFor = picked; }
            return picked;
        }

        // Aspect-ratio correction for side-offset. Skyrim is Hor+: vertical
        // FOV is fixed, horizontal grows with aspect. A side-offset value
        // tuned to put the player at the rule-of-thirds line on 16:9 would
        // appear nearly centered on 32:9 because the horizontal frustum is
        // 2x wider. Scale side-offset by (currentAspect / 16:9) so the
        // screen-relative framing is consistent across aspect ratios.
        // Storage stays in raw 16:9 units so presets are portable across
        // users with different monitors. Returns 1.0 on any failure so we
        // never break the camera.
        // Math:    Sx_corrected = Sx_raw * (A / (16/9))
        // Self-check: at A = 16:9 the factor is exactly 1.000 (no-op).
        // SmoothCam GH#80: arrows ~3ft off at 30ft on 32:9 â€” same root cause.
        float GetAspectFactor()
        {
            constexpr float kBaseAspect = 16.0f / 9.0f;
            auto* state = RE::BSGraphics::State::GetSingleton();
            if (!state) return 1.0f;

            float w = static_cast<float>(state->screenWidth);
            float h = static_cast<float>(state->screenHeight);

            // Letterbox path: the screen has black bars, but the engine
            // renders to frameBufferViewport at its own aspect. Use that
            // when the flag is set and the viewport dims are valid.
            if (state->GetLetterbox() &&
                state->frameBufferViewport[0] > 0 &&
                state->frameBufferViewport[1] > 0)
            {
                w = static_cast<float>(state->frameBufferViewport[0]);
                h = static_cast<float>(state->frameBufferViewport[1]);
            }

            if (w <= 0.0f || h <= 0.0f || w > 16384.0f || h > 16384.0f) return 1.0f;
            const float factor = (w / h) / kBaseAspect;

            // One-time startup diagnostic so a non-ultrawide user can
            // verify the detection is sane (factor must be ~1.000 on 16:9).
            // Multi-aspect users can confirm a sensible factor (e.g.
            // 1.313 at 21:9, 2.000 at 32:9).
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                spdlog::info(
                    "AspectCorrection: screen={}x{} letterbox={} aspect={:.4f} factor={:.4f}",
                    state->screenWidth, state->screenHeight,
                    state->GetLetterbox() ? 1 : 0,
                    w / h, factor);
            }
            return factor;
        }
    }

    CameraController& CameraController::GetSingleton()
    {
        static CameraController instance;
        return instance;
    }

    // Published for CameraNoiseController so it can look up the matching
    // per-state noise profile without re-doing the full stateâ†’profile
    // switch. Updated each tick of CameraController::Update.
    static CameraProfile* sLastResolvedProfile = nullptr;
    CameraProfile* CameraController::GetLastResolvedProfile()
    {
        return sLastResolvedProfile;
    }

    static CameraProfile* sLastTLProfile = nullptr;
    CameraProfile* CameraController::GetLastTLProfile()
    {
        return sLastTLProfile;
    }

    static CameraProfile* sLastTLSlotAnchor = nullptr;
    CameraProfile* CameraController::GetLastTLSlotAnchor()
    {
        return sLastTLSlotAnchor;
    }

    // True while the combat-lock path owns TDM yaw (set on the target-switch
    // take, cleared at every release site). File-scope so ReleaseLeakedLockYaw
    // can run the safety release from outside Update() â€” Update only ticks in
    // third person, so 1p/transform lock-offs need a universal hook.
    static bool s_lockYawTaken = false;

    // True while the combat face-target assist owns TDM yaw. DDC fills the gap
    // TDM leaves: TDM bails its target-facing whenever playerFlags.isSprinting
    // is set (research_acc/tdm DirectionalMovementHandler.cpp:1024) â€” but that
    // flag is "WANTS to sprint" (button held), not "is sprinting". Hold sprint
    // and cast Flames at a locked target: Skyrim can't sprint while casting, but
    // the held button keeps the flag true, so TDM never re-faces and the swing/
    // stream flies wherever the body points (free 360s). The assist faces the
    // locked target during any combat action regardless of that flag. A second
    // legitimate yaw holder the orphan watchdog must know about.
    static bool s_combatFaceYawTaken = false;

    bool CameraController::IsLockYawTaken()       { return s_lockYawTaken; }
    bool CameraController::IsCombatFaceYawTaken() { return s_combatFaceYawTaken; }

    void CameraController::ReleaseLeakedLockYaw()
    {
        auto& tdm = TDMIntegration::GetSingleton();
        const bool locked = tdm.IsAvailable() && tdm.IsTargetLocked();
        if (s_lockYawTaken && !locked) {
            if (tdm.HasYawControl()) tdm.ReleaseYawControl();
            s_lockYawTaken = false;
        }

        // Combat face-target assist safety release. The in-Update state machine
        // does the normal release (attack/cast ends), but Update only ticks in
        // 3p â€” if the lock drops while Update isn't ticking (e.g. yanked to 1p),
        // free the claim here so it can't leak into the "can cast but can't
        // walk" freeze.
        if (s_combatFaceYawTaken && !locked) {
            if (tdm.HasYawControl()) tdm.ReleaseYawControl();
            s_combatFaceYawTaken = false;
        }

        // Orphan watchdog. Exactly five paths legitimately hold TDM claims:
        // the combat lock's switch ease (s_lockYawTaken), the combat face-target
        // assist (s_combatFaceYawTaken), the Show-Player-In-Menus framing (its
        // active_ flag), the 3p dialogue body-face for non-Actor speakers
        // (HookManager::IsDialogueFacingBodyYaw â€” the Statue of Mara etc.),
        // and Quick Tune's directional-movement disable (taken while QT is
        // open with a target locked, so its R3 POV toggle can't drop the
        // lock). A claim alive while NONE is live is a leak â€” and a leaked
        // yaw claim makes TDM's ProcessInput early-return, which is the "can
        // cast but can't walk" lock-off freeze. This runs on the universal
        // camera hook (every state, every frame), so any leak heals in one
        // frame; the warn identifies the escape path so the root cause can be
        // fixed rather than papered over.
        //
        // THE QT HOLDER WAS ADDED WITHOUT BEING REGISTERED HERE, and the
        // 21:59 log caught the consequence in one millisecond: "directional
        // movement disabled" â†’ "orphaned â€¦ releasing" â†’ "re-enabled", every
        // time QT opened while locked â€” after which QT's close-path release
        // ran against a claim it no longer held. A watchdog that judges
        // claims against a holder list is only as correct as the list; every
        // new holder must be added the same commit it starts claiming.
        if (!s_lockYawTaken && !s_combatFaceYawTaken &&
            !ShowPlayerInMenusController::GetSingleton().IsActive() &&
            !HookManager::IsDialogueFacingBodyYaw() &&
            !MenuUI::HasQuickTuneDMClaim()) {
            if (tdm.HasYawControl()) {
                spdlog::warn("TDM watchdog: orphaned yaw-control claim with no live holder â€” releasing");
                tdm.ReleaseYawControl();
            }
            if (tdm.HasDirectionalMovementDisabled()) {
                spdlog::warn("TDM watchdog: orphaned directional-movement disable with no live holder â€” releasing");
                tdm.ReleaseDisableDirectionalMovement();
            }
        }
    }

    // The engine zoom offset ApplyZoom last wrote. Published so the dialogue-
    // exit zoom ease can aim at the value the camera is ACTUALLY going to sit
    // at, rather than re-deriving it from a profile pointer and hoping the two
    // agree. NaN-free by construction (ApplyZoom clamps).
    static float sLastAppliedEngineZoom      = 0.0f;
    static bool  sLastAppliedEngineZoomValid = false;
    float CameraController::GetLastAppliedEngineZoom(bool& a_outValid)
    {
        a_outValid = sLastAppliedEngineZoomValid;
        return sLastAppliedEngineZoom;
    }

    // Last framing CameraController left in the ThirdPersonState fields on a
    // steady gameplay frame. Consumed by HookManager's weapon-draw edge guard.
    // Last world FOV ApplyFOV resolved. Read by the Tween hold, which runs in a
    // camera state where CameraController::Update does not tick at all.
    static float sLastAppliedWorldFov      = 0.0f;
    static bool  sLastAppliedWorldFovValid = false;

    // Live target for the mMountY channel. Zero except during the mount-ENTRY
    // ease, where it holds the engine's pull-back so the channel can carry the
    // camera out to it instead of the engine slamming it there in one frame.
    // Written by the y block in Update, read by the channel group and stepMotion
    // earlier in the same function — so it is one frame stale on the entry
    // frame, which is exactly right: frame one must write 0 (continuity with
    // where the camera already was on foot) and the ease starts from frame two.
    static float sMountYTargetLive = 0.0f;

    // The mount-entry ease's state. File scope because HookManager arms it from
    // the PRE-Update hook — see NotifyMountEntry below for why it cannot be
    // armed from Update itself.
    static float sMountEnterY = 0.0f;
    static int   sMountEnterN = -1;

    void CameraController::NotifyMountEntry(float a_engineY)
    {
        // ARMED FROM BEFORE THE ENGINE COMPOSES, AND IT HAS TO BE.
        //
        // The first attempt armed this from CameraController::Update, which runs
        // AFTER the engine's camera Update. Measured 2026-08-26 19:10:
        //
        //   [MOUNTENTER +1] DIST=430.4  posExp=(+46.8, +0.0, -18.7)
        //   [MOUNTENTER +2] DIST=147.9  dW=307.03
        //
        // Row +1 shows our y=0 write already in place while the probe's
        // one-frame-lagged DIST still reads 430 — i.e. the engine had ALREADY
        // composed one mounted frame at its own -300 before our write landed.
        // So the ease did not replace the slam, it ADDED a 282-unit collapse
        // after it: out to 430, in to 148, then ease back out. Strictly worse
        // than the single jump it was meant to remove.
        //
        // Same class as the draw-edge snap ([[draw-edge-and-tween-fov]]): our
        // writes land after the engine's Update, so an edge frame slips through.
        // The fix is the same one that already exists there — write before
        // CallOriginalUpdate composes.
        if (!std::isfinite(a_engineY) || a_engineY > -50.0f) {
            sMountEnterN = -1;                    // nothing worth easing
            return;
        }
        sMountEnterY = a_engineY;
        sMountEnterN = 0;
        auto& self = GetSingleton();
        self.mMountY.position = 0.0f;
        self.mMountY.velocity = 0.0f;
        spdlog::debug("[MOUNTENTER-Y] easing the {:.1f}u mount pull-back in over the entry "
                     "instead of cutting to it (armed pre-Update)", a_engineY);
    }

    // Set by the dismount abort, cleared when mMountY has finished leaving or
    // the player is back on a horse. While it is up, the y write below is
    // FORCED — see NotifyMountExit in the header for why the resolver cannot
    // be the gate here.
    static bool sMountExitLatched = false;

    void CameraController::NotifyMountExit()
    {
        sMountExitLatched = true;
    }

    static float sLastFramingX = 0.0f, sLastFramingY = 0.0f, sLastFramingZ = 0.0f;
    static float sLastFramingZoomT = 0.0f, sLastFramingZoomC = 0.0f;
    static bool  sLastFramingValid = false;

    bool CameraController::GetLastAppliedFraming(float& a_x, float& a_y, float& a_z,
                                                 float& a_zoomTarget, float& a_zoomCurrent)
    {
        if (!sLastFramingValid) return false;
        a_x = sLastFramingX;  a_y = sLastFramingY;  a_z = sLastFramingZ;
        a_zoomTarget  = sLastFramingZoomT;
        a_zoomCurrent = sLastFramingZoomC;
        return true;
    }

    void CameraController::InvalidateLastAppliedFraming()
    {
        sLastFramingValid = false;
    }


    static CameraProfile* sLastCategoriesProfile = nullptr;
    CameraProfile* CameraController::GetLastCategoriesProfile()
    {
        return sLastCategoriesProfile;
    }

    static int  sLastPowerAttackDir        = -1;
    static bool sLastPowerAttackBaseRouted = false;
    static bool sLastTLPowerAttackBaseRouted = false;
    int  CameraController::GetLastPowerAttackDir()          { return sLastPowerAttackDir; }
    bool CameraController::GetLastPowerAttackBaseRouted()   { return sLastPowerAttackBaseRouted; }
    bool CameraController::GetLastTLPowerAttackBaseRouted() { return sLastTLPowerAttackBaseRouted; }

    static CameraProfile* sLastEnemyOverrideProfile  = nullptr;
    static int            sLastEnemyOverrideEnemyIdx = -1;
    static int            sLastCustomEnemyIdx        = -1;
    static std::string    sLastEnemyOverrideBindingLabel;
    const std::string& CameraController::GetLastEnemyOverrideBindingLabel()
    {
        return sLastEnemyOverrideBindingLabel;
    }
    // Live enemy-override aim-bias edit target for Quick Tune (see header).
    // Point into the active EnemyFieldOverride; null when no enemy override is
    // active this locked frame. Set beside sLastEnemyOverrideProfile below.
    CameraProfile* CameraController::GetLastEnemyOverrideProfile()
    {
        return sLastEnemyOverrideProfile;
    }
    int CameraController::GetLastEnemyOverrideEnemyIdx()
    {
        return sLastEnemyOverrideEnemyIdx;
    }
    int CameraController::GetLastCustomEnemyIdx()
    {
        return sLastCustomEnemyIdx;
    }

    static CameraController::LockedActorIdentity sLockedIdentity{};
    CameraController::LockedActorIdentity CameraController::GetLastLockedActorIdentity()
    {
        return sLockedIdentity;
    }
    // Capture the locked actor's race + NPC-base identity (local form id +
    // plugin filename + display name) for the menu's bind button. Mirrors the
    // weapon-binding form-identity scheme (GetLocalFormID + GetFile(0)).
    static void PublishLockedActorIdentity(RE::Actor* actor)
    {
        if (!actor) return;
        CameraController::LockedActorIdentity id{};
        // TESForm::GetLocalFormID() dereferences a null source file for dynamic
        // (0xFF...) runtime forms â€” e.g. a leveled creature's generated actor
        // base (a wolf's base is 0xFF...) â€” so resolve via GetFile(0) first and
        // capture identity only for plugin-backed forms. A file-less form leaves
        // its plugin empty, so its Bind button is hidden and it never matches.
        auto capture = [](RE::TESForm* f, std::uint32_t& localID, std::string& plugin, std::string& name) {
            localID = 0; plugin.clear(); name.clear();
            if (!f) return;
            auto* file = f->GetFile(0);
            if (!file) return;
            localID = f->GetLocalFormID();
            plugin  = file->fileName;
            const char* n = f->GetName();
            if (n && n[0]) {
                name = n;
            } else if (const char* eid = f->GetFormEditorID(); eid && eid[0]) {
                name = eid;
            }
        };
        capture(actor->GetRace(),      id.raceFormID, id.racePlugin, id.raceName);
        capture(actor->GetActorBase(), id.npcFormID,  id.npcPlugin,  id.npcName);
        // Display name + unique flag drive the single bind button.
        if (const char* n = actor->GetName(); n && n[0]) id.enemyName = n;
        if (auto* ab = actor->GetActorBase()) id.isUnique = ab->IsUnique();
        // Race editor id drives the race-family (substring) bind option, and is
        // available at runtime even for races without a plugin-backed file.
        if (auto* race = actor->GetRace()) {
            if (const char* e = race->GetFormEditorID()) id.raceEditorID = e;
        }
        // Which curated creature-type keywords this actor has -> broad bind
        // options ("All Undead", "All Humanoids", ...).
        for (const auto& kd : kCreatureKeywords) {
            if (auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kd.editorID);
                kw && actor->HasKeyword(kw))
                id.keywords.emplace_back(kd.editorID);
        }
        // Member factions (rank >= 0) -> "by faction" bind options. VisitFactions
        // covers base + runtime faction changes; skip file-less factions (can't be
        // matched load-order-safely) and cap the list so a heavily-factioned NPC
        // doesn't flood the menu.
        actor->VisitFactions([&](RE::TESFaction* fac, std::int8_t rank) {
            if (fac && rank >= 0) {
                std::uint32_t fid; std::string fplug, fname;
                capture(fac, fid, fplug, fname);
                if (!fplug.empty())
                    id.factions.push_back({ fid, fplug, fname.empty() ? std::string("Faction") : fname });
            }
            return id.factions.size() >= 16;   // stop once the list is full
        });
        id.valid = !id.enemyName.empty() || !id.racePlugin.empty() || !id.npcPlugin.empty() ||
                   !id.raceEditorID.empty() || !id.keywords.empty() || !id.factions.empty();
        if (id.valid) sLockedIdentity = std::move(id);
    }

    // Diagnostic counters Quick Tune reads to verify whether Update is
    // actually running during framework-paused operation. Incremented
    // at the top of Update before any early-return; each early-return
    // increments its own bucket so we can tell which gate fired.
    static std::atomic<std::uint32_t> sUpdateCallCount{0};
    static std::atomic<std::uint32_t> sUpdateRetSuspend{0};
    static std::atomic<std::uint32_t> sUpdateRetNotThird{0};
    static std::atomic<std::uint32_t> sUpdateRetNullPlayer{0};
    static std::atomic<std::uint32_t> sUpdateRetNullRoot{0};
    static std::atomic<std::uint32_t> sUpdateRetMenuBlock{0};
    std::uint32_t CameraController::GetUpdateCallCount()        { return sUpdateCallCount.load(); }
    std::uint32_t CameraController::GetUpdateEarlyReturnCount() {
        return sUpdateRetSuspend.load() + sUpdateRetNotThird.load() +
               sUpdateRetNullPlayer.load() + sUpdateRetNullRoot.load() +
               sUpdateRetMenuBlock.load();
    }
    std::uint32_t CameraController::GetUpdateRetSuspend()    { return sUpdateRetSuspend.load(); }
    std::uint32_t CameraController::GetUpdateRetNotThird()   { return sUpdateRetNotThird.load(); }
    std::uint32_t CameraController::GetUpdateRetNullPlayer() { return sUpdateRetNullPlayer.load(); }
    std::uint32_t CameraController::GetUpdateRetNullRoot()   { return sUpdateRetNullRoot.load(); }
    std::uint32_t CameraController::GetUpdateRetMenuBlock()  { return sUpdateRetMenuBlock.load(); }

    // Live internal-state read for Quick Tune diagnostic. These are
    // updated by the spring step inside Update; if they track the
    // slider edits but tps.x doesn't, the writes-to-engine step is
    // being short-circuited downstream.
    float CameraController::GetCurrentSideOffset() const { return currentProfile.sideOffset; }
    float CameraController::GetCurrentHeight()     const { return currentProfile.height; }
    float CameraController::GetCurrentZoom()       const { return currentProfile.zoom; }
    float CameraController::GetTargetSideOffset()  const { return targetProfile.sideOffset; }


    void CameraController::InvalidateProfileReferences()
    {
        // Preset reloads rebuild indoor, binding, location and enemy storage.
        // Quick Tune and pre-update hooks must not use the previous frame's
        // pointers while waiting for the next camera update to publish them.
        sLastResolvedProfile = nullptr;
        sLastCategoriesProfile = nullptr;
        sLastTLProfile = nullptr;
        sLastTLSlotAnchor = nullptr;
        sLastEnemyOverrideProfile = nullptr;
        sLastEnemyOverrideEnemyIdx = -1;
        sLastCustomEnemyIdx = -1;
        sLastEnemyOverrideBindingLabel.clear();
        MenuUI::InvalidateSettingsReferences();
    }

    void CameraController::ResetTransitionState()
    {
        m_vanityTransition.Reset();
        InvalidateProfileReferences();
        lastSelectedProfile = nullptr;
        zoomBaseInitialized = false;
        lastAppliedZoomOffset = 0.0f;
        lastWrittenTargetZoom = 0.0f;
        zoomSettleFrames = 0;
        velSideOffset = velHeight = velZoom = velFOV = velRotation = velPitchOffset = 0.0f;
        mSide = mHeight = mZoom = mFOV = mRotation = mPitch = ChannelMotion{};
    }

    void CameraController::ResetZoomBaseline()
    {
        m_vanityTransition.Reset();
        // Snap every profile channel back to Skyrim-vanilla values AND
        // restore the engine zoom fields to the PRE-PLUGIN value captured
        // in `zoomBase`. Critical distinction from an earlier buggy version
        // of this function: we previously wrote `targetZoomOffset = 0`
        // which maps to the engine's minimum zoom (close-up) â€” well below
        // the default ~1.0 it holds on a fresh save. That produced the
        // "Reset to Vanilla gives close-up camera" bug reported on Apr 22.
        //
        // Correct restore value = `zoomBase`. ApplyZoom captures zoomBase
        // once on first call from whatever targetZoomOffset the engine
        // holds pre-plugin (see CameraController.cpp line 815:
        //   zoomBase = a_tps->targetZoomOffset - priorOffset
        // with priorOffset=0 on the very first call). So zoomBase IS the
        // vanilla zoom position the engine would render at with no plugin
        // influence. Writing it back here, plus resetting the per-profile
        // zoom delta to 0 (VanillaSheathed.zoom == 0 already), leaves
        // final targetZoomOffset == zoomBase on the next ApplyZoom frame.
        //
        // On the off chance zoomBase was never initialized (no 3p frame
        // between plugin load and reset click â€” unlikely but possible),
        // fall back to reading the engine's current value and subtracting
        // any profile offset we've accumulated. That recreates the
        // pre-plugin value from the current state.
        float restoreValue = 0.0f;
        bool  haveRestoreValue = false;
        if (zoomBaseInitialized) {
            restoreValue = zoomBase;
            haveRestoreValue = true;
        } else if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            if (auto* tps = skyrim_cast<RE::ThirdPersonState*>(
                    cam->cameraStates[RE::CameraState::kThirdPerson].get())) {
                restoreValue = tps->targetZoomOffset - lastAppliedZoomOffset;
                haveRestoreValue = true;
            }
        }

        // posOffset channels DO reset to {0,0,0} here â€” CameraController::
        // Update will immediately overwrite posOffsetActual/Expected with
        // the just-reset VanillaSheathed profile values (sideOffset=30,
        // height=-10) on its next call. Zeroing here is belt-and-suspenders
        // against a one-frame glitch during the transition.
        auto resetTps = [&](RE::TESCameraState* state) {
            if (!state) return;
            if (auto* tps = skyrim_cast<RE::ThirdPersonState*>(state)) {
                if (haveRestoreValue) {
                    tps->targetZoomOffset  = restoreValue;
                    tps->currentZoomOffset = restoreValue;
                    tps->savedZoomOffset   = restoreValue;
                }
                tps->posOffsetActual   = { 0.0f, 0.0f, 0.0f };
                tps->posOffsetExpected = { 0.0f, 0.0f, 0.0f };
            }
        };
        if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            resetTps(cam->cameraStates[RE::CameraState::kThirdPerson].get());
            resetTps(cam->cameraStates[RE::CameraState::kMount].get());
            if (cam->currentState) resetTps(cam->currentState.get());
        }

        // Snap the spring-smoothed profile to Vanilla values so the next
        // ApplyFOV/ApplyZoom cycle lands on vanilla immediately, not 1s
        // later via the spring decay.
        currentProfile = CameraProfile::VanillaSheathed();
        targetProfile  = CameraProfile::VanillaSheathed();
        lastSelectedProfile = nullptr;

        // Zero all spring velocities so the snap doesn't bounce back.
        velSideOffset  = 0.0f;
        velHeight      = 0.0f;
        velZoom        = 0.0f;
        velFOV         = 0.0f;
        velRotation    = 0.0f;
        velPitchOffset = 0.0f;

        // Preserve zoomBase (the pre-plugin baseline). Next ApplyZoom:
        //   profileOffset = currentProfile.zoom * 0.01 = 0.0
        //   finalTarget   = zoomBase + 0.0 = zoomBase
        // which matches what we just wrote to tps->targetZoomOffset.
        // Reset lastAppliedZoomOffset = 0 to reflect our new profile.zoom=0,
        // and lastWrittenTargetZoom = restoreValue so the engine-target
        // delta detection (ApplyZoom:845 `inputDelta = engineTarget -
        // lastWrittenTargetZoom`) doesn't misread the reset as user zoom
        // input on the next frame.
        lastAppliedZoomOffset = 0.0f;
        lastWrittenTargetZoom = haveRestoreValue ? restoreValue : 0.0f;
        lastZoom              = 0.0f;

        // Clear any residual dialogue zoom decay.
        dialogueLastY         = 0.0f;
        dialogueExitDecayTime = 0.0f;

        spdlog::debug("CameraController: vanilla snap â€” restored engine zoom to {:.3f} (zoomBase={})",
                     restoreValue, zoomBaseInitialized ? "captured" : "uninitialized");
    }

    void CameraController::Update(RE::PlayerCamera* a_camera)
    {
        sUpdateCallCount.fetch_add(1, std::memory_order_relaxed);

        // Diagnostic short-circuit: the user toggled "Suspend Overrides"
        // in Extras to capture pure-vanilla engine values. Skip every
        // override write so the engine renders untouched.
        if (SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            sUpdateRetSuspend.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Check if a game menu or dialogue is blocking (but not our menu)
        auto* ui = RE::UI::GetSingleton();
        bool menuBlocking = false;
        if (ui) {
            bool journalOpen = ui->IsMenuOpen("Journal Menu");
            if (ui->GameIsPaused() && !journalOpen) menuBlocking = true;
            if (ui->IsMenuOpen("Dialogue Menu")) menuBlocking = true;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            sUpdateRetNullPlayer.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto* root = player->Get3D();
        if (!root) {
            sUpdateRetNullRoot.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        playerPos = root->world.translate;
        UpdateForwardVector(player);

        // Right = normalize(Forward x WorldUp)
        static constexpr RE::NiPoint3 kWorldUp{ 0.0f, 0.0f, 1.0f };
        rightDir = forwardDir.Cross(kWorldUp);
        float len = rightDir.Length();
        if (len > 1e-6f) {
            rightDir /= len;
        }

        // Up = Right x Forward
        upDir = rightDir.Cross(forwardDir);

        // TDM yaw-control SAFETY RELEASE â€” runs BEFORE the third-person gate
        // below. The combat lock takes TDM yaw control on a target switch and
        // releases it inside the lock block, but that block sits past the
        // `currentState != thirdPerson/mount/dragon` early-return. The Vampire
        // Lord / werewolf transform briefly swaps to a non-third-person camera,
        // so if the lock drops during that window the release never runs and
        // TDM is left owning the player's yaw â€” which makes TDM's ProcessInput
        // early-return and stop rotating the body to the input direction (the
        // player can still cast but cannot walk; TDM never auto-releases).
        // Releasing here the instant the lock is gone fixes it. s_lockYawTaken
        // (file scope) tracks that the LOCK path took the yaw, so we only ever
        // release what it owned â€” never the menu-framing controller's yaw claim.
        // This never runs while locked, so the active-lock spring/ease below is
        // untouched. ALSO called from HookedUpdateCameraPost (a universal hook)
        // because this Update only ticks in third person â€” a lock dropped in 1p
        // or mid-transform would otherwise never reach this release.
        ReleaseLeakedLockYaw();

        auto* currentState = a_camera->currentState.get();
        auto* thirdPerson  = a_camera->cameraStates[RE::CameraState::kThirdPerson].get();
        auto* mounted      = a_camera->cameraStates[RE::CameraState::kMount].get();
        auto* dragon       = a_camera->cameraStates[RE::CameraState::kDragon].get();
        // Track POV across frames (even on the non-3p early-return below) so the
        // dialogue branch can detect a mid-dialogue 1p->3p switch: the first
        // frame we reach the 3p dialogue branch after the previous frame was 1p.
        const bool wasFirstLastFrame = m_prevFramePovFirst;
        m_prevFramePovFirst          = a_camera->IsInFirstPerson();
        // ----- COVER THE ENGINE'S TRANSITION ------------------------------
        //
        // Vanilla runs a real camera transition on dismount (kMount ->
        // kPCTransition -> kThirdPerson, ~267 ms measured 2026-08-25 19:38).
        // DDC used to fall straight through the gate below during it, because
        // currentState is the TRANSITION state and not one of the three we
        // recognise — so for a quarter of a second the mod applied NOTHING and
        // the engine composed vanilla framing, and DDC then snapped its own
        // framing in when the transition landed. That snap is the dismount
        // artifact. It is not a zoom and not a pitch step — both of those were
        // wrong diagnoses; the mod simply was not covering the transition.
        //
        // PlayerCameraTransitionState composes by interpolating
        // transitionFrom->GetTranslation() and transitionTo->GetTranslation().
        // So if the DESTINATION state carries our framing on every frame of
        // the lerp, vanilla's own transition delivers the camera to the DDC
        // pose smoothly — we supply the target, the engine supplies the
        // motion. Nothing to animate ourselves.
        //
        // tps must be the DESTINATION, never the transition state: the cast
        // below is a static_cast to ThirdPersonState and a
        // PlayerCameraTransitionState is a different, smaller object.
        auto* transition = a_camera->cameraStates[RE::CameraState::kPCTransition].get();
        const bool inTransition = (currentState == transition);
        bool coveringTransition = false;
        if (inTransition && transition) {
            // Only cover transitions whose destination is the third-person
            // state. Anything heading to 1p / furniture / vanity is not ours.
            auto* tr = static_cast<RE::PlayerCameraTransitionState*>(transition);
            coveringTransition = (tr->transitionTo == thirdPerson) && thirdPerson;
            static bool sCoverLogged = false;
            if (coveringTransition && !sCoverLogged) {
                sCoverLogged = true;
                spdlog::debug("[CAMCOVER] covering the engine's camera transition — "
                             "applying DDC framing to the destination third-person state "
                             "so vanilla's lerp lands on our pose instead of snapping to it");
            }
        }

        if (!coveringTransition &&
            currentState != thirdPerson && currentState != mounted && currentState != dragon) {
            m_vanityTransition.Reset();
            if (originalFOV >= 0.0f) {
                a_camera->worldFOV = originalFOV;
                originalFOV = -1.0f;
                lastFOV = -1.0f;
            }
            // Force re-initialization when returning to third person
            lastSelectedProfile = nullptr;
            zoomBaseInitialized = false;
            lastAppliedZoomOffset = 0.0f;
            lastWrittenTargetZoom = 0.0f;
            returningToThirdPerson = true;
            sUpdateRetNotThird.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto* tps = static_cast<RE::ThirdPersonState*>(coveringTransition ? thirdPerson
                                                                          : currentState);

        // ----- DISMOUNT LOOK-DIRECTION CONTINUITY --------------------------
        //
        // The framing channels ease perfectly across the dismount now, but the
        // engine drops the mount's freeRotation to zero as the state changes —
        // pitch on the first frame, yaw on the next — and that is the residual
        // jerk. Measured 2026-08-25 20:33:
        //
        //   +1  dW= 0.16   rend 0.270 -> 0.135   freeRot.y -0.135 -> 0
        //   +2  dW=16.62   DIST -16.2            freeRot.x +0.125 -> 0
        //   +3  dW=19.51   DIST +13.2   <- and back
        //   +4+ dW~6 steady, DDC's own channels, smooth
        //
        // The engine does NOT compensate: bodyYaw is byte-identical across the
        // edge (3.377 -> 3.377, 0.390 -> 0.390) while WORLD YAW JUMP reads
        // 9.7 deg, so the rendered direction really does step.
        //
        // Absorb the loss into the BODY ANGLES, which is where the composition
        // reads it from anyway — world yaw = bodyYaw + freeRotation.x, rendered
        // pitch = bodyX - freeRotation.y. NEVER write freeRotation itself: the
        // engine reads that field back and integrates it, so an ease there runs
        // away into the pitch clamp ([[dismount-transition-skyward]]).
        //
        // One write per axis, on the frame that axis actually drops, inside a
        // short window after leaving the mount. Not an ease — a continuity
        // correction, so the player's look direction survives the state change
        // and the only motion left is DDC's own transition.
        {
            static float sPrevFreeX = 0.0f, sPrevFreeY = 0.0f;
            static int   sCarryWindow = 0;
            const bool   mountedNow = (currentState == mounted);

            auto wrapPi = [](float a) {
                while (a >  3.14159265f) a -= 6.28318530f;
                while (a < -3.14159265f) a += 6.28318530f;
                return a;
            };

            if (mountedNow) {
                sCarryWindow = 30;            // ~0.5s of cover for the edge
            } else if (sCarryWindow > 0) {
                --sCarryWindow;
                const float prevY = sPrevFreeY;
                const float prevX = wrapPi(sPrevFreeX);
                if (std::abs(prevY) > 0.01f && std::abs(tps->freeRotation.y) < 0.001f) {
                    const float before = player->data.angle.x;
                    player->data.angle.x = std::clamp(before - prevY, -1.4f, 1.4f);
                    spdlog::debug("[MOUNTEXIT-LOOK] pitch continuity: carried freeRot.y={:.3f} "
                                 "into bodyX {:.3f} -> {:.3f}", prevY, before, player->data.angle.x);
                }
                if (std::abs(prevX) > 0.01f && std::abs(wrapPi(tps->freeRotation.x)) < 0.001f) {
                    const float before = player->data.angle.z;
                    player->data.angle.z = wrapPi(before + prevX);
                    spdlog::debug("[MOUNTEXIT-LOOK] yaw continuity: carried freeRot.x={:.3f} "
                                 "into bodyYaw {:.3f} -> {:.3f}", prevX, before, player->data.angle.z);
                }
            }
            sPrevFreeX = tps->freeRotation.x;
            sPrevFreeY = tps->freeRotation.y;
        }

        if (originalFOV < 0.0f) {
            originalFOV = a_camera->worldFOV;
        }

        // Resolve the active camera state and pick the matching profile.
        auto& resolver = StateResolver::GetSingleton();
        resolver.Update(player);
        auto& settings = SettingsManager::GetSingleton();

        // TDM target-lock yaw control. Hard-anchor the camera by driving
        // the player's yaw so the camera (offset by `sideOffset` to the
        // right of the player) ends up on the direct line from camera to
        // target. The closed-form correction is:
        //   playerYaw = atan2(T - P) - arcsin(sideOffset / distance)
        // Derivation: require camera yaw == player yaw, solve
        // sin(yaw - phi) = -side/d. Zoom cancels out. With this correction,
        // camera stays glued relative to player AND target stays centered.
        {
            auto& tdm = TDMIntegration::GetSingleton();
            const bool wantLock = tdm.IsAvailable() && resolver.IsTargetLocked();

            // TDM's UpdateRotation runs in PlayerCharacter::Update â€” earlier
            // in the frame than this hook. By the time we see lock=true,
            // TDM has already snapped data.angle.z to face target (the
            // "body snap" bug). The intercept: track data.z continuously
            // BEFORE TDM acts, and on the lock-acquire edge, take yaw
            // control (TDM's UpdateRotation early-returns when an external
            // plugin owns yaw) and spring-smooth body rotation back to and
            // beyond the pre-lock heading.
            static float s_prevDataZ    = 0.0f;
            static float s_prevFreeRotX = 0.0f;
            static bool  s_prevLocked   = false;
            static RE::ActorHandle s_lockPrevTarget{};

            if (wantLock) {
                const bool acquireEdge = !s_prevLocked;
                // Target SWITCH: the locked actor changed while we stayed locked.
                const RE::ActorHandle curLockTgt = tdm.GetCurrentTarget();
                const bool switchEdge = !acquireEdge && curLockTgt && curLockTgt != s_lockPrevTarget;
                s_lockPrevTarget = curLockTgt;

                // On a target SWITCH we do NOT take body control / glue the
                // camera to the body. The old body-ease eased the body partway
                // over the switch time and then handed the REMAINING rotation
                // back to TDM, which closed it at TDM's own fast speed â€” the
                // "hold for the duration, then snap" the user saw. Instead the
                // HookManager camera-yaw spring + switch latch swings the camera
                // smoothly to the new target over Target Switch Time (the spring
                // targets the enemy's WORLD bearing, so the lag is bounded and
                // self-correcting â€” no spin). Acquire was already spring-driven.
                // Bonus: not grabbing TDM yaw on a switch also removes the
                // yaw-claim release that could leak and freeze movement.
                if (switchEdge) {
                    // Distance-driven switch DURATION. The swing turns at a
                    // constant angular rate (Target Switch Speed), so a wider gap
                    // between the old heading and the new target takes
                    // proportionally longer than a small one â€” "how far apart the
                    // targets are decides how fast/slow the transition is."
                    // Computed once here and shared by the body-ease (edur) and
                    // the framing latch (swDur) below. Seed the swing angle from
                    // the pre-switch heading (s_prevDataZ) so it matches what the
                    // ease actually sweeps.
                    float switchSwingAngle = 0.0f;
                    if (auto tp = curLockTgt.get()) {
                        const auto& tpos = tp->GetPosition();
                        const auto& pp   = player->GetPosition();
                        const float dx = tpos.x - pp.x;
                        const float dy = tpos.y - pp.y;
                        if (dx * dx + dy * dy > 1.0f) {
                            float a = std::atan2(dx, dy) - s_prevDataZ;
                            while (a >  3.14159265f) a -= 6.28318530f;
                            while (a < -3.14159265f) a += 6.28318530f;
                            switchSwingAngle = std::abs(a);
                        }
                    }
                    m_lockSwitchDur = settings.SwitchDurationForAngle(switchSwingAngle);

                    // Arm the framing-spring switch latch (see lockAimOmega) so
                    // the centering + side-scale settle with the main swing.
                    m_lockSwitching = true;
                    m_lockSwitchT   = 0.0f;

                    // Body-ease the switch. Take TDM yaw and drive the BODY from
                    // its current heading onto the new target over the (angle-
                    // scaled) Target Switch Time via the ease block below; while
                    // it runs, HookManager glues the camera to the eased body
                    // (IsLockBodyEasing) so the two swing onto the enemy TOGETHER.
                    // Without this the body was left to TDM â€” it snapped onto the
                    // target in ~0.3s while only the camera eased over Switch Time,
                    // so they decoupled (freeRotation.x diverged ~35deg) and the
                    // body re-settled in a visible twitch at the end ([SWITCHDIAG]
                    // confirmed). The yaw-claim leak that got this removed before
                    // is now covered by the universal-hook watchdog
                    // (HookedUpdateCameraPost -> ReleaseLeakedLockYaw), which frees
                    // s_lockYawTaken on lock-off in ANY camera state. speedMul 0 =
                    // instant: WE supply the eased value each frame via
                    // SetPlayerYaw, so TDM must add no ease of its own. Re-seed on
                    // every switch edge so a second switch mid-ease restarts the
                    // sweep from the live body heading. NOT while sprint-moving:
                    // holding TDM yaw during a sprint makes the character sprint
                    // in place (see the sprint-facing note) â€” let TDM own the body
                    // there and fall back to the camera-only swing.
                    bool sprinting = false;
                    if (auto* as = player->AsActorState()) sprinting = as->IsSprinting();
                    if (!sprinting) {
                        // Switching WHILE ATTACKING/CASTING: the combat face-target
                        // assist already holds the yaw (at its own ease speed), and
                        // its per-frame else-branch RELEASES that yaw the moment the
                        // body-ease takes over (it sees s_lockYawTaken and backs off)
                        // â€” which killed the ease after one frame, so the curve and
                        // duration never applied during an attack. Hand the claim to
                        // the body-ease: drop the assist's hold (clears its flag so
                        // it won't release again) and re-take at INSTANT speed, since
                        // RequestYawControl is a no-op while already owned and would
                        // otherwise leave TDM easing on top of our curve.
                        if (s_combatFaceYawTaken) {
                            tdm.ReleaseYawControl();
                            s_combatFaceYawTaken = false;
                        }
                        if (s_lockYawTaken || tdm.RequestYawControl(0.0f)) {
                            s_lockYawTaken      = true;
                            lockYawSpringActive = true;
                            lockGlueSettling    = false;
                            // Seed from LAST frame's rendered heading (s_prevDataZ),
                            // NOT this frame's data.angle.z. TDM's UpdateRotation
                            // runs earlier in the frame than this hook (see :422),
                            // so on the switch frame TDM has ALREADY rotated the body
                            // one frame's worth toward the new target. Seeding from
                            // the post-TDM value bakes that first-frame step in as an
                            // instant pop before the curve engages. For far-apart
                            // targets one frame is a tiny fraction of the swing (no-
                            // op); for CLOSE-TOGETHER targets the swing is small, so
                            // that step is a large fraction of it -> a visible snap,
                            // intermittently (depends whether TDM's step landed
                            // before our hook this frame). s_prevDataZ is last frame's
                            // final body heading (saved post-write at :714), so the
                            // ease starts where the body visibly was and absorbs
                            // TDM's step into the curved swing. Mirrors the acquire
                            // path's pre-TDM intercept.
                            lockEaseStart       = s_prevDataZ;
                            lockEaseT           = 0.0f;
                        }
                    }
                }

                if (lockYawSpringActive) {
                    // Drive the BODY only while we actually hold TDM yaw. The
                    // timer + termination below run UNCONDITIONALLY so the ease
                    // always self-terminates: if TDM reclaims the yaw mid-ease (it
                    // bails the instant it sees a sprint/rotation), this drive just
                    // stops, but the timer still runs out and clears the flags.
                    // Gating the whole block on HasYawControl (the old way) left
                    // lockYawSpringActive stuck true forever when yaw was lost â€”
                    // [SWITCHDIAG] saw easing=1 frozen with tdmHasYaw=0, the camera
                    // glued for the rest of the lock.
                    if (tdm.HasYawControl()) {
                        if (auto targetHandle = tdm.GetCurrentTarget()) {
                            if (auto targetPtr = targetHandle.get()) {
                                const auto  tp = targetPtr->GetPosition();
                                const auto& pp = player->GetPosition();
                                const float dx = tp.x - pp.x;
                                const float dy = tp.y - pp.y;
                                if (dx * dx + dy * dy > 1.0f) {
                                    const float desired = std::atan2(dx, dy);

                                    // TIME-BASED smoothstep ease from the start
                                    // heading to the LIVE target. HookManager glues
                                    // the camera to this eased body so the two move
                                    // in lockstep (freeRotation.x ~ 0 -> no spin).
                                    float td = desired - lockEaseStart;
                                    while (td >  3.14159265f) td -= 6.28318530f;
                                    while (td < -3.14159265f) td += 6.28318530f;

                                    // SMOOTHSTEP the swing (ease-in / ease-out):
                                    // zero angular velocity at BOTH the start and
                                    // the end of the turn. This replaced a pure
                                    // LINEAR ramp (eased = start + td*lockEaseT),
                                    // which turned on and snapped off instantly â€”
                                    // on short/close switches the distance-driven
                                    // duration floors at 0.08s (a few frames), so
                                    // those two hard velocity edges read as discrete
                                    // steps: the "target switch stutters/isn't
                                    // smooth" the user reported. Smoothstep rounds
                                    // both corners so every switch glides. The
                                    // distance-driven DURATION is untouched (wider
                                    // swings still take proportionally longer â€” the
                                    // rework's intent), only the velocity SHAPE
                                    // within the window changes. The zero end-slope
                                    // is a bonus: the swing hands off to the post-
                                    // ease tracking spring carrying no residual
                                    // velocity, removing the end-of-swing jerk the
                                    // linear ramp left for the spring to absorb.
                                    const float sT =
                                        lockEaseT * lockEaseT * (3.0f - 2.0f * lockEaseT);
                                    float eased = lockEaseStart + td * sT;
                                    while (eased >  3.14159265f) eased -= 6.28318530f;
                                    while (eased < -3.14159265f) eased += 6.28318530f;
                                    tdm.SetPlayerYaw(eased);
                                }
                            }
                        }
                    }

                    // Duration = the distance-driven window computed on the switch
                    // edge (swing angle / Target Switch Speed). Wider swings take
                    // longer; a constant-rate turn. Advance unconditionally so the
                    // ease always terminates even if yaw was never granted.
                    const float edur = std::max(m_lockSwitchDur, 0.0001f);
                    lockEaseT += std::clamp(smoothDt, 0.0001f, 0.05f) / edur;

                    // Terminate on sweep completion OR loss of yaw (TDM reclaimed
                    // it â€” there's nothing left for us to drive, hand off now). Then
                    // DON'T stop gluing yet: enter settle so the camera stays pinned
                    // to the body while TDM finishes rotating it onto the target.
                    if (lockEaseT >= 1.0f || !tdm.HasYawControl()) {
                        if (tdm.HasYawControl()) tdm.ReleaseYawControl();
                        s_lockYawTaken      = false;
                        lockYawSpringActive = false;
                        lockGlueSettling    = true;
                        lockSettleT         = 0.0f;
                    }
                }

                // Post-ease settle. Camera is glued to the body in HookManager
                // for this whole window; here we just decide when TDM has
                // rotated the body close enough to the target to let go (so the
                // residual gap the spring inherits is tiny -> no snap/spin).
                // Times out so a perpetually-moving target can't hold it open.
                if (lockGlueSettling && !lockYawSpringActive) {
                    lockSettleT += std::clamp(smoothDt, 0.0001f, 0.05f);
                    bool settled = lockSettleT >= 0.6f;
                    if (!settled) {
                        if (auto h = tdm.GetCurrentTarget()) {
                            if (auto tp = h.get()) {
                                const auto  tpos = tp->GetPosition();
                                const auto& pp = player->GetPosition();
                                const float dx = tpos.x - pp.x;
                                const float dy = tpos.y - pp.y;
                                if (dx * dx + dy * dy > 1.0f) {
                                    float gap = std::atan2(dx, dy) - player->data.angle.z;
                                    while (gap >  3.14159265f) gap -= 6.28318530f;
                                    while (gap < -3.14159265f) gap += 6.28318530f;
                                    if (std::abs(gap) < 0.06f) settled = true;
                                }
                            }
                        }
                    }
                    if (settled) lockGlueSettling = false;
                }
            } else {
                if (tdm.HasYawControl()) tdm.ReleaseYawControl();
                s_lockYawTaken      = false;
                lockYawSpringActive = false;
                lockGlueSettling    = false;
                s_lockPrevTarget    = {};
                m_lockSwitching     = false;
            }

            // ---- Combat face-target assist ----
            // While target-locked, the body should face the locked enemy during
            // an attack or cast â€” but TDM gives up its target-facing the instant
            // playerFlags.isSprinting is set, and that flag is the sprint BUTTON
            // ("wants to sprint", not "is sprinting"; TDM names it bWantsToSprint
            // :969 and bails on it :1024). Hold sprint and cast Flames at a locked
            // target: Skyrim can't sprint while casting, but the held button keeps
            // the flag true, so TDM never re-faces and the player can spin a full
            // 360 while streaming. So during a locked attack/cast DDC takes TDM
            // yaw and eases the body onto the live target; the moment the action
            // ends it releases, and TDM resumes the sprint and eases the body back
            // to the movement direction on its own. Engage is gated on the action
            // ONLY (not sprint), so the player can still sprint away from a lock.
            {
                constexpr float kYawSpeed = 2.0f;   // TDM eases at PI*kYawSpeed*(1+|delta|) rad/s

                int  atkState  = 0;
                bool sprinting = false;
                if (auto* as = player->AsActorState()) {
                    atkState  = static_cast<int>(as->GetAttackState());
                    sprinting = as->IsSprinting();
                }
                // Melee attack states, matching TDM's own bIsAttacking test
                // (>kNone && <kBowDraw=8) â€” leaves bow aiming to TDM.
                const bool meleeAttacking = atkState > 0 && atkState < 8;
                // Spell casts (charge or stream) â€” from the StateResolver's
                // engine-state signal (same source the Quick Menu header reads),
                // NOT the flaky sprint button flag.
                const bool casting = resolver.IsChargingSpell() || resolver.IsCastingStream();
                const bool combatAction = meleeAttacking || casting;

                // Jittery-enemy tracking smoothing. A custom-bound enemy (ice
                // wraith, horker, ...) can carry a Tracking Smoothing strength;
                // while locked onto it in STEADY state (no attack/cast) and not
                // sprinting, drive a LOW-PASSED bearing so its jerky animation
                // doesn't whip the camera. A combat action uses the RAW bearing
                // (responsive), and sprint hands the body back to TDM. This shares
                // the combat face-assist's single yaw claim + release, so there's
                // no extra watchdog holder. See target-lock-jitter memory.
                float bearing = 0.0f;
                bool  haveT   = false;
                float smTau   = 0.0f;
                if (tdm.IsAvailable() && tdm.IsTargetLocked()) {
                    if (auto h = tdm.GetCurrentTarget()) {
                        if (auto tp = h.get()) {
                            const auto& pp   = player->GetPosition();
                            const auto  tpos = tp->GetPosition();
                            const float dx = tpos.x - pp.x;
                            const float dy = tpos.y - pp.y;
                            if (dx*dx + dy*dy > 1.0f) {
                                float smStr = 0.0f;
                                if (const int ci = settings.FindCustomEnemyForActor(tp.get()); ci >= 0)
                                    smStr = std::clamp(
                                        settings.customEnemyOverrides[static_cast<std::size_t>(ci)].trackingSmoothing,
                                        0.0f, 3.0f);
                                const bool smoothWanted = smStr > 0.001f && !sprinting && !combatAction;
                                if (combatAction || smoothWanted) {
                                    bearing = std::atan2(dx, dy);
                                    smTau   = smoothWanted ? (0.04f + smStr * 0.40f) : 0.0f;  // strength -> LP tau (s)
                                    haveT   = true;
                                }
                            }
                        }
                    }
                }

                const bool wantAssist = haveT && !s_lockYawTaken;
                static float s_smBearing = 0.0f; static bool s_haveSmBear = false;
                if (wantAssist) {
                    float driveBearing = bearing;
                    if (smTau > 0.0f) {
                        // Exponential low-pass of the bearing (wrapped): damp the
                        // enemy's high-frequency jitter while still following its
                        // real motion. WE own the smoothing; TDM eases toward it.
                        float rdt = 1.0f / 60.0f;
                        if (auto* timer = RE::BSTimer::GetSingleton())
                            rdt = std::clamp(timer->realTimeDelta, 1.0f / 240.0f, 0.1f);
                        if (!s_haveSmBear) { s_smBearing = bearing; s_haveSmBear = true; }
                        const float aa = 1.0f - std::exp(-rdt / smTau);
                        float d = bearing - s_smBearing;
                        while (d >  3.14159265f) d -= 6.28318530f;
                        while (d < -3.14159265f) d += 6.28318530f;
                        s_smBearing += d * aa;
                        while (s_smBearing >  3.14159265f) s_smBearing -= 6.28318530f;
                        while (s_smBearing < -3.14159265f) s_smBearing += 6.28318530f;
                        driveBearing = s_smBearing;
                    } else {
                        s_haveSmBear = false;   // re-seed when smoothing next engages
                    }
                    if (!s_combatFaceYawTaken && tdm.RequestYawControl(kYawSpeed))
                        s_combatFaceYawTaken = true;
                    if (s_combatFaceYawTaken)
                        tdm.SetPlayerYaw(driveBearing);    // TDM eases the body onto the target
                } else {
                    // Release the INSTANT the action ends / the enemy is no longer
                    // smoothed / sprint resumes. A held facing that doesn't match
                    // the movement direction makes the character sprint in place;
                    // TDM owns the turn back to the movement line.
                    s_haveSmBear = false;
                    if (s_combatFaceYawTaken) {
                        tdm.ReleaseYawControl();
                        s_combatFaceYawTaken = false;
                    }
                }
            }

            // Track for next frame's edge detection. Saved AFTER our writes
            // so when we re-enter on the next frame and TDM hasn't yet
            // rotated, we can capture this pre-TDM value.
            s_prevDataZ    = player->data.angle.z;
            s_prevFreeRotX = tps->freeRotation.x;
            s_prevLocked   = wantLock;
        }


        // Helper: pick the Blocking profile from (BlockKind, sub-state).
        // Sub-state is already constrained by ResolveBlocking (only None,
        // Sneak, or Sprint reach here for Blocking). Sprint only valid for
        // Shield kind. Ward is opt-in: when the matching wardEnabled flag
        // is off, fall through to the Restoration Concentration magic
        // profile so the Ward entry doesn't conflict with the spell's own.
        auto pickBlockingProfile = [&](BlockKind kind, CameraSubState sub) -> CameraProfile* {
            const bool sneak = (sub == CameraSubState::Sneak);
            switch (kind) {
            case BlockKind::TwoHanded:
                return sneak ? &settings.weaponsBlockingTwoHandedSneak
                             : &settings.weaponsBlockingTwoHanded;
            case BlockKind::Shield:
                if (sub == CameraSubState::Sprint) return &settings.weaponsBlockingShieldSprint;
                return sneak ? &settings.weaponsBlockingShieldSneak
                             : &settings.weaponsBlockingShield;
            case BlockKind::Ward: {
                const bool wardEnabled = sneak
                    ? settings.weaponsBlockingWardSneakEnabled
                    : settings.weaponsBlockingWardEnabled;
                if (wardEnabled) {
                    return sneak ? &settings.weaponsBlockingWardSneak
                                 : &settings.weaponsBlockingWard;
                }
                // Disabled: defer to Restoration Concentration. The ward
                // is a restoration concentration spell, so this picks the
                // closest matching Magic profile.
                return settings.PickMagicProfile(MagicSchool::Restoration,
                                                  CastType::Concentration, sneak);
            }
            case BlockKind::OneHanded:
            case BlockKind::None:
            default:
                return sneak ? &settings.weaponsBlockingOneHandedSneak
                             : &settings.weaponsBlockingOneHanded;
            }
        };
        // Slot anchor for the enemy-override splice. The per-hand magic grid and
        // the per-weapon-type melee grids live OUTSIDE the TLSlot table, so when
        // one of them wins the pick, SlotFromTLProfile(tl) misses and every
        // slot-keyed layer above it (enemy overrides, their aim bias, the Quick
        // Tune apply scopes) silently goes dark. Whenever a picker substitutes an
        // off-table profile it records the plain TL slot it hung off here, and
        // the tlSlot resolve below falls back to it. Enemy overrides therefore
        // outrank both layers â€” the same precedence bindings already have
        // (pickBindingInternal records activeTLBinding before its hand return).
        //
        // tlAnchorFor is the off-table profile the anchor belongs to. The state
        // switch below can PICK more than once per frame (the base state, then
        // a swim / attack / directional-power-attack re-pick that overwrites
        // `tl`), so an anchor recorded by an earlier pick must not be applied to
        // a later, unrelated `tl` â€” that mis-keyed the enemy-override splice
        // onto the base melee slot during directional power attacks. Every use
        // below is gated on the anchor still describing the profile that won.
        CameraProfile* tlSlotAnchor = nullptr;
        CameraProfile* tlAnchorFor  = nullptr;
        // Same map but against the tl* tree.
        auto pickTLBlockingProfile = [&](BlockKind kind, CameraSubState sub) -> CameraProfile* {
            const bool sneak = (sub == CameraSubState::Sneak);
            switch (kind) {
            case BlockKind::TwoHanded:
                return sneak ? &settings.tlWeaponsBlockingTwoHandedSneak
                             : &settings.tlWeaponsBlockingTwoHanded;
            case BlockKind::Shield:
                if (sub == CameraSubState::Sprint) return &settings.tlWeaponsBlockingShieldSprint;
                return sneak ? &settings.tlWeaponsBlockingShieldSneak
                             : &settings.tlWeaponsBlockingShield;
            case BlockKind::Ward: {
                const bool wardEnabled = sneak
                    ? settings.weaponsBlockingWardSneakEnabled
                    : settings.weaponsBlockingWardEnabled;
                if (wardEnabled) {
                    return sneak ? &settings.tlWeaponsBlockingWardSneak
                                 : &settings.tlWeaponsBlockingWard;
                }
                return PickTLMagicAnchored(settings, tlSlotAnchor, tlAnchorFor,
                                           MagicSchool::Restoration,
                                           CastType::Concentration, sneak);
            }
            case BlockKind::OneHanded:
            case BlockKind::None:
            default:
                return sneak ? &settings.tlWeaponsBlockingOneHandedSneak
                             : &settings.tlWeaponsBlockingOneHanded;
            }
        };

        CameraProfile* selected = &settings.sheathed;
        const char*    shoutLogSource = nullptr;

        // Classify the player's equipped melee weapon once; the 5 Melee
        // profile-selection branches below feed it through the per-weapon
        // / weight-class override resolver. Cheap (one pointer chase per
        // hand), and harmless when the resolved state isn't Melee.
        const auto curMeleeWeapon = ClassifyPlayerMeleeWeapon(player);
        // Matched mod-added weapon-type keyword (Custom tab binds) â€” an
        // enabled custom slot outranks the vanilla-type slot. Custom slots
        // carry their own indoor twin (they can't join the variant
        // registry), so the env is resolved here.
        const std::string* curCustomMeleeKw = settings.CurrentCustomMeleeKeyword();
        const char* curCustomMeleeKwC =
            curCustomMeleeKw ? curCustomMeleeKw->c_str() : nullptr;
        const bool curMeleeOvIndoor =
            settings.RuntimeEnv() == SettingsManager::kEnvIndoor;

        // Capture both hands' form identity once for specific-form binding
        // lookups across all weapon/spell/shield categories. Most
        // categories (Melee/Bow/Crossbow/Spell/Staff) check the right
        // hand first and fall back to left for dual-wield setups; Shield
        // is left-hand-only.
        const auto rightItem = ItemBindings::DescribeEquipped(player, false);
        const auto leftItem = ItemBindings::DescribeEquipped(player, true);

        // Tracks the bound weapon matched for the locked state's TL profile,
        // regardless of whether that slot's base profile is enabled. Lets the
        // binding's enemy override win over the category one (specific weapons
        // win over everything). First TL match in the frame wins.
        SettingsManager::WeaponBinding* activeTLBinding     = nullptr;
        int                             activeTLBindingSlot = -1;

        // Look up a specific-form binding profile by category + sub-state
        // index. Returns nullptr when no binding matches or the slot is
        // not enabled â€” caller falls through to the per-state default.
        auto pickBindingInternal = [&](SettingsManager::BindingCategory cat, int subStateIdx,
                                       bool useTL) -> CameraProfile* {
            if (subStateIdx < 0 ||
                subStateIdx >= (int)SettingsManager::kWeaponBindingSubStates) return nullptr;
            // Candidate forms per category: Shield is left-hand-only;
            // everything else tries the RIGHT hand first and falls back to
            // the LEFT whenever the right form has no matching binding â€”
            // not just when the right hand is empty. A left-hand-bound
            // spell (Bind Left Hand) with an unbound weapon in the right
            // hand would otherwise never match. Once a candidate MATCHES a
            // binding, that binding's decision stands (right-hand priority)
            // even if its slot is disabled.
            const ItemBindings::EquippedItem* cands[2] = { &rightItem, &leftItem };
            const int first = cat == SettingsManager::BindingCategory::Shield ? 1 : 0;
            for (int c = first; c < 2; ++c) {
                if (auto* match = ItemBindings::FindBest(settings.weaponBindings, static_cast<int>(cat), false, *cands[c])) {
                    auto& b = *match;
                    // Record the matched binding for enemy-override priority,
                    // even when this slot's base profile is disabled (the enemy
                    // override is independent of the base camera).
                    if (useTL && !activeTLBinding) {
                        activeTLBinding     = &b;
                        activeTLBindingSlot = subStateIdx;
                    }
                    // The TL slot this binding sub-state STANDS IN FOR.
                    //
                    // A binding's profiles live outside the TLSlot table, so
                    // without an anchor `tlSlot` resolves to nothing whenever a
                    // bound weapon is equipped â€” which silently made the whole
                    // player-bound custom-enemy override layer dead for every
                    // bound weapon in every sub-state. The five built-in enemy
                    // categories are unaffected either way: their resolver
                    // returns on the binding branch before it reads the slot.
                    //
                    // Published only alongside a profile we actually RETURN, and
                    // paired with tlAnchorFor, because the caller drops any
                    // anchor whose owner didn't end up as the live TL profile.
                    CameraProfile* bindAnchor = nullptr;
                    if (useTL) {
                        if (b.category == SettingsManager::BindingCategory::Melee &&
                            subStateIdx >= 10 &&
                            subStateIdx < 10 + (int)SettingsManager::kPowerAttackDirectionCount) {
                            // Directional power attacks have no TLSlot; anchor
                            // to the direction profile so the PA-direction
                            // branch of the resolvers matches instead.
                            bindAnchor = &settings.tlWeaponsMeleePowerAttackDir[subStateIdx - 10];
                        } else if (auto sl = SettingsManager::TLSlotForBinding(b, subStateIdx)) {
                            bindAnchor = settings.TLProfileFromSlot(*sl);
                        }
                    }
                    const auto publishBind = [&](CameraProfile* a_picked) {
                        if (useTL && a_picked && bindAnchor) {
                            tlSlotAnchor = bindAnchor;
                            tlAnchorFor  = a_picked;
                        }
                        return a_picked;
                    };
                    // Separate indoor / outdoor sets, exactly like the base
                    // profiles: interiors always use the indoor variant. Indoor
                    // defaults to vanilla (not a copy of outdoor); the Copy
                    // Outdoor/Indoor button syncs them.
                    // Every regular sub-state is ALWAYS ACTIVE the moment the
                    // weapon is bound (seeded to vanilla, flags normalized true
                    // on bind/load â€” the per-slot Enable toggles were removed).
                    // Only the melee DIRECTIONAL power-attack slots (10-14)
                    // remain optional: the Power Attack base (slot 5) blankets
                    // the directions unless one is explicitly overridden.
                    const int  env    = settings.RuntimeEnv();
                    // Per-binding LOCATION layer (2026-08-15): a place that
                    // binds this weapon's sub-state key outranks the
                    // binding's own profile (and its per-hand layer) â€” the
                    // same place-beats-plain precedence the base states use,
                    // scoped inside the binding's decision.
                    if (auto* lp = settings.ActiveLocationBindingCam(
                            SettingsManager::BindingCamLocationKey(b, subStateIdx, useTL)))
                        return publishBind(lp);
                    // Per-hand layer (Spell bindings) â€” its own override
                    // tier above the slot: fires whenever the resolved
                    // hand's flag is on, independent of the slot's base
                    // Enable toggle (mirrors how melee weapon-type
                    // overrides fire independently of the base Customize).
                    if (b.category == SettingsManager::BindingCategory::Spell) {
                        const int h = SettingsManager::ResolveSpellBindingHand(b, subStateIdx);
                        if (h >= 0 && h < (int)SettingsManager::kMagicHandCount) {
                            if (useTL) {
                                if (b.handTlEnabled[h][subStateIdx])
                                    return publishBind(&b.HandTlProfilesFor(env)[h][subStateIdx]);
                            } else if (b.handEnabled[h][subStateIdx]) {
                                return &b.HandProfilesFor(env)[h][subStateIdx];
                            }
                        }
                    }
                    const bool isBase = (subStateIdx == 0) ||
                        (b.category == SettingsManager::BindingCategory::Melee && subStateIdx == 5);
                    if (useTL) {
                        if (isBase || b.tlEnabled[subStateIdx])
                            return publishBind(&b.TlProfilesFor(env)[subStateIdx]);
                    } else {
                        if (isBase || b.enabled[subStateIdx])
                            return &b.ProfilesFor(env)[subStateIdx];
                    }
                    // Matched binding with this slot disabled â€” its decision
                    // stands; don't fall through to the other hand.
                    return nullptr;
                }
            }
            return nullptr;
        };
        auto pickBinding   = [&](SettingsManager::BindingCategory cat, int subStateIdx) {
            return pickBindingInternal(cat, subStateIdx, /*useTL=*/false);
        };
        auto pickBindingTL = [&](SettingsManager::BindingCategory cat, int subStateIdx) {
            return pickBindingInternal(cat, subStateIdx, /*useTL=*/true);
        };

        // Shout binding lookup â€” matches the active MOD-ADDED shout (known
        // shouts route through the regular per-shout override tables) by
        // form identity instead of the equipped hands. Slot 0 = Shouting,
        // 1 = Shouting (Sneaking); both are always active once bound, like
        // the other categories' base slots.
        auto pickShoutBinding = [&](bool useTL) -> CameraProfile* {
            std::uint32_t fid = 0;
            std::string   plg;
            if (!resolver.GetActiveModShout(fid, plg)) return nullptr;
            const int slot = resolver.IsSneaking() ? 1 : 0;
            for (auto& b : settings.weaponBindings) {
                if (b.fpOnly || b.category != SettingsManager::BindingCategory::Shout) continue;
                if (b.formID != fid || b.pluginName != plg) continue;
                if (useTL && !activeTLBinding) {
                    activeTLBinding     = &b;
                    activeTLBindingSlot = slot;
                }
                const int env = settings.RuntimeEnv();
                if (useTL) return &b.TlProfilesFor(env)[slot];
                return &b.ProfilesFor(env)[slot];
            }
            return nullptr;
        };

        // Sub-state index used by WeaponBinding's per-sub-state slots.
        // Mapped per call-site below; 0 = Base, 1 = Sprinting, 2 = Swim,
        // 3 = Attack, 4 = Sneak.
        auto pickMelee = [&](CameraProfile& base, const MeleeWeaponOverrides& ov, int subStateIdx) -> CameraProfile* {
            // 1. Specific-form binding wins when matched and the requested
            //    sub-state's slot is enabled. Routed through pickBinding (same
            //    as the TL path's pickBindingTL) so the indoor/outdoor variant
            //    is selected by the live cell â€” the previous inline lookup
            //    always returned the outdoor profile, so indoor edits in the
            //    Specific Weapons menu were never read by the camera.
            if (auto* p = pickBinding(SettingsManager::BindingCategory::Melee, subStateIdx)) return p;
            // 2. Per-weapon-type override (custom keyword slots outrank the
            //    vanilla type inside the resolver).
            // 3. Falls through to base.
            return const_cast<CameraProfile*>(ResolveMeleeOverride(
                base, ov, curMeleeWeapon, curCustomMeleeKwC, curMeleeOvIndoor));
        };

        // Per-weapon-type override only (no per-form binding slot). Used by
        // the attack-variant profiles (power / sneak-attack / sprint-attack),
        // which aren't part of the 5-slot weapon-binding model but still
        // honor the sword/axe/mace/etc. type overrides.
        auto pickMeleeOv = [&](CameraProfile& base, const MeleeWeaponOverrides& ov) -> CameraProfile* {
            return const_cast<CameraProfile*>(ResolveMeleeOverride(
                base, ov, curMeleeWeapon, curCustomMeleeKwC, curMeleeOvIndoor));
        };

        // Reset the published power-attack direction each resolve; the melee
        // power branch republishes the live value when one is active.
        sLastPowerAttackDir          = -1;
        sLastPowerAttackBaseRouted   = false;
        sLastTLPowerAttackBaseRouted = false;

        using BCat = SettingsManager::BindingCategory;
        switch (resolver.GetState()) {
        case CameraState::Melee:    selected = pickMelee(settings.weaponsMelee, settings.weaponsMeleeOverrides, 0); break;
        case CameraState::Blocking: {
            CameraProfile* p = nullptr;
            // Shield binding overrides only when the player is actively
            // shield-blocking â€” kind == Shield. One-handed / two-handed /
            // ward blocks don't have specific-form bindings (yet).
            if (resolver.GetBlockKind() == BlockKind::Shield) {
                p = pickBinding(BCat::Shield, 0);
            }
            selected = p ? p : pickBlockingProfile(resolver.GetBlockKind(), resolver.GetSubState());
            break;
        }
        case CameraState::Bow: {
            auto* p = pickBinding(BCat::Bow, 0);
            selected = p ? p : &settings.weaponsBow;
            break;
        }
        case CameraState::Crossbow: {
            auto* p = pickBinding(BCat::Crossbow, 0);
            selected = p ? p : &settings.weaponsCrossbow;
            break;
        }
        case CameraState::Staves: {
            auto* p = pickBinding(BCat::Staff, 0);
            selected = p ? p
                : settings.PickStavesProfile(resolver.GetSchool(), resolver.GetCastType(), resolver.IsSneaking());
            break;
        }
        case CameraState::Magic: {
            // Spell binding: when the bound spell is being actively cast
            // (charging a Fire-and-Forget / Ritual or streaming a
            // Concentration), prefer the Casting slot. Falls back to the
            // baseline Magic slot, then to PickMagicProfile's per-school
            // per-cast-type matrix.
            CameraProfile* p = nullptr;
            if (resolver.IsChargingSpell() || resolver.IsCastingStream()) {
                p = pickBinding(BCat::Spell, 3);
            }
            if (!p) p = pickBinding(BCat::Spell, 0);
            selected = p ? p
                : settings.PickMagicProfile(resolver.GetSchool(), resolver.GetCastType(), resolver.IsSneaking());
            break;
        }
        case CameraState::Werewolf:
            // Feeding outranks drawn/sheathed: the feed idle roots the
            // werewolf over the corpse, so none of the movement sub-states
            // below can fire while it holds.
            selected = resolver.IsWerewolfFeeding() ? &settings.transformationsWerewolfFeeding
                     : resolver.IsWeaponDrawn()     ? &settings.transformationsWerewolf
                                                    : &settings.transformationsWerewolfSheathed;
            break;
        case CameraState::VampireLordSheathed:            selected = &settings.vampireLordSheathed; break;
        case CameraState::VampireLordSheathedLevitating:  selected = &settings.vampireLordSheathedLevitating; break;
        case CameraState::VampireLordMelee:       selected = &settings.vampireLordMelee; break;
        case CameraState::VampireLordMagic:       selected = &settings.vampireLordMagic; break;
        case CameraState::VampireLordConcentration: selected = &settings.vampireLordConcentration; break;
        case CameraState::VampireLordFireAndForget: selected = &settings.vampireLordFireAndForget; break;
        case CameraState::Horseback:
            // MOUNTED WEAPON SPLIT. mountsHorsebackMelee / Archery have had
            // storage, TOML keys, TL twins, menu rows and location overrides
            // since they were added â€” and no runtime selection at all, so
            // they were configurable and never applied. This is that missing
            // line. Sheathed reports None and keeps the plain profile;
            // staves / spells report None too (there is no mounted slot for
            // them) rather than being mistaken for melee.
            //
            // Sprint and Swim still override below, which is right: a gallop
            // is a gallop whether or not a bow is out.
            switch (resolver.GetRiderWeapon()) {
            case RiderWeapon::Archery:
                selected = resolver.IsBowZoomed() ? &settings.mountsHorsebackArcheryZoom
                         : resolver.GetSubState() == CameraSubState::Attack ? &settings.mountsHorsebackArcheryDraw
                                                                            : &settings.mountsHorsebackArchery;
                break;
            case RiderWeapon::Melee:
                // Mid-swing splits by side; between swings the side reads None
                // and this is the plain melee profile exactly as before.
                switch (resolver.GetMountAttackSide()) {
                case StateResolver::MountAttackSide::Left:
                    selected = &settings.mountsHorsebackMeleeLeft;  break;
                case StateResolver::MountAttackSide::Right:
                    selected = &settings.mountsHorsebackMeleeRight; break;
                default:
                    selected = &settings.mountsHorsebackMelee;      break;
                }
                break;            default:
                selected = &settings.mountsHorseback;
                break;
            }
            break;
        case CameraState::DragonRiding:
            // Sub-state routing lives INSIDE the case, not in the sub-state
            // override block below, because a dragon's action is read off the
            // MOUNT and has nothing to do with CameraSubState (you cannot
            // sneak, sprint or swim while riding one). Cruising returns the
            // base, so this is the old line whenever nothing else is firing.
            selected = settings.DragonRidingProfileFor(resolver.GetDragonAction());
            break;
        default: break;
        }

        // Sub-state overrides
        const auto sub = resolver.GetSubState();
        if (sub == CameraSubState::Swimming) {
            switch (resolver.GetState()) {
            case CameraState::Sheathed:  selected = &settings.sheathedSwim; break;
            case CameraState::Melee:     selected = pickMelee(settings.weaponsMeleeSwim, settings.weaponsMeleeSwimOverrides, 2); break;
            case CameraState::Bow: {
                auto* p = pickBinding(BCat::Bow, 3);
                selected = p ? p : &settings.weaponsBowSwim;
                break;
            }
            case CameraState::Crossbow: {
                auto* p = pickBinding(BCat::Crossbow, 3);
                selected = p ? p : &settings.weaponsCrossbowSwim;
                break;
            }
            case CameraState::Magic: {
                auto* p = pickBinding(BCat::Spell, 2);
                selected = p ? p : &settings.weaponsMagicSwim;
                break;
            }
            case CameraState::Staves: {
                auto* p = pickBinding(BCat::Staff, 2);
                selected = p ? p : &settings.weaponsStavesSwim;
                break;
            }
            case CameraState::Werewolf:  selected = &settings.transformationsWerewolfSwim; break;
            case CameraState::Horseback: selected = &settings.mountsHorsebackSwim; break;
            default: break;
            }
        } else if (sub == CameraSubState::Attack) {
            switch (resolver.GetState()) {
            case CameraState::Melee: {
                // Attack variant matrix: {sprint|sneak|none} x {power|normal}.
                // Vanilla sprint attacks set the power flag (â†’ SprintPower);
                // MCO/BFCO normal sprint attacks don't (â†’ SprintAttack). You
                // can't sprint and sneak at once, so those are exclusive.
                // Binding sub-state slots: 5=Power Attack, 6=Sprint Normal,
                // 7=Sprint Power, 8=Sneak Normal, 9=Sneak Power,
                // 10-14=directional power attacks (10 + direction index).
                const bool power = resolver.IsPowerAttacking();
                // Latched, not live: the engine clears the sprint flag while a
                // sprint power attack is still swinging, which used to drop us
                // onto the standing power-attack profile mid-action.
                if (resolver.IsAttackSprint()) {
                    selected = power
                        ? pickMelee(settings.weaponsMeleeSprintPowerAttack, settings.weaponsMeleeSprintPowerAttackOverrides, 7)
                        : pickMelee(settings.weaponsMeleeSprintAttack,      settings.weaponsMeleeSprintAttackOverrides,      6);
                } else if (resolver.IsAttackSneak()) {
                    selected = power
                        ? pickMelee(settings.weaponsMeleeSneakPowerAttack,  settings.weaponsMeleeSneakPowerAttackOverrides,  9)
                        : pickMelee(settings.weaponsMeleeSneakAttack,       settings.weaponsMeleeSneakAttackOverrides,       8);
                } else if (power) {
                    // Directional power-attack. Direction captured at the PA
                    // rising edge / combo flip (see StateResolver::PollAttack).
                    // Per-weapon bindings are most specific and apply on their
                    // own, independent of the Melee-tab directional toggle:
                    // directional binding slot first, then the base Power Attack
                    // slot. Only when no binding matches does the toggle decide
                    // between the directional override/base and the plain Power
                    // Attack override/base.
                    const auto dirIdx = static_cast<std::size_t>(resolver.GetPowerAttackDirection());
                    const bool dirValid = dirIdx < SettingsManager::kPowerAttackDirectionCount;
                    CameraProfile* bindDir = dirValid ? pickBinding(BCat::Melee, static_cast<int>(10 + dirIdx)) : nullptr;
                    bool paBaseRouted = false;
                    if (bindDir) {
                        selected = bindDir;                 // bound weapon's directional override
                    } else if (CameraProfile* bindBase = pickBinding(BCat::Melee, 5)) {
                        // Bound weapon with no directional slot for this direction:
                        // its base Power Attack blankets the directional PAs. That
                        // IS the base, so flag it (same as the generic base below)
                        // â€” the UI marks the state "Base" rather than the struck
                        // direction. "Works the same for specific weapons."
                        selected = bindBase;
                        if (dirValid) paBaseRouted = true;
                    } else if (dirValid && settings.weaponsMeleePowerAttackDirEnabled[dirIdx]) {
                        selected = pickMeleeOv(settings.weaponsMeleePowerAttackDir[dirIdx],
                                               settings.weaponsMeleePowerAttackDirOverrides[dirIdx]);
                    } else {
                        selected = pickMeleeOv(settings.weaponsMeleePowerAttack,
                                               settings.weaponsMeleePowerAttackOverrides);
                        // Generic base Power Attack (this direction's override is
                        // off). Flag so the UI marks the state "Base".
                        paBaseRouted = true;
                    }
                    // Publish the live direction (and base-route flag) for the UI.
                    sLastPowerAttackDir        = dirValid ? static_cast<int>(dirIdx) : 0;
                    sLastPowerAttackBaseRouted = paBaseRouted;
                } else {
                    selected = pickMelee(settings.weaponsMeleeAttack, settings.weaponsMeleeAttackOverrides, 3);
                }
                break;
            }
            case CameraState::Bow: {
                // Drawing = slot 2 standing, slot 5 sneak.
                // ZOOMED outranks drawing and takes slots 6 / 7 â€” you can only
                // zoom while drawn, so it is the narrower answer. It REPLACES
                // the engine's Eagle Eye zoom rather than layering on it: DDC
                // writes worldFOV every frame anyway, so whatever FOV the user
                // puts here is the zoom. Above the base FOV it zooms OUT.
                const bool zoom = resolver.IsBowZoomed();
                const bool sneak = resolver.IsSneaking();
                const int idx = zoom ? (sneak ? 7 : 6) : (sneak ? 5 : 2);
                auto* p = pickBinding(BCat::Bow, idx);
                selected = p ? p
                    : zoom  ? (sneak ? &settings.weaponsBowSneakZoom : &settings.weaponsBowZoom)
                            : (sneak ? &settings.weaponsBowSneakDraw : &settings.weaponsBowDraw);
                break;
            }
            case CameraState::Crossbow: {
                const bool zoom = resolver.IsBowZoomed();
                const bool sneak = resolver.IsSneaking();
                const int idx = zoom ? (sneak ? 7 : 6) : (sneak ? 5 : 2);
                auto* p = pickBinding(BCat::Crossbow, idx);
                selected = p ? p
                    : zoom  ? (sneak ? &settings.weaponsCrossbowSneakZoom : &settings.weaponsCrossbowZoom)
                            : (sneak ? &settings.weaponsCrossbowSneakDraw : &settings.weaponsCrossbowDraw);
                break;
            }
            case CameraState::Werewolf:
                if (resolver.IsPowerAttacking()) {
                    selected = resolver.IsAttackSprint()
                        ? &settings.transformationsWerewolfSprintPowerAttack
                        : &settings.transformationsWerewolfPowerAttack;
                } else {
                    selected = &settings.transformationsWerewolfAttack;
                }
                break;
            case CameraState::VampireLordMelee:
                selected = resolver.IsPowerAttacking()
                    ? &settings.vampireLordMeleePowerAttack
                    : &settings.vampireLordMeleeAttack;
                break;
            default: break;
            }
        }

        if (sub == CameraSubState::Sprint) {
            switch (resolver.GetState()) {
            case CameraState::Sheathed:  selected = &settings.sheathedSprint; break;
            case CameraState::Melee:     selected = pickMelee(settings.weaponsMeleeSprint, settings.weaponsMeleeSprintOverrides, 1); break;
            case CameraState::Bow: {
                auto* p = pickBinding(BCat::Bow, 1);
                selected = p ? p : &settings.weaponsBowSprint;
                break;
            }
            case CameraState::Crossbow: {
                auto* p = pickBinding(BCat::Crossbow, 1);
                selected = p ? p : &settings.weaponsCrossbowSprint;
                break;
            }
            case CameraState::Magic: {
                auto* p = pickBinding(BCat::Spell, 1);
                selected = p ? p : &settings.weaponsMagicSprint;
                break;
            }
            case CameraState::Staves: {
                auto* p = pickBinding(BCat::Staff, 1);
                selected = p ? p : &settings.weaponsStavesSprint;
                break;
            }
            case CameraState::Werewolf:  selected = &settings.transformationsWerewolfSprint; break;
            case CameraState::VampireLordSheathed:
            case CameraState::VampireLordSheathedLevitating:
            case CameraState::VampireLordMelee:
            case CameraState::VampireLordMagic:
            case CameraState::VampireLordConcentration:
            case CameraState::VampireLordFireAndForget:
                                         selected = resolver.IsVampireLordLevitating() ? &settings.vampireLordSprintLevitating : &settings.vampireLordSprint; break;
            case CameraState::Horseback: selected = &settings.mountsHorsebackSprint; break;
            case CameraState::Blocking: {
                // Shield-blocking sprint = slot 2.
                if (resolver.GetBlockKind() == BlockKind::Shield) {
                    if (auto* p = pickBinding(BCat::Shield, 2)) selected = p;
                }
                break;
            }
            default: break;
            }
        } else if (sub == CameraSubState::Sneak) {
            switch (resolver.GetState()) {
            case CameraState::Sheathed: selected = &settings.sheathedSneak; break;
            case CameraState::Melee:    selected = pickMelee(settings.weaponsMeleeSneak, settings.weaponsMeleeSneakOverrides, 4); break;
            case CameraState::Bow: {
                auto* p = pickBinding(BCat::Bow, 4);
                selected = p ? p : &settings.weaponsBowSneak;
                break;
            }
            case CameraState::Crossbow: {
                auto* p = pickBinding(BCat::Crossbow, 4);
                selected = p ? p : &settings.weaponsCrossbowSneak;
                break;
            }
            case CameraState::Magic: {
                // Spell sneak = slot 4. Falls through to PickMagicProfile
                // when no binding match (top-level already routed sneak).
                if (auto* p = pickBinding(BCat::Spell, 4)) selected = p;
                break;
            }
            case CameraState::Staves: {
                if (auto* p = pickBinding(BCat::Staff, 3)) selected = p;
                break;
            }
            case CameraState::Blocking: {
                if (resolver.GetBlockKind() == BlockKind::Shield) {
                    if (auto* p = pickBinding(BCat::Shield, 1)) selected = p;
                }
                break;
            }
            default: break;
            }
        } else if (sub == CameraSubState::Shout) {
            // Priority: per-(state, shout) override > per-state base > outer.
            const bool isSneak = resolver.IsSneaking();
            CameraProfile* picked = nullptr;
            const char*    source = "outer-fallback";
            if (auto sIdx = ShoutableIndexFor(resolver.GetState())) {
                // Mod-added shout binding â€” most specific, wins over the
                // per-(state, shout) override path (which can't match a mod
                // shout anyway: the registry never resolves an id for it).
                if (auto* bp = pickShoutBinding(/*useTL=*/false)) {
                    picked = bp;
                    source = "mod-shout-binding";
                }
                if (!picked) if (auto shoutId = resolver.GetActiveShoutId()) {
                    const auto shoutIdx = static_cast<std::size_t>(*shoutId);
                    const bool enabled = isSneak
                        ? settings.shoutOverrideByStateEnabledSneak[*sIdx][shoutIdx]
                        : settings.shoutOverrideByStateEnabled[*sIdx][shoutIdx];
                    if (enabled) {
                        picked = isSneak
                            ? &settings.shoutOverrideByStateSneak[*sIdx][shoutIdx]
                            : &settings.shoutOverrideByState[*sIdx][shoutIdx];
                        source = isSneak ? "per-(state,shout)-sneak" : "per-(state,shout)";
                    }
                }
                if (!picked) {
                    // Always use the shout's own base profile â€” no "unconfigured"
                    // fallback to the parent state. Its slider values (vanilla by
                    // default) ARE the configuration; the state is self-contained.
                    picked = isSneak
                        ? &settings.shoutsBaseByStateSneak[*sIdx]
                        : &settings.shoutsBaseByState[*sIdx];
                    source = isSneak ? "per-state-sneak" : "per-state";
                }
            }
            if (picked) selected = picked;
            shoutLogSource = source;

            // Werewolf doesn't go through the shoutable-state pipeline
            // above (it's not in kShoutableStates) â€” its Shout sub-state
            // is the howl/roar power. Route directly to the Roar profile.
            if (resolver.GetState() == CameraState::Werewolf) {
                selected = &settings.transformationsWerewolfRoar;
            }
        }

        // Snapshot the resolved Categories profile BEFORE the target-lock
        // swap below can replace `selected` with the tl* pointer. This is
        // what the camera would use if not locked â€” published (env-applied)
        // as sLastCategoriesProfile so Quick Tune's Categories box edits an
        // identical target whether or not the player is locked on.
        CameraProfile* catSelected = selected;

        // Target-lock override â€” parallel to the picker above against the
        // tl* tree. When locked, substitute the equivalent tl* profile if
        // the user has tuned it (non-default); otherwise fall through to
        // the regular selection so an un-tuned lock doesn't snap the camera.
        // tl + tlSlot are hoisted so the enemy-override splice (further
        // down) can reach them.
        CameraProfile* tl = nullptr;
        std::optional<SettingsManager::TLSlot> tlSlot;
        tlSlotAnchor   = nullptr;  // set by the pickers when an off-table profile wins
        tlAnchorFor    = nullptr;
        sLastTLProfile = nullptr;  // overwritten inside the locked branch below
        sLastTLSlotAnchor = nullptr;
        // Cleared every frame; republished by the enemy-override splice below
        // only when an override is actually active for the locked target.
        sLastEnemyOverrideProfile  = nullptr;
        sLastEnemyOverrideEnemyIdx = -1;
        sLastCustomEnemyIdx        = -1;
        if (resolver.IsTargetLocked()) {
            const bool isSneak = resolver.IsSneaking();
            // TL parallel of the Categories pickMelee lambda â€” same weapon
            // classification + per-weapon-type override resolver, plus the
            // shared per-form binding (tl slots) above both.
            // A per-weapon-type override profile is off-table, so anchor to the
            // base slot it overrides (see tlSlotAnchor).
            auto resolveMeleeTLOv = [&](CameraProfile& base, const MeleeWeaponOverrides& ov) -> CameraProfile* {
                auto* p = const_cast<CameraProfile*>(ResolveMeleeOverride(
                    base, ov, curMeleeWeapon, curCustomMeleeKwC, curMeleeOvIndoor));
                if (p != &base) { tlSlotAnchor = &base; tlAnchorFor = p; }
                return p;
            };
            auto pickMeleeTL = [&](CameraProfile& base, const MeleeWeaponOverrides& ov,
                                    int subStateIdx) -> CameraProfile* {
                if (auto* p = pickBindingTL(BCat::Melee, subStateIdx)) return p;
                return resolveMeleeTLOv(base, ov);
            };
            // Per-weapon-type override only (no binding slot) â€” TL parallel
            // of pickMeleeOv for the attack-variant profiles.
            auto pickMeleeTLOv = [&](CameraProfile& base, const MeleeWeaponOverrides& ov) -> CameraProfile* {
                return resolveMeleeTLOv(base, ov);
            };
            switch (resolver.GetState()) {
            case CameraState::Sheathed:  tl = &settings.tlSheathed; break;
            case CameraState::Melee:     tl = pickMeleeTL(settings.tlWeaponsMelee, settings.tlWeaponsMeleeOverrides, 0); break;
            case CameraState::Blocking: {
                CameraProfile* p = nullptr;
                if (resolver.GetBlockKind() == BlockKind::Shield) {
                    p = pickBindingTL(BCat::Shield, 0);
                }
                tl = p ? p : pickTLBlockingProfile(resolver.GetBlockKind(), resolver.GetSubState());
                break;
            }
            case CameraState::Bow: {
                auto* p = pickBindingTL(BCat::Bow, 0);
                tl = p ? p : &settings.tlWeaponsBow;
                break;
            }
            case CameraState::Crossbow: {
                auto* p = pickBindingTL(BCat::Crossbow, 0);
                tl = p ? p : &settings.tlWeaponsCrossbow;
                break;
            }
            case CameraState::Magic: {
                CameraProfile* p = nullptr;
                if (resolver.IsChargingSpell() || resolver.IsCastingStream()) {
                    p = pickBindingTL(BCat::Spell, 3);
                }
                if (!p) p = pickBindingTL(BCat::Spell, 0);
                tl = p ? p : PickTLMagicAnchored(settings, tlSlotAnchor, tlAnchorFor,
                                                 resolver.GetSchool(), resolver.GetCastType(), isSneak);
                break;
            }
            case CameraState::Staves: {
                auto* p = pickBindingTL(BCat::Staff, 0);
                tl = p ? p : settings.PickTLStavesProfile(resolver.GetSchool(), resolver.GetCastType(), isSneak);
                break;
            }
            case CameraState::Werewolf:
                // Feeding outranks drawn/sheathed â€” same reasoning as the
                // Categories pick above.
                tl = resolver.IsWerewolfFeeding() ? &settings.tlTransformationsWerewolfFeeding
                   : resolver.IsWeaponDrawn()     ? &settings.tlTransformationsWerewolf
                                                  : &settings.tlTransformationsWerewolfSheathed;
                break;
            case CameraState::VampireLordSheathed:               tl = &settings.tlVampireLordSheathed; break;
            case CameraState::VampireLordSheathedLevitating:     tl = &settings.tlVampireLordSheathedLevitating; break;
            case CameraState::VampireLordMelee:                  tl = &settings.tlVampireLordMelee; break;
            case CameraState::VampireLordMagic:           tl = &settings.tlVampireLordMagic; break;
            case CameraState::VampireLordConcentration:   tl = &settings.tlVampireLordConcentration; break;
            case CameraState::VampireLordFireAndForget:   tl = &settings.tlVampireLordFireAndForget; break;
            case CameraState::Horseback:
                switch (resolver.GetRiderWeapon()) {
                case RiderWeapon::Archery:
                    tl = resolver.IsBowZoomed() ? &settings.tlMountsHorsebackArcheryZoom
                       : sub == CameraSubState::Attack ? &settings.tlMountsHorsebackArcheryDraw
                                                      : &settings.tlMountsHorsebackArchery;
                    break;
                case RiderWeapon::Melee:
                    switch (resolver.GetMountAttackSide()) {
                    case StateResolver::MountAttackSide::Left:
                        tl = &settings.tlMountsHorsebackMeleeLeft;  break;
                    case StateResolver::MountAttackSide::Right:
                        tl = &settings.tlMountsHorsebackMeleeRight; break;
                    default:
                        tl = &settings.tlMountsHorsebackMelee;      break;
                    }
                    break;                default:                 tl = &settings.tlMountsHorseback;      break;
                }
                break;
            // DragonRiding has no case: TDM cannot lock while riding one
            // without the Lock-On Extension Patch, so `tl` stays null and the
            // Categories dragon profile applies unmodified.
            default: break;
            }
            if (sub == CameraSubState::Swimming) {
                switch (resolver.GetState()) {
                case CameraState::Sheathed:  tl = &settings.tlSheathedSwim; break;
                case CameraState::Melee:     tl = pickMeleeTL(settings.tlWeaponsMeleeSwim, settings.tlWeaponsMeleeSwimOverrides, 2); break;
                case CameraState::Bow: {
                    auto* p = pickBindingTL(BCat::Bow, 3);
                    tl = p ? p : &settings.tlWeaponsBowSwim;
                    break;
                }
                case CameraState::Crossbow: {
                    auto* p = pickBindingTL(BCat::Crossbow, 3);
                    tl = p ? p : &settings.tlWeaponsCrossbowSwim;
                    break;
                }
                case CameraState::Magic: {
                    auto* p = pickBindingTL(BCat::Spell, 2);
                    tl = p ? p : &settings.tlWeaponsMagicSwim;
                    break;
                }
                case CameraState::Staves: {
                    auto* p = pickBindingTL(BCat::Staff, 2);
                    tl = p ? p : &settings.tlWeaponsStavesSwim;
                    break;
                }
                case CameraState::Werewolf:  tl = &settings.tlTransformationsWerewolfSwim; break;
                case CameraState::Horseback: tl = &settings.tlMountsHorsebackSwim; break;
                default: break;
                }
            } else if (sub == CameraSubState::Attack) {
                const bool power = resolver.IsPowerAttacking();
                switch (resolver.GetState()) {
                case CameraState::Melee:
                    if (resolver.IsAttackSprint()) {   // latched for the swing
                        tl = power
                            ? pickMeleeTL(settings.tlWeaponsMeleeSprintPowerAttack, settings.tlWeaponsMeleeSprintPowerAttackOverrides, 7)
                            : pickMeleeTL(settings.tlWeaponsMeleeSprintAttack,      settings.tlWeaponsMeleeSprintAttackOverrides,      6);
                    } else if (resolver.IsAttackSneak()) {
                        tl = power
                            ? pickMeleeTL(settings.tlWeaponsMeleeSneakPowerAttack,  settings.tlWeaponsMeleeSneakPowerAttackOverrides,  9)
                            : pickMeleeTL(settings.tlWeaponsMeleeSneakAttack,       settings.tlWeaponsMeleeSneakAttackOverrides,       8);
                    } else if (power) {
                        // Directional power-attack (Target Lock) â€” same priority
                        // as the Categories side: per-weapon binding (directional
                        // slot, then base Power Attack slot) wins independent of
                        // the global directional toggle; otherwise the toggle
                        // decides directional override/base vs plain Power Attack.
                        const auto dirIdx = static_cast<std::size_t>(resolver.GetPowerAttackDirection());
                        const bool dirValid = dirIdx < SettingsManager::kPowerAttackDirectionCount;
                        CameraProfile* bindDir = dirValid ? pickBindingTL(BCat::Melee, static_cast<int>(10 + dirIdx)) : nullptr;
                        if (bindDir) {
                            tl = bindDir;                       // bound weapon's directional override
                        } else if (CameraProfile* bindBase = pickBindingTL(BCat::Melee, 5)) {
                            // Bound weapon, no directional slot for this direction:
                            // its base Power Attack is used. Flag base-routed so the
                            // TL box title reads "... - Power Attack - Base" â€” the
                            // same indication as the generic base below.
                            tl = bindBase;
                            if (dirValid) sLastTLPowerAttackBaseRouted = true;
                        } else if (dirValid && settings.tlWeaponsMeleePowerAttackDirEnabled[dirIdx]) {
                            tl = pickMeleeTLOv(settings.tlWeaponsMeleePowerAttackDir[dirIdx],
                                               settings.tlWeaponsMeleePowerAttackDirOverrides[dirIdx]);
                        } else {
                            tl = pickMeleeTLOv(settings.tlWeaponsMeleePowerAttack,
                                               settings.tlWeaponsMeleePowerAttackOverrides);
                            // Generic base TL Power Attack (this direction's TL
                            // override is off, no binding matched). Flag base-routed
                            // for the TL box title.
                            if (dirValid) sLastTLPowerAttackBaseRouted = true;
                        }
                    } else {
                        tl = pickMeleeTL(settings.tlWeaponsMeleeAttack, settings.tlWeaponsMeleeAttackOverrides, 3);
                    }
                    break;
                case CameraState::Bow: {
                    const bool zoom = resolver.IsBowZoomed();
                    const int idx = zoom ? (isSneak ? 7 : 6) : (isSneak ? 5 : 2);
                    auto* p = pickBindingTL(BCat::Bow, idx);
                    tl = p ? p
                       : zoom ? (isSneak ? &settings.tlWeaponsBowSneakZoom : &settings.tlWeaponsBowZoom)
                              : (isSneak ? &settings.tlWeaponsBowSneakDraw : &settings.tlWeaponsBowDraw);
                    break;
                }
                case CameraState::Crossbow: {
                    const bool zoom = resolver.IsBowZoomed();
                    const int idx = zoom ? (isSneak ? 7 : 6) : (isSneak ? 5 : 2);
                    auto* p = pickBindingTL(BCat::Crossbow, idx);
                    tl = p ? p
                       : zoom ? (isSneak ? &settings.tlWeaponsCrossbowSneakZoom : &settings.tlWeaponsCrossbowZoom)
                              : (isSneak ? &settings.tlWeaponsCrossbowSneakDraw : &settings.tlWeaponsCrossbowDraw);
                    break;
                }
                case CameraState::Werewolf:
                    if (power) {
                        tl = resolver.IsAttackSprint()
                            ? &settings.tlTransformationsWerewolfSprintPowerAttack
                            : &settings.tlTransformationsWerewolfPowerAttack;
                    } else {
                        tl = &settings.tlTransformationsWerewolfAttack;
                    }
                    break;
                case CameraState::VampireLordMelee:
                    tl = power ? &settings.tlVampireLordMeleePowerAttack
                               : &settings.tlVampireLordMeleeAttack;
                    break;
                default: break;
                }
            } else if (sub == CameraSubState::Sprint) {
                switch (resolver.GetState()) {
                case CameraState::Sheathed:  tl = &settings.tlSheathedSprint; break;
                case CameraState::Melee:     tl = pickMeleeTL(settings.tlWeaponsMeleeSprint, settings.tlWeaponsMeleeSprintOverrides, 1); break;
                case CameraState::Bow: {
                    auto* p = pickBindingTL(BCat::Bow, 1);
                    tl = p ? p : &settings.tlWeaponsBowSprint;
                    break;
                }
                case CameraState::Crossbow: {
                    auto* p = pickBindingTL(BCat::Crossbow, 1);
                    tl = p ? p : &settings.tlWeaponsCrossbowSprint;
                    break;
                }
                case CameraState::Magic: {
                    auto* p = pickBindingTL(BCat::Spell, 1);
                    tl = p ? p : &settings.tlWeaponsMagicSprint;
                    break;
                }
                case CameraState::Staves: {
                    auto* p = pickBindingTL(BCat::Staff, 1);
                    tl = p ? p : &settings.tlWeaponsStavesSprint;
                    break;
                }
                case CameraState::Werewolf:  tl = &settings.tlTransformationsWerewolfSprint; break;
                case CameraState::VampireLordSheathed:
                case CameraState::VampireLordSheathedLevitating:
                case CameraState::VampireLordMelee:
                case CameraState::VampireLordMagic:
                case CameraState::VampireLordConcentration:
                case CameraState::VampireLordFireAndForget:  tl = resolver.IsVampireLordLevitating() ? &settings.tlVampireLordSprintLevitating : &settings.tlVampireLordSprint; break;
                case CameraState::Horseback: tl = &settings.tlMountsHorsebackSprint; break;
                case CameraState::Blocking: {
                    if (resolver.GetBlockKind() == BlockKind::Shield) {
                        if (auto* p = pickBindingTL(BCat::Shield, 2)) tl = p;
                    }
                    break;
                }
                default: break;
                }
            } else if (sub == CameraSubState::Sneak) {
                switch (resolver.GetState()) {
                case CameraState::Sheathed: tl = &settings.tlSheathedSneak; break;
                case CameraState::Melee:    tl = pickMeleeTL(settings.tlWeaponsMeleeSneak, settings.tlWeaponsMeleeSneakOverrides, 4); break;
                case CameraState::Bow: {
                    auto* p = pickBindingTL(BCat::Bow, 4);
                    tl = p ? p : &settings.tlWeaponsBowSneak;
                    break;
                }
                case CameraState::Crossbow: {
                    auto* p = pickBindingTL(BCat::Crossbow, 4);
                    tl = p ? p : &settings.tlWeaponsCrossbowSneak;
                    break;
                }
                case CameraState::Magic: {
                    if (auto* p = pickBindingTL(BCat::Spell, 4)) tl = p;
                    break;
                }
                case CameraState::Staves: {
                    if (auto* p = pickBindingTL(BCat::Staff, 3)) tl = p;
                    break;
                }
                case CameraState::Blocking: {
                    if (resolver.GetBlockKind() == BlockKind::Shield) {
                        if (auto* p = pickBindingTL(BCat::Shield, 1)) tl = p;
                    }
                    break;
                }
                default: break;
                }
            } else if (sub == CameraSubState::Shout) {
                if (auto sIdx = ShoutableIndexFor(resolver.GetState())) {
                    // Mod-added shout binding â€” parallel to the Categories
                    // side; also records activeTLBinding so the binding's
                    // enemy overrides can splice on top.
                    CameraProfile* shoutTl = pickShoutBinding(/*useTL=*/true);
                    if (!shoutTl) if (auto shoutId = resolver.GetActiveShoutId()) {
                        const auto shoutIdx = static_cast<std::size_t>(*shoutId);
                        const bool enabled = isSneak
                            ? settings.tlShoutOverrideByStateEnabledSneak[*sIdx][shoutIdx]
                            : settings.tlShoutOverrideByStateEnabled[*sIdx][shoutIdx];
                        if (enabled) {
                            shoutTl = isSneak
                                ? &settings.tlShoutOverrideByStateSneak[*sIdx][shoutIdx]
                                : &settings.tlShoutOverrideByState[*sIdx][shoutIdx];
                            // A per-shout override is OFF the TLSlot table, so
                            // without an anchor SlotFromTLProfile(tl) misses
                            // and ResolveEnemyOverride returns nothing: every
                            // enabled per-shout override silently lost the
                            // state's shout enemy overrides (the popup on the
                            // Target Lock shout rows keys off the state's
                            // shout Base slot). Anchor to that Base slot, the
                            // same way the per-hand magic and per-weapon-type
                            // melee grids anchor to the slot they override.
                            tlSlotAnchor = isSneak
                                ? &settings.tlShoutsBaseByStateSneak[*sIdx]
                                : &settings.tlShoutsBaseByState[*sIdx];
                            tlAnchorFor  = shoutTl;
                        }
                    }
                    // No enabled specific-shout override -> always use the shout's
                    // own TL base (self-contained; no fallback to the parent-state
                    // TL when it's still at vanilla defaults).
                    if (!shoutTl) {
                        shoutTl = isSneak
                            ? &settings.tlShoutsBaseByStateSneak[*sIdx]
                            : &settings.tlShoutsBaseByState[*sIdx];
                    }
                    if (shoutTl) tl = shoutTl;
                }
                // Werewolf Roar TL parallel â€” same routing reasoning as
                // the Categories side; werewolves aren't in the shoutable
                // states pipeline so handle them separately.
                if (resolver.GetState() == CameraState::Werewolf) {
                    tl = &settings.tlTransformationsWerewolfRoar;
                }
            }
            // Drop an anchor recorded by an earlier pick that a later one
            // overwrote â€” it describes a profile that is no longer live.
            if (tlAnchorFor != tl) { tlSlotAnchor = nullptr; tlAnchorFor = nullptr; }
            tlSlot = settings.SlotFromTLProfile(tl);
            // Per-hand magic / per-weapon-type melee profiles aren't in the
            // TLSlot table. Key the enemy-override layer off the slot they
            // override so a customized hand or weapon type doesn't disable it.
            if (!tlSlot && tlSlotAnchor) tlSlot = settings.SlotFromTLProfile(tlSlotAnchor);
            // Always use the resolved TL profile when locked â€” including when
            // it's still at its defaults. The user wants an unconfigured lock
            // to apply the TL profile's own default settings, NOT fall back to
            // the Categories profile for the state. (tlTuned kept for logging.)
            const bool tlTuned = (tl && *tl != CameraProfile{});
            if (tl) selected = tl;
            // Publish for Quick Tune so its TL slider edits land on the
            // exact pointer the picker chose â€” base, per-weapon-type
            // override, or specific-weapon binding. Apply ResolveByEnv so it's
            // the INDOOR variant when indoors (TL slots are env-eligible): the
            // camera swaps `selected` to that indoor twin at the env step below,
            // so publishing the raw outdoor pointer made the Quick Tune TL box
            // edit the outdoor profile while the camera read the indoor one
            // ("indoor target lock does nothing"). ResolveByEnv is identity for
            // non-eligible TL pointers (per-weapon overrides), so it's safe.
            sLastTLProfile = settings.ResolveByEnv(tl);
            // Publish the anchor too: Quick Tune's apply scopes resolve the
            // edited profile back to a TLSlot, which an off-table per-hand /
            // per-weapon-type pointer can't do on its own.
            sLastTLSlotAnchor = tlSlotAnchor ? settings.ResolveByEnv(tlSlotAnchor) : nullptr;

            // Diagnostic: when locked + attacking, log whether the TL slot
            // was selected or whether we fell back to the Categories tree
            // (because the TL profile equals the default-constructed
            // CameraProfile{}). This is the exact "TL slider didn't apply,
            // it's using my Categories values" diagnostic.
            static bool sLockAttackLogged = false;
            const bool  isLockAttack = (sub == CameraSubState::Attack);
            if (isLockAttack && !sLockAttackLogged) {
                sLockAttackLogged = true;
                if (tl) {
                    spdlog::debug("[TLATTACK] state={} tl_tuned={} tl=(side={:.1f} h={:.1f} fov={:.1f} zoom={:.1f}) cat=(side={:.1f} h={:.1f} fov={:.1f} zoom={:.1f})",
                        static_cast<int>(resolver.GetState()),
                        tlTuned ? 1 : 0,
                        tl->sideOffset, tl->height, tl->fov, tl->zoom,
                        selected->sideOffset, selected->height, selected->fov, selected->zoom);
                } else {
                    spdlog::debug("[TLATTACK] state={} tl=null (no TL slot mapped for this state)",
                        static_cast<int>(resolver.GetState()));
                }
            }
            if (!isLockAttack) sLockAttackLogged = false;
        }

        // One-shot log on shout entry â€” shows profile source + target so we
        // can verify the same shout from different base states resolves to
        // the same target.
        {
            static bool wasShout = false;
            const bool  isShout  = (sub == CameraSubState::Shout);
            if (isShout && !wasShout) {
                const auto shoutId = resolver.GetActiveShoutId();
                spdlog::debug("ShoutEntry: baseState={} isSneak={} shoutId={} source={} tgt side={:.1f} h={:.1f} zoom={:.1f} fov={:.1f} rot={:.1f} pitch={:.1f}",
                             static_cast<int>(resolver.GetState()), resolver.IsSneaking() ? 1 : 0,
                             shoutId ? static_cast<int>(*shoutId) : -1,
                             shoutLogSource ? shoutLogSource : "(none)",
                             selected->sideOffset, selected->height, selected->zoom,
                             selected->fov, selected->rotation, selected->pitchOffset);
            }
            wasShout = isShout;
        }

        // (Shouts-menu live preview removed â€” it re-engaged the shout camera
        // path on every menu selection and jumped even with identical values.
        // Shout cameras are tuned by opening the menu while actually shouting.)

        // Dialogue override â€” wins over every other picker. The camera
        // profile applied while the Dialogue Menu is open is always
        // dialogueProfile, regardless of underlying state. FOV applies
        // via the normal ApplyFOV path; position/rotation overrides
        // happen in HookedGetTranslation/HookedGetRotation since
        // posOffsetActual writes don't survive dialogue's engine path.
        //
        // Previously this gate also required `!playerMoving`. The intent
        // was to defer the override on dialogue entry mid-run. The
        // side effect was severe: when the player STOPPED during open
        // dialogue, dialogueActiveForProfile flipped on, posOffsetExpected.y
        // snapped from 0 to the dialogue zoom-back value, the cameraRoot
        // position jumped backward, and atan2 to the NPC face recomputed
        // a different desiredYaw â€” so the face-lock visibly "reset its
        // transition" on every stop. Holding the dialogue position the
        // whole time eliminates the snap; the spring system handles
        // entry transitions cleanly without needing a movement gate.
        bool dialogueActiveForProfile = false;
        if (settings.dialogueEnabled && ui && ui->IsMenuOpen("Dialogue Menu")) {
            // Phase 2: prefer the resolved bucket preset (DialogueLookPicker
            // sets activeDialogueLook* on dialogue open / cycle). Fall back
            // to the legacy single dialogueProfile if no bucket is active
            // (empty buckets case, or pre-Phase-1 configs).
            selected = settings.ResolveDialogueProfile(false);
            dialogueActiveForProfile = true;
        }

        // Edge-detect dialogue rising. Starts the lockstep entry
        // window, scaled by user sliders to match the face-lock
        // cubic blend duration (`HookManager` divides 0.22s by
        // mulPitch/mulRotation). At default mul=1.0 the window is
        // 0.22s; at lowest mul=0.05 it stretches to 4.4s. Using
        // the slower of the two sliders so lockstep covers BOTH
        // pitch and yaw blends until both complete.
        // The dialogue position pipeline (posOffset Y/Z entry ramp + the channel
        // blend below) is the composed shoulder offset, driven by the Position
        // slider (Zoom mirrors it). Face-lock yaw/pitch run on their own windows.
        const float kDialogueEntryLockstepDuration =
            SettingsManager::DialogueBlendDuration((std::max)(0.05f, settings.dialogueMulPosition));
        const bool dlgEdge     = dialogueActiveForProfile && !m_prevDialogueActiveForLockstep;
        const bool dlgExitEdge = !dialogueActiveForProfile && m_prevDialogueActiveForLockstep;
        // A 3p dialogue branch reached for the first time right after the
        // previous frame was 1p == the user switched 1p->3p mid-dialogue
        // (dialogue can't run this branch in 1p â€” Update early-returns). The
        // engine's posOffset.y / zoom baseline is transient on this frame, so
        // the open-edge capture below pulls the cached steady gameplay-3p
        // baseline instead, keeping the dialogue EXIT blend from snapping.
        const bool dlgPovSwitchEntry = dlgEdge && wasFirstLastFrame;
        // Mid-dialogue preset switch (e.g., picker cycle to next look).
        // Triggers the shared blend with the user's slider-driven duration
        // so cycling presets feels like the initial entry transition. Spring
        // alone (no blend) ignores all dialogue mul sliders except pitch/
        // rotation, so without this the position/zoom/fov sliders had no
        // effect on mid-dialogue retargets â€” user reported 2026-05-24.
        const bool dlgPresetSwitch = dialogueActiveForProfile
                                   && !dlgEdge
                                   && lastSelectedProfile
                                   && lastSelectedProfile != selected;
        if (dlgEdge) {
            m_dialogueEntryLockstepActive = true;
            m_dialogueEntryLockstepT      = 0.0f;
            // Start every conversation square behind the player. The reverse
            // blend is only stepped while a conversation is open, so without
            // this it would still be holding whatever the last one ended on
            // and the new one would open with the camera already swung round.
            m_dlgReverseBlend     = 0.0f;
        }
        if (!dialogueActiveForProfile) {
            m_dialogueEntryLockstepActive = false;
        }
        m_prevDialogueActiveForLockstep = dialogueActiveForProfile;

        // One-shot edge capture for diagnostic log emission later in
        // the dialogue branch (where tps is in scope). Records the
        // pre-stepMotion state; post-stepMotion fields are filled in
        // after the channel integration runs below.
        if (dlgEdge || dlgExitEdge) {
            m_diagDlgEdge.pending          = true;
            m_diagDlgEdge.isEntry          = dlgEdge;
            m_diagDlgEdge.sidePosBefore    = mSide.position;
            m_diagDlgEdge.heightPosBefore  = mHeight.position;
            m_diagDlgEdge.zoomPosBefore    = mZoom.position;
            m_diagDlgEdge.zoomTargetBefore = mZoom.target;
            m_diagDlgEdge.fovPosBefore     = mFOV.position;
            m_diagDlgEdge.pitchPosBefore   = mPitch.position;
            m_diagDlgEdge.lockstepDuration = kDialogueEntryLockstepDuration;
            if (dlgEdge) {
                m_diagDlgSampleActive  = true;
                m_diagDlgSampleIdx     = 0;
                m_diagDlgSampleElapsed = 0.0f;
                m_diagDlgPerFrameActive  = true;
                m_diagDlgPerFrameCount   = 0;
                m_diagDlgPerFrameElapsed = 0.0f;
            }
        }
        if (!dialogueActiveForProfile) {
            m_diagDlgSampleActive = false;
        }


        // --- EMA-smoothed delta time (used by the spring step below) ---
        {
            int64_t now, freq;
            ::QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now));
            if (perfFreqInv == 0.0) {
                ::QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));
                perfFreqInv = 1.0 / static_cast<double>(freq);
                lastPerfCount = now;
            }
            float rawDt = static_cast<float>(
                static_cast<double>(now - lastPerfCount) * perfFreqInv);
            lastPerfCount = now;
            rawDt = std::clamp(rawDt, 0.0001f, 0.1f);
            float alpha = std::clamp(rawDt * 12.0f, 0.3f, 0.9f);
            smoothDt = smoothDt + alpha * (rawDt - smoothDt);
        }

        // --- Critical-damped spring transition ---
        // First-run / reset: snap current to selected, zero velocities so
        // we don't slowly ramp up from default values when the plugin loads
        // (or when returning from 1p â€” keeping the snap instant means only
        // one frame of sideOffset change instead of a multi-frame ramp).
        // Paragliding (Cinematic Effects; mod support) â€” the TOP framing
        // override while a glide is live. Placed after the whole selection
        // chain (states, shouts, target lock) so nothing re-picks over it,
        // and the enemy-override splice below stands down for the same
        // reason. The profiles are off every env/location table, so
        // ResolveByEnv is identity for them and the ordinary channel springs
        // carry the transition in and out like any state change.
        //
        // Two profiles on the lock axis, following the TL tree's own
        // convention: the scenic glide framing normally, and â€” once the user
        // has TUNED it (non-default, same test the tl* swap uses) â€” the
        // glide's Target Lock framing while locked, so a wide beautiful view
        // can zoom in on the thing you are about to drop on. An untouched TL
        // profile falls through to the scenic one instead of snapping to
        // defaults.
        //
        // Both are also PUBLISHED for Quick Tune: catSelected so the QT
        // Categories box edits the glide framing mid-glide, and
        // sLastTLProfile so the QT Target Lock box edits the lock variant
        // while locked (published even un-tuned â€” editing it in QT is
        // exactly how it becomes tuned).
        const bool paragliding =
            settings.paraglideEnabled && resolver.IsParagliding();
        if (paragliding) {
            // Tuned = any FRAMING field moved. Deliberately not the whole
            // struct: the entry carries a Transitions override too, and
            // enabling that alone must not engage a default-framed lock â€”
            // transitions say how to ARRIVE, not where.
            // Read the LIVE env variant (2026-09-05): an indoor-only TL tune
            // counts indoors, the way every other entry's variants do.
            const CameraProfile* tv = settings.ResolveByEnv(&settings.paraglideTLProfile);
            const auto&         t = tv ? *tv : settings.paraglideTLProfile;
            const CameraProfile d{};
            const bool tlTuned =
                t.sideOffset != d.sideOffset || t.height != d.height ||
                t.zoom != d.zoom || t.fov != d.fov ||
                t.rotation != d.rotation || t.pitchOffset != d.pitchOffset;
            selected = (resolver.IsTargetLocked() && tlTuned)
                           ? &settings.paraglideTLProfile
                           : &settings.paraglideProfile;
            catSelected = &settings.paraglideProfile;
            if (resolver.IsTargetLocked()) {
                sLastTLProfile = settings.ResolveByEnv(&settings.paraglideTLProfile);
            }
        }

        // [CLIPCAM] Animation-camera override — an entry bound to the exact
        // clip the player's behavior graph is playing right now. Placed with
        // paraglide at the top of the selection chain: a per-animation
        // framing is the most specific statement the user can make, so
        // nothing re-picks over it. The uid is re-resolved to an entry EVERY
        // frame (the vector reallocates on menu edits — never cache the
        // profile pointer), and the env split is the entry's own explicit
        // twin, chosen here so ResolveByEnv below (which passes unknown
        // pointers through) has nothing left to do.
        bool animCamActive = false;
        {
            const std::uint32_t animUid =
                AnimationCameraController::GetSingleton().ActiveEntryUid();
            if (auto* entry = settings.FindAnimationCamera(animUid)) {
                const int animEnv = settings.RuntimeEnv();
                const std::string animLocKey = "animcam." + std::to_string(entry->uid);
                selected = &entry->ProfileFor(animEnv);
                catSelected = settings.ActiveLocationBindingCam(animLocKey);
                if (!catSelected) catSelected = selected;
                if (resolver.IsTargetLocked()) {
                    sLastTLProfile = settings.ActiveLocationBindingCam(animLocKey + "|tl");
                    if (!sLastTLProfile) sLastTLProfile = &entry->TlProfileFor(animEnv);
                    sLastTLSlotAnchor = nullptr;
                    // Locked: tuned TL variant beats the base (the paraglide
                    // tlTuned rule), and a place's TL binding beats both;
                    // the plain place binding is the fallback.
                    if (entry->TlTuned(animEnv))
                        selected = &entry->TlProfileFor(animEnv);
                    if (auto* lp = settings.ActiveLocationBindingCam(animLocKey + "|tl"))
                        selected = lp;
                    else if (auto* lp2 = settings.ActiveLocationBindingCam(animLocKey))
                        selected = lp2;
                } else if (auto* lp = settings.ActiveLocationBindingCam(animLocKey)) {
                    selected = lp;
                }
                animCamActive = true;
            }
        }

        // Indoor / Outdoor environment substitution. Routes the resolved
        // `selected` pointer through ResolveByEnv: when the player is in an
        // interior cell (settings.indoorMode set by HookManager from the
        // parent cell), substitute the indoor variant of `selected` if one
        // exists in the indoor-eligible map. The spring system then chases
        // the indoor target, smoothly transitioning when the cell changes.
        // For non-eligible profiles (target-lock, dialogue, etc.) ResolveByEnv
        // returns `selected` unchanged.
        selected = settings.ResolveByEnv(selected);

        // Vanity replaces only the idle composition. Staying in ThirdPersonState
        // preserves the view direction, engine collision and every normal framing
        // channel; zero Rotation has exactly the same meaning as Sheathed.
        CameraProfile* const gameplaySelected = selected;
        const bool vanityActiveForProfile = VanityCamera::IsActive() &&
            !dialogueActiveForProfile && !animCamActive &&
            (currentState == thirdPerson || coveringTransition);
        if (vanityActiveForProfile) selected = &settings.vanityCamera;

        // Shout Lag hand-off: while the Shout sub-state owns the camera,
        // publish the RESOLVED shout entry's Lag (CameraProfile::shoutLag)
        // to the resolver â€” base / per-shout override / mod-shout binding /
        // TL variant / env variant, whichever pointer won the pick above,
        // carries its own value. PollShoutLinger adds it to the exit tail
        // so the switch back to sheathed/unsheathed waits that long.
        if (sub == CameraSubState::Shout && selected)
            resolver.SetActiveShoutLag(selected->shoutLag);

        // TEMPORARY [DLGX-EXIT] â€” "dialogue keeps leaking into third person
        // settings when exiting" (user reports since 2026-08-15) â€” STILL
        // OPEN. What the probe has ESTABLISHED so far: resolution and the
        // side/height/zoom/fov springs land on the correct 3p profile on
        // every captured exit (round 1, 10:42/10:49). The 11:43 round-2
        // capture showed the ENGINE fields diverging to the adaptive-
        // collision targets â€” but adaptive was only enabled for that
        // session's phase-1 test; the user had it DISABLED for weeks, so
        // that divergence CANNOT be the historical leak (2026-08-16
        // correction; adaptive collision has since been REMOVED entirely,
        // so it is off the suspect list for good). Eight samples over ~4s:
        // springs incl. rot/pitch, the engine's persistent pose fields
        // (freeRotation, posOffsets, engine zoom, worldFOV), the dialogue
        // speaker gate (a stale MenuTopicManager speaker keeps face-lock +
        // look-lock alive post-close), and the look-handler enable bit.
        // Strip only when a leak repro's log names the carrier.
        {
            static float sDlgExitTailT = -1.0f;
            static float sDlgExitNext  = 0.0f;
            static int   sDlgExitN     = 0;
            if (dlgExitEdge) { sDlgExitTailT = 0.0f; sDlgExitNext = 0.0f; sDlgExitN = 0; }
            if (sDlgExitTailT >= 0.0f && selected) {
                sDlgExitTailT += smoothDt;
                if (sDlgExitTailT >= sDlgExitNext && sDlgExitN < 8) {
                    ++sDlgExitN;
                    sDlgExitNext += 0.5f;
                    bool spkSet = false, lastSpkSet = false, goodbye = false;
                    if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
                        spkSet     = static_cast<bool>(mtm->speaker.get());
                        lastSpkSet = static_cast<bool>(mtm->lastSpeaker.get());
                        goodbye    = mtm->forceGoodbye;
                    }
                    bool lookEn = true;
                    if (auto* pc = RE::PlayerControls::GetSingleton(); pc && pc->lookHandler)
                        lookEn = pc->lookHandler->inputEventHandlingEnabled;
                    auto* pcam = RE::PlayerCamera::GetSingleton();
                    spdlog::debug("[DLGX-EXIT] t={:.2f} sel(side {:.1f} h {:.1f} zoom {:.1f} fov {:.1f} rot {:.2f} pitch {:.2f}) "
                                 "springs side {:.1f}->{:.1f} h {:.1f}->{:.1f} zoom {:.1f}->{:.1f} fov {:.1f}->{:.1f} "
                                 "rot {:.1f}->{:.1f} pit {:.1f}->{:.1f}",
                                 sDlgExitTailT,
                                 selected->sideOffset, selected->height, selected->zoom,
                                 selected->fov, selected->rotation, selected->pitchOffset,
                                 mSide.position, mSide.target,
                                 mHeight.position, mHeight.target,
                                 mZoom.position, mZoom.target,
                                 mFOV.position, mFOV.target,
                                 mRotation.position, mRotation.target,
                                 mPitch.position, mPitch.target);
                    spdlog::debug("[DLGX-EXIT]        eng frX={:+.3f} frY={:+.3f} frEn={} posAct=({:.1f},{:.1f},{:.1f}) "
                                 "posExp=({:.1f},{:.1f},{:.1f}) engZoom={:.3f}/{:.3f} wFOV={:.1f} "
                                 "| spk={} lastSpk={} goodbye={} dlgAct={} lookEn={} pace={:.2f}",
                                 tps->freeRotation.x, tps->freeRotation.y,
                                 tps->freeRotationEnabled ? 1 : 0,
                                 tps->posOffsetActual.x, tps->posOffsetActual.y, tps->posOffsetActual.z,
                                 tps->posOffsetExpected.x, tps->posOffsetExpected.y, tps->posOffsetExpected.z,
                                 tps->currentZoomOffset, tps->targetZoomOffset,
                                 pcam ? pcam->worldFOV : -1.0f,
                                 spkSet ? 1 : 0, lastSpkSet ? 1 : 0, goodbye ? 1 : 0,
                                 dialogueActiveForProfile ? 1 : 0, lookEn ? 1 : 0,
                                 settings.dlgPaceMul);
                }
                if (sDlgExitTailT > 4.1f) sDlgExitTailT = -1.0f;
            }
        }

        // Publish the env-applied Categories profile for Quick Tune. When
        // not locked catSelected == the pre-env `selected`, so this equals
        // sLastResolvedProfile; when locked it stays on the Categories tree
        // instead of the tl* swap, giving Quick Tune a lock-independent
        // Categories edit target.
        sLastCategoriesProfile = settings.ResolveByEnv(catSelected);

        if (!lastSelectedProfile) {
            currentProfile = *(vanityActiveForProfile ? gameplaySelected : selected);
            velSideOffset = velHeight = velZoom = velFOV = velRotation = velPitchOffset = 0.0f;
            auto initM = [](ChannelMotion& m, float v) {
                m.position = v;
                m.velocity = 0.0f;
                m.target   = v;
            };
            initM(mSide,     currentProfile.sideOffset);
            initM(mHeight,   currentProfile.height);
            initM(mZoom,     currentProfile.zoom);
            initM(mFOV,      currentProfile.fov);
            initM(mRotation, currentProfile.rotation);
            initM(mPitch,    currentProfile.pitchOffset);
        }
        returningToThirdPerson = false;
        lastSelectedProfile = selected;
        sLastResolvedProfile = selected;
        targetProfile = *selected;

        // PRECEDENCE, lowest to highest: Outdoor/Indoor variant -> Location
        // override -> enemy override. `selected` has already been through
        // ResolveByEnv (which asks the active Location first), and the splice
        // below then writes its enabled fields over the top â€” so an enemy
        // override wins against a Location override while locked on, which is
        // the intended order: the Location says how the WORLD should be framed,
        // the enemy override says how THIS TARGET must be framed to stay in
        // shot, and the second is the more urgent constraint. Keep this
        // ordering; moving the splice above the env/location step would let a
        // place silently overwrite a giant's zoom-out.
        //
        // Enemy override splice â€” runs whenever the lock is on a known
        // enemy type AND the picker resolved a TL slot (which it does for
        // every locked frame). Only enabled fields are spliced; everything
        // else inherits from `selected`. Splicing into targetProfile means
        // the spring chases the override values directly, so a toggled-on
        // FOV slider transitions smoothly the same as any other channel.
        //
        // The lookup is keyed off the slot anchor whenever an off-table override
        // profile won the pick (per-hand magic, per-weapon-type melee). Passing
        // the override pointer instead would defeat the directional-power-attack
        // branch inside ResolveEnemyOverride the same way it defeated the TLSlot
        // branch â€” the override isn't the direction slot either.
        CameraProfile* const enemyKey = tlSlotAnchor ? tlSlotAnchor : selected;
        // The enemy splice writes its fields OVER targetProfile, so it would
        // overwrite the paraglide framing while locked mid-glide â€” the one
        // framing that must win outright. It stands down for the glide, and
        // for a [CLIPCAM] animation override for the same reason.
        if (resolver.IsTargetLocked() && !paragliding && !animCamActive) {
            if (auto handle = TDMIntegration::GetSingleton().GetCurrentTarget()) {
                if (auto actorPtr = handle.get()) {
                    RE::Actor* lockedActor = actorPtr.get();
                    // Publish identity for the menu's "Bind current target"
                    // button (kept until replaced, even after the lock drops).
                    PublishLockedActorIdentity(lockedActor);

                    // Custom (player-bound) enemies win over the 5 built-in
                    // categories â€” a specific bind is the more intentional match.
                    const int customIdx = settings.FindCustomEnemyForActor(lockedActor);
                    if (customIdx >= 0) {
                        settings.ApplyCustomEnemyOverrideResolved(targetProfile, customIdx, enemyKey, tlSlot,
                                                                  activeTLBinding, activeTLBindingSlot);
                        if (const auto* eo = settings.ResolveCustomEnemyOverride(customIdx, enemyKey, tlSlot,
                                                                                 activeTLBinding, activeTLBindingSlot);
                            eo && eo->fieldsEnabled) {
                            sLastEnemyOverrideProfile  = const_cast<CameraProfile*>(&eo->profile);
                            sLastEnemyOverrideEnemyIdx = -1;  // custom (not a built-in index)
                            sLastCustomEnemyIdx        = customIdx;
                        }
                    } else {
                        const auto enemy = EnemyDetector::GetSingleton().Classify(lockedActor);
                        if (enemy != EnemyType::None) {
                            settings.ApplyEnemyOverrideResolved(targetProfile, EnemyTypeIndex(enemy), enemyKey, tlSlot,
                                                                activeTLBinding, activeTLBindingSlot);
                            // Publish the active override profile for Quick Tune. The
                            // override (when its master toggle is on) REPLACES the TL
                            // profile's fields above, so QT's Target Lock box must edit
                            // THIS, not the base TL slot â€” otherwise edits to the base
                            // are silently overwritten by the override every frame.
                            if (const auto* eo = settings.ResolveEnemyOverride(
                                    EnemyTypeIndex(enemy), enemyKey, tlSlot,
                                    activeTLBinding, activeTLBindingSlot);
                                eo && eo->fieldsEnabled) {
                                sLastEnemyOverrideProfile  = const_cast<CameraProfile*>(&eo->profile);
                                sLastEnemyOverrideEnemyIdx = static_cast<int>(EnemyTypeIndex(enemy));
                            }
                        }
                    }
                }
            }
        }

        sLastEnemyOverrideBindingLabel.clear();
        if (sLastEnemyOverrideProfile && activeTLBinding && activeTLBindingSlot >= 0 &&
            activeTLBindingSlot < static_cast<int>(SettingsManager::kWeaponBindingSubStates)) {
            sLastEnemyOverrideBindingLabel =
                (activeTLBinding->displayName.empty() ? std::string("Bound Weapon")
                                                     : activeTLBinding->displayName) +
                " - " + SettingsManager::GetBindingSubStateName(
                    activeTLBinding->category, activeTLBindingSlot);
        }

        // ---- [TLHAND] diagnostic ------------------------------------------
        // "The Both Hands override doesn't apply in target lock." Every layer
        // that could eat it is in scope right here, so log all of them at once
        // instead of guessing which one it was: the resolved state, the hand
        // the resolver attributed the cast to, whether the grid cell was found
        // and armed, whether the picker actually returned the hand pointer, the
        // environment substitution, and whether an enemy override then wrote
        // over the result. One line per change of that whole signature, so a
        // fight produces a handful of lines rather than a flood.
        if (resolver.IsTargetLocked() &&
            (resolver.GetState() == CameraState::Magic ||
             resolver.GetState() == CameraState::Staves)) {
            const auto  school = resolver.GetSchool();
            const auto  cast   = resolver.GetCastType();
            const bool  sneak  = resolver.IsSneaking();
            const int   hand   = static_cast<int>(resolver.GetCastingHand());
            const auto* hs     = settings.GetMagicHandSet(school, cast, sneak, /*a_targetLock=*/true);
            const bool  handOn = hs && hand >= 0 &&
                                 hand < static_cast<int>(SettingsManager::kMagicHandCount) &&
                                 hs->enabled[static_cast<std::size_t>(hand)];
            // Did the pick actually LAND on the hand cell? tlSlotAnchor is only
            // set when an off-table profile beat the plain TL slot, so this
            // separates "the grid said yes" from "the grid won".
            // (A shout cast from a magic state anchors too - to the shout
            // Base slot, not the hand cell - so exclude the Shout sub-state
            // or it reads as a hand win.)
            const bool  handWon = (tlSlotAnchor != nullptr) && (sub != CameraSubState::Shout);
            const bool  enemyHit = (sLastEnemyOverrideProfile != nullptr);
            // Both dual-cast sources, so a log proves which one carried the
            // Both attribution (see ResolveActiveHandCast: the actor flag is
            // transient, the per-caster flag is durable).
            bool dualActor = false, dualL = false, dualR = false;
            if (player) {
                dualActor = player->IsDualCasting();
                if (auto* cl = player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand))
                    dualL = cl->GetIsDualCasting();
                if (auto* cr = player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand))
                    dualR = cr->GetIsDualCasting();
            }

            // Signature: everything that changes the verdict, quantised so a
            // steady cast doesn't re-log every frame.
            const long long sig =
                (static_cast<long long>(resolver.GetState()) * 1000000LL) +
                (static_cast<long long>(resolver.GetSubState()) * 100000LL) +
                (static_cast<long long>(school) * 10000LL) +
                (static_cast<long long>(cast) * 1000LL) +
                (static_cast<long long>(hand + 1) * 100LL) +
                (handOn ? 40LL : 0LL) + (handWon ? 20LL : 0LL) +
                (enemyHit ? 10LL : 0LL) + (sneak ? 4LL : 0LL) +
                (dualActor ? 400LL : 0LL) + (dualL ? 800LL : 0LL) + (dualR ? 1600LL : 0LL) +
                (settings.indoorMode ? 2LL : 0LL) +
                (settings.ActiveLocation() ? 1LL : 0LL);
            static long long sLastSig = -1;
            if (sig != sLastSig) {
                sLastSig = sig;
                spdlog::debug("[TLHAND] state={} sub={} school={} cast={} sneak={} hand={} "
                             "set={} enabled={} handWon={} dual=a{}/l{}/r{} env={} loc={} enemy={}/{} "
                             "picked=(side={:.1f} h={:.1f} zoom={:.1f} fov={:.1f}) "
                             "final=(side={:.1f} h={:.1f} zoom={:.1f} fov={:.1f})",
                             static_cast<int>(resolver.GetState()),
                             static_cast<int>(resolver.GetSubState()),
                             static_cast<int>(school), static_cast<int>(cast), sneak, hand,
                             hs != nullptr, handOn, handWon, dualActor, dualL, dualR,
                             settings.indoorMode ? "Indoor" : "Outdoor",
                             settings.ActiveLocation() ? settings.ActiveLocation()->name : "-",
                             sLastEnemyOverrideEnemyIdx, sLastCustomEnemyIdx,
                             selected->sideOffset, selected->height, selected->zoom, selected->fov,
                             targetProfile.sideOffset, targetProfile.height,
                             targetProfile.zoom, targetProfile.fov);
            }
        }

        // ===== Spring + retarget-impulse + sub-pixel snap =====
        //
        // Per the user's direct feedback: the spring system felt the best
        // out of every curve we iterated through. The "moving to the left
        // on sprint stop" issue at the time was the engine's sheathed-
        // sprint FOV pulse, which we've since suppressed for any sprint
        // state in HookManager. Restoring this system as the chosen design.
        //
        // Three pieces:
        //   1. Critically-damped 2nd-order spring per channel (no overshoot).
        //   2. Velocity impulse on every retarget â€” the moment the target
        //      changes, we kick the velocity by `(target - pos) * omega
        //      * impulseFactor`. This eliminates the slow-start phase a
        //      pure spring would have (v(0)=0 reads as "stuck"). The
        //      camera moves perceptibly from the very first frame.
        //   3. Sub-pixel snap when |delta| AND |velocity| are both below
        //      visual threshold â€” eliminates the asymptotic tail.
        const float dt = std::clamp(smoothDt, 0.0001f, 0.05f);

        // Advance the dialogue-entry lockstep timer. Expires when the
        // face-lock cubic blend completes â€” duration is slider-scaled
        // so the lockstep window grows with the blend at lower sliders.
        if (m_dialogueEntryLockstepActive) {
            m_dialogueEntryLockstepT += dt;
            if (m_dialogueEntryLockstepT >= kDialogueEntryLockstepDuration) {
                m_dialogueEntryLockstepActive = false;
            }
        }

        // Combat enter / exit pulse. Edge-detect player IsInCombat,
        // schedule a pulse with t = duration on each edge, and bake an
        // additive impulse into FOV/zoom targets below. The spring
        // smoothly chases the modulated target â€” no separate spring,
        // no fighting the existing math. Bell-curve envelope (parabola
        // peaking at t = duration/2) so pulse rises and falls cleanly.
        if (settings.combatPulseIntensity > 0.001f) {
            const bool inCombat = player && player->IsInCombat();
            if (inCombat != m_lastInCombat) {
                m_combatPulseT     = std::max(0.1f, settings.combatPulseDuration);
                m_combatPulseEnter = inCombat;
            }
            m_lastInCombat = inCombat;
            if (m_combatPulseT > 0.0f) {
                m_combatPulseT = std::max(0.0f, m_combatPulseT - dt);
            }
        } else {
            m_combatPulseT = 0.0f;
            m_lastInCombat = player && player->IsInCombat();
        }
        // Compute additive deltas; magnitude in degrees / zoom units.
        // Enter pulse: FOV widen + zoom pull-back. Exit: opposite, half mag.
        float pulseFovDelta  = 0.0f;
        float pulseZoomDelta = 0.0f;
        if (m_combatPulseT > 0.0f && settings.combatPulseDuration > 0.001f) {
            const float duration = std::max(0.1f, settings.combatPulseDuration);
            const float u = 1.0f - (m_combatPulseT / duration);   // 0 â†’ 1 over the pulse
            const float env = 4.0f * u * (1.0f - u);              // parabola: 0 â†’ 1 â†’ 0
            const float amp = settings.combatPulseIntensity;
            const float sign = m_combatPulseEnter ? 1.0f : -0.5f; // exit at half mag, opposite direction
            pulseFovDelta  =  5.0f * env * amp * sign;
            pulseZoomDelta = -8.0f * env * amp * sign;
        }
        targetProfile.fov  += pulseFovDelta;
        targetProfile.zoom += pulseZoomDelta;

        // --- Shout-return transition latch ---
        // While a Shout sub-state is active, targetProfile IS the shout profile,
        // so its transition-speed override already governs the swing INTO the
        // shout. Cache that override each shout frame; on the edge back out of the
        // shout, keep applying it to the swing back to the parent state so the
        // shout's transition speed governs the RETURN too (per user request).
        // Cleared once the position framing has settled or after a short cap so it
        // can't bleed into steady-state (e.g. combat-pulse FOV breathing).
        {
            const bool subIsShout = (sub == CameraSubState::Shout);
            if (subIsShout) {
                m_shoutReturnOverride = targetProfile.transitionOverride;
                m_shoutReturnRotation = targetProfile.transitionRotation;
                m_shoutReturnPitch    = targetProfile.transitionPitch;
                m_shoutReturnPosition = targetProfile.transitionPosition;
                m_shoutReturnZoom     = targetProfile.transitionZoom;
                m_shoutReturnFOV      = targetProfile.transitionFOV;
                m_shoutReturnSetRotation = targetProfile.transitionSetRotation;
                m_shoutReturnSetPitch    = targetProfile.transitionSetPitch;
                m_shoutReturnSetPosition = targetProfile.transitionSetPosition;
                m_shoutReturnSetZoom     = targetProfile.transitionSetZoom;
                m_shoutReturnSetFOV      = targetProfile.transitionSetFOV;
                m_shoutReturnActive   = false;   // still shouting â€” not a return yet
            } else if (m_prevSubWasShout) {
                // Just left the shout. Only meaningful if the shout actually
                // overrode its transition (else the return uses the normal path).
                m_shoutReturnActive = m_shoutReturnOverride;
                m_shoutReturnT      = 0.0f;
            }
            m_prevSubWasShout = subIsShout;

            if (m_shoutReturnActive) {
                m_shoutReturnT += dt;
                // Settle on the FRAMING channels only â€” FOV/zoom are perturbed by
                // the combat pulse and would never converge.
                //
                // ROTATION IS ONE OF THEM. It was left out, and that is the
                // measured cause of "rotation seems to be overshooting when it
                // returns back to sheathed" ([ROTRET], 2026-09-03, third person,
                // Dragon Aspect):
                //
                //   t=625ms  pos=5.636  vel=-36.627  omega=19.20  shoutRet=1
                //   t=639ms  pos=5.130  vel=-35.622  omega= 2.70  shoutRet=0
                //   t=810ms  pos=0.004  ... crosses ...
                //   t=1731ms pos=-8.523                      <-- 8.5 deg past
                //
                // Side/height/pitch settled first, the latch cleared, and the
                // stiffness fell 19.20 -> 2.70 in one frame while rotation was
                // still 5 degrees out and moving at 36 deg/s. The exact spring is
                // monotonic only while B = v + omega*x keeps x's sign: at the
                // switch x=+5.130 and B = -35.622 + 2.70*5.130 = -21.77, opposite
                // sign, so a zero crossing became unavoidable. Under the stiffness
                // it was carrying (B = -35.622 + 19.20*5.130 = +62.9) the move was
                // clean the whole way. The drop CAUSED the overshoot.
                //
                // Rotation is framing, not FOV/zoom, so it belongs in this test on
                // the latch's own stated terms: the return is not over while the
                // camera is still five degrees from where it is going. Shortest-arc
                // so a target across the +/-180 seam reads as nearly-settled rather
                // than 360 out.
                float rotErr = mRotation.position - targetProfile.rotation;
                while (rotErr >  180.0f) rotErr -= 360.0f;
                while (rotErr < -180.0f) rotErr += 360.0f;
                const bool settled =
                    std::abs(mSide.position   - targetProfile.sideOffset)  < 0.5f &&
                    std::abs(mHeight.position - targetProfile.height)      < 0.5f &&
                    std::abs(mPitch.position  - targetProfile.pitchOffset) < 0.25f &&
                    std::abs(rotErr)                                       < 0.5f;
                if (settled || m_shoutReturnT >= 2.0f) m_shoutReturnActive = false;
            }
        }

        // Spring stiffness. Higher omega = faster transition. Fixed
        // baseline of 15 (= the former Base Speed slider's default of
        // 0.5 Ã— 30); per-channel multipliers below scale it. When
        // dialogue is active, swap the per-channel mult source from
        // the global Transitions sliders to the dedicated dialogue
        // sliders so dialogue feel tunes independently.
        const float omegaBase      = 15.0f;
        constexpr float kImpulseFactor    = 0.8f;   // bumped from 0.6 â€” more front-load
        constexpr float kSnapPositionThr  = 0.05f;
        constexpr float kSnapVelocityThr  = 0.5f;

        // Per-channel omegas. Each Transition menu slider multiplies the
        // base for its channel. mulPosition covers side+height (one knob
        // in UI). All defaults are 1.0 so today's feel is preserved when
        // sliders are untouched. When dialogue is active, the dialogue
        // menu's per-channel sliders take over so dialogue feel is
        // independent of the global Transitions menu.
        // Per-entry transition-speed override: when the resolved profile opts in
        // (and dialogue isn't driving its own per-channel sliders), its own
        // multipliers replace the global Transitions sliders for this profile.
        const auto settledChannel = [](const ChannelMotion& motion, float target, bool angle = false) {
            const float error = angle ? std::remainder(motion.position - target, 360.0f)
                                      : motion.position - target;
            return std::abs(error) < 0.05f && std::abs(motion.velocity) < 0.5f;
        };
        const bool vanityReturnSettled =
            settledChannel(mSide, targetProfile.sideOffset) &&
            settledChannel(mHeight, targetProfile.height) &&
            settledChannel(mZoom, targetProfile.zoom) &&
            settledChannel(mFOV, targetProfile.fov) &&
            settledChannel(mRotation, targetProfile.rotation, true) &&
            settledChannel(mPitch, targetProfile.pitchOffset);
        const bool vanityWasReturning = m_vanityTransition.IsReturning();
        if (dialogueActiveForProfile || animCamActive) m_vanityTransition.Reset();
        else m_vanityTransition.Update(vanityActiveForProfile, settings.vanityCamera,
                                       *selected, vanityReturnSettled);
        if (vanityWasReturning != m_vanityTransition.IsReturning()) {
            spdlog::debug("[VANITY] return={} state={} sub={} fov={:.2f}->{:.2f} zoom={:.2f}->{:.2f} settled={}",
                         m_vanityTransition.IsReturning(), static_cast<int>(resolver.GetState()), static_cast<int>(sub),
                         mFOV.position, targetProfile.fov, mZoom.position, targetProfile.zoom,
                         vanityReturnSettled);
        }
        if (!vanityActiveForProfile && !m_vanityTransition.IsReturning()) VanityCamera::FinishReturn();
        const CameraProfile& transitionProfile = m_vanityTransition.Source(targetProfile);
        const bool perEntryTrans = transitionProfile.transitionOverride && !dialogueActiveForProfile;
        // Shout-return: use the cached shout profile's per-channel transition
        // override for the swing back to the parent state (see the latch above).
        // Wins over the parent's own per-entry override / the global sliders, but
        // still yields to dialogue.
        const bool shoutRet = m_shoutReturnActive && !dialogueActiveForProfile &&
                              !vanityActiveForProfile && !m_vanityTransition.IsReturning();
        // Range stretch for the Transitions speed sliders (global and
        // per-entry): remap each multiplier through a power curve
        // anchored at the 0.5 default â€” 0.5Â·(mul/0.5)Â² = 2Â·mulÂ². The
        // sliders keep their printed 0.05â€“1.0 numbers and 0.5 feels
        // exactly as before, but 1.0 now lands twice as fast as it used
        // to (Ï‰ 30 vs 15) and 0.05 ten times slower (Ï‰ 0.075 vs 0.75).
        // Dialogue keeps its linear muls â€” those sliders were tuned
        // against their own curves (blend durations, face-lock).
        auto stretchMul = [](float a_mul) {
            const float m = std::max(0.05f, a_mul);
            return 2.0f * m * m;
        };
        // Source priority per channel: dialogue > Vanity entry/return >
        // shout-return > per-entry override > global. Vanity selects settings
        // only; it runs through this same speed curve and stepMotion below.
        // a_entrySet: does the entry override THIS channel? Each transition
        // setting carries its own toggle (2026-08-19), so an entry can slow
        // its Zoom without dragging Rotation/FOV/Pitch off the globals too.
        // An unticked channel falls through to the global exactly as if the
        // entry had no override at all.
        auto pickMul = [&](float a_dlg, bool a_shoutSet, float a_shout,
                           bool a_entrySet, float a_entry, float a_global) {
            if (dialogueActiveForProfile)    return std::max(0.05f, a_dlg);
            if (shoutRet && a_shoutSet)      return stretchMul(a_shout);
            if (perEntryTrans && a_entrySet) return stretchMul(a_entry);
            return stretchMul(a_global);
        };
        const float mulPos = pickMul(settings.dialogueMulPosition,
                                     m_shoutReturnSetPosition, m_shoutReturnPosition,
                                     transitionProfile.transitionSetPosition,
                                     transitionProfile.transitionPosition, settings.transitionMulPosition);
        const float mulZm  = pickMul(settings.dialogueMulZoom,
                                     m_shoutReturnSetZoom, m_shoutReturnZoom,
                                     transitionProfile.transitionSetZoom,
                                     transitionProfile.transitionZoom, settings.transitionMulZoom);
        const float mulFv  = pickMul(settings.dialogueMulFOV,
                                     m_shoutReturnSetFOV, m_shoutReturnFOV,
                                     transitionProfile.transitionSetFOV,
                                     transitionProfile.transitionFOV, settings.transitionMulFOV);
        const float mulRt  = pickMul(settings.dialogueMulRotation,
                                     m_shoutReturnSetRotation, m_shoutReturnRotation,
                                     transitionProfile.transitionSetRotation,
                                     transitionProfile.transitionRotation, settings.transitionMulRotation);
        const float mulPt  = pickMul(settings.dialogueMulPitch,
                                     m_shoutReturnSetPitch, m_shoutReturnPitch,
                                     transitionProfile.transitionSetPitch,
                                     transitionProfile.transitionPitch, settings.transitionMulPitch);

        // (Per-channel transition PERSONALITY resolved here until 2026-08-17.
        // Heavy and Glide were cut with the whole selector; every channel is
        // the critically-damped chase Smooth always was. See Core/Spring.h.)

        // ---- DISMOUNT: ONE CLOCK, ONE SHORT MOVE --------------------------
        //
        // MEASURED 2026-08-24 20:13 — the first trace with a real distance
        // column. HORIZONTAL camera-to-player distance across a dismount:
        //
        //   mounted        386.4   (8 frames, steady)
        //   +4             335.8
        //   +20            203.3
        //   +48            136.6   <- posOffset.y is basically done here
        //   +98            122.9   <- zoom and FOV are STILL creeping in
        //   settled       ~122.5
        //
        // The camera travels 263 units TOWARD the player, down to under a
        // third of the mounted distance, and DDC spreads that over ~1.5
        // seconds. That slow push-in IS the complaint: "it zooms into the
        // player." Vanilla does it as a state swap in one frame, because the
        // third-person state's offsets are simply already correct.
        //
        // And it arrives in TWO PHASES, which is what "2 separate instances of
        // zooming into the player" was describing: the posOffset.y carry runs
        // on omegaZoom and is done by ~+48 (386 -> 137), then zoom and FOV keep
        // pulling for another fifty frames (137 -> 123) on their own clocks.
        // Two overlapping inward moves finishing at different times read as two
        // separate zoom-ins, because that is exactly what they are.
        //
        // A hard cut is NOT the answer — that was the original report ("the
        // transition back to third person looks terrible and snaps") and it is
        // what started this whole chase. So: one clock, and a short one. Every
        // framing channel runs at the same fixed omega across the dismount,
        // ignoring the per-channel sliders for this one edge, so the move is a
        // single quick handover instead of a dolly delivered in instalments.
        //
        // A 0.30s one-clock blend was tried first and the user still reported
        // two zoom-ins, because ANY blend from the mounted framing is a
        // push-in — it just gets shorter. The ask, four times over, was:
        // "get off the horse and have the camera switch nicely to my third
        // person settings." So the channels do not travel from the mount's
        // framing at all. They ARRIVE at the on-foot framing.
        //
        // The engine is already doing the right thing underneath — its own
        // first third-person frame had act=(50.0, 0.0, -20.0) zoom -0.100,
        // which IS the user's third person profile. Snapping our channels to
        // the same targets means DDC agrees with that handover instead of
        // dragging the camera in from the saddle over the next second and a
        // half. Horseback only; dragon riding is untouched.
        // WHY A BLEND IS SAFE NOW AND WAS NOT BEFORE. An earlier 0.30s blend
        // still read as a push-in, because the posOffset.y CARRY was alive and
        // the blend was dragging 300 units of mount pull-back along with it.
        // With the carry gone the engine's own handover supplies the distance,
        // and these channels only have to cover side ~50, height ~20, a little
        // zoom, and the FOV step. Blending THAT cannot become a dolly — it is
        // the settle the user is missing now that it snaps.
        //
        // One clock for every channel, deliberately. Letting them run on their
        // own sliders is what produced two zoom-ins finishing at different
        // times; that must not come back just because the move is smaller.
        // THE ONE NUMBER TO TUNE. Everything else about this edge is settled;
        // if the dismount reads as a zoom, lower it, and if it reads as a snap,
        // raise it. 1.5s (the old zoom-slider path) was a cinematic dolly. One
        // frame is the engine's raw cut. This is the dial between them.
        // THE DISMOUNT USES THE NORMAL TRANSITION SYSTEM. NO SPECIAL CASE.
        //
        // A `[MOUNTEXIT-BLEND]` lived here and forced every framing channel to
        // a hardcoded 0.22 s clock on the Horseback -> anything edge, bypassing
        // omegaBase and the per-channel transition sliders entirely. It also
        // suppressed Weight (see spanOmegaScaleFor). It was tuned down from
        // 1.5 s to 0.30 s to 0.22 s across a dozen rounds, chasing a
        // "push-in" that turned out to be other bugs — the MOUNTEXIT guard
        // re-injecting the mount's 300-unit pull-back, and DDC not covering the
        // engine's transition at all. Both of those are fixed now, and what the
        // special case was left doing was making this one edge feel like
        // nothing else in the mod. User, 2026-08-25: *"it should be using our
        // transition system. it doesn't, at all. its just a broken snap. it
        // should feel like the transitions everywhere else in this mod."*
        //
        // So: no override. Same omegas, same sliders, same Weight, same
        // velocity caps as every other state change. If the dismount needs to
        // be faster or slower it is a TRANSITION SLIDER, not a constant here.
        // DO NOT REINTRODUCE A PER-EDGE CLOCK.
        const float omegaSide  = omegaBase * mulPos;
        const float omegaHght  = omegaBase * mulPos;
        const float omegaZoom  = omegaBase * mulZm;
        const float omegaFov   = omegaBase * mulFv;
        const float omegaRot   = omegaBase * mulRt;
        const float omegaPit   = omegaBase * mulPt;

        // Framerate-independent per-channel velocity cap. Bounds the
        // per-frame step to `maxSpeed * dt` regardless of framerate, so
        // a 200-unit zoom transition that completes in 21 frames at
        // 30 FPS (lurching ~10 units/frame) instead completes in N
        // frames where N = (delta / maxSpeed) * fps â€” same wall-clock
        // duration at every framerate, smaller visible steps as fps
        // rises. Cap is per-second so the spring's settle time grows
        // gracefully on slow rigs but never produces visible per-frame
        // jumps on fast ones. Engages only when natural spring velocity
        // exceeds maxSpeed (large retargets like dialogue entry); small
        // transitions stay under the cap and feel snappy as before.
        constexpr float kMaxChannelSpeed = 600.0f;  // bumped from 60: was too low; directional
                                                    // PA profiles with large deltas (>60 units) never
                                                    // completed within a ~1.4s PA window because the
                                                    // spring's velocity stayed pinned at the cap and
                                                    // its natural exponential decay slowed it as it
                                                    // approached the target. 600 lets the impulse
                                                    // carry the early frames so the spring's settle
                                                    // window finishes inside one PA.
        // ---- "Weight": move-size-aware duration -------------------------
        // A spring settles in ~4/omega no matter how far it travels, so today
        // a 4-unit height nudge and a 90-unit zoom pull take exactly as long
        // as each other and PEAK SPEED is what scales with distance. Real
        // camera motion is the other way round â€” an operator's hands are
        // speed-bounded, so a bigger move takes longer. Weight scales a
        // discrete retarget's omega by (refSpan/span)^(0.5*weight).
        //
        // refSpan is per-channel and picked so a TYPICAL move for that channel
        // returns 1.0: sheathed->combat moves side/height ~30u, the engine's
        // own combat FOV step is 10 degrees, and so on. That keeps every
        // existing slider meaning what it means at normal magnitudes â€” only
        // the extremes move.
        //
        // Result is CLAMPED to a duration multiplier of [0.4x, 2.5x] nominal
        // so no move becomes instant or glacial, and spans below 2% of the
        // reference return 1.0 outright: a sub-noise correction is not a
        // transition and must not collect the ceiling multiplier.
        constexpr float kRefSpanPos   = 25.0f;   // units  (side / height)
        constexpr float kRefSpanZoom  = 25.0f;   // SLIDER units
        constexpr float kRefSpanFov   = 12.0f;   // degrees
        constexpr float kRefSpanRot   = 15.0f;   // degrees
        constexpr float kRefSpanPitch = 10.0f;   // degrees
        // mMountY IS THE ONE CHANNEL MEASURED IN GAME UNITS, AND IT NEEDS ITS
        // OWN REFERENCE.
        //
        // It borrows the zoom channel's omega and cap, and it was borrowing
        // kRefSpanZoom too — but that reference is in SLIDER units and
        // posOffset.y is in GAME units. One slider unit is 4.436 game units, so
        // the mount pull-back's ~300u span was being measured against a
        // reference roughly ten times too small, i.e. reported to the Weight
        // logic as a twelve-reference move when the same physical distance
        // expressed as zoom is barely more than one.
        //
        // That matters because the group takes the MOST-SLOWED member's scale.
        // At the user's Weight of 0.3 the bad reference gave 0.689 and dragged
        // every other channel down with it, so the whole mount and dismount ran
        // ~45% longer than the sliders asked for — "it doesn't use my
        // transition settings", from the opposite direction to the per-edge
        // clocks that were just removed. Converting the reference puts it back
        // on the same footing as the zoom it shares a screen with.
        constexpr float kRefSpanMountY = kRefSpanZoom * Defaults::GameUnitsPerSliderZoom;
        // AND THE VELOCITY CAP IS THE SAME TRAP ON THE SAME LINE.
        //
        // mMountY also borrows m_chanCapZoom, which is 600 in SLIDER units.
        // In game units that same cap is 600 * 4.436 = 2662 u/s, so the one
        // channel in the mod carrying a ~300-unit span was capped 4.4x tighter
        // than the zoom it is twinned with.
        //
        // IT BINDS, AND IT BINDS FIRST. The retarget impulse is
        // span * omega * scale * kRetargetImpulse; at the user's sliders
        // (mul_zoom 0.4 -> omega 4.8, group scale ~0.91) that is
        // 297.8 * 4.8 * 0.91 * 0.8 = 1041 u/s, clipped to 600 on frame one and
        // HELD there until omega*x falls back under the cap ~16 frames later.
        // Both horse edges in the 2026-08-27 16:22 log show it plainly —
        // posOffset.y moves a FLAT 11-12.8 units per frame for the first ~10
        // frames of the mount AND the dismount, then decays. A critically
        // damped spring has no flat top. A clamp does.
        //
        // So for the first quarter-second of every mount and dismount — the
        // part that is actually watched — the distance channel was not a
        // spring at all, it was a fixed-rate ramp at a hardcoded 600 u/s. The
        // transition speed slider could not change it, Weight could not change
        // it (Weight sets peak speed by move size, which is exactly what the
        // clamp overrides), and the group pacing could not change it.
        //
        // By this file's own test — "does it change what a slider does, on one
        // edge only?" — THE CAP IS A PER-EDGE CLOCK, the fourth found on this
        // camera and the last one standing. User, five days running: *"horse
        // transitions aren't using the transition system."*
        //
        // The earlier note that the cap "was checked and does NOT bind here —
        // peak spring velocity for a 300u move at these omegas is ~456 u/s"
        // used the wrong omega. It binds on frame one, every time.
        // [[horse-edges-use-the-normal-machinery]]
        const float chanCapMountY = m_chanCapZoom * Defaults::GameUnitsPerSliderZoom;
        // Weight follows the same source priority as the personalities:
        // per-entry override > global. It deliberately skips the shout-return
        // latch (that caches SPEEDS only) and dialogue (whose lockstep blend
        // never calls stepMotion at all), so there is nothing for either to
        // contribute. A werewolf entry can therefore carry more mass than a
        // sheathed walk â€” the case this was added for.
        const float transWeight = std::clamp(
            (perEntryTrans && transitionProfile.transitionSetWeight)
                ? transitionProfile.transitionWeight : settings.transitionWeight,
            0.0f, 1.0f);
        auto spanOmegaScaleFor = [transWeight](float span, float refSpan) -> float {
            // (The dismount used to bypass Weight here, to keep its fixed
            // one-clock blend in step. That special case is gone — the
            // dismount is an ordinary transition now and gets ordinary
            // Weight.)
            if (transWeight <= 0.0001f) return 1.0f;
            const float s = std::abs(span);
            if (s < refSpan * 0.02f) return 1.0f;
            const float scale = std::pow(refSpan / s, 0.5f * transWeight);
            return std::clamp(scale, 0.4f, 2.5f);
        };

        // "Masking" (scaling the live transition rate up with the camera's own
        // yaw rate, on the theory that fast rotation hides change) was built
        // and CUT 2026-08-16. It was NOT inert â€” the [TRANS] capture showed it
        // hitting its 2.5x ceiling on HALF of all moves, average peak 1.95x â€”
        // and the user still could not perceive it at any setting. Working
        // that hard for no felt result is not worth the two costs it carried:
        // its real effect was just "transitions are faster", which the speed
        // sliders already do honestly, and it made transitions
        // NON-REPRODUCIBLE (the same state change taking a different time
        // depending on how the mouse happened to be moving). Do not
        // re-propose. See [[transition-weight-move-size-duration]].

        // externalScale: the caller has already written m.spanOmegaScale and
        // stepMotion must not overwrite it. Used by Coordinate, which sizes
        // the whole shoulder-offset group's window together (below).
        auto stepMotion = [&](ChannelMotion& m, const char* chLabel, float newTarget,
                              float omegaCh, float maxSpeed,
                              float refSpan, bool externalScale = false) {
            // No omega stability clamp: the integration below is the EXACT
            // closed-form critically-damped spring (CriticalDampedSpringExact),
            // which is unconditionally stable at any omega and dt. A maxed
            // Transition-speed slider (omega up to 120) therefore gives a
            // genuinely fast transition that settles in the same wall-clock
            // time (~4/omega s) at every framerate, instead of the explicit
            // integrator's jitter/divergence past omega*dt ~= 0.83.
            if (newTarget != m.target) {
                // Retarget impulse â€” instant velocity boost so the camera
                // starts moving in the very first frame of a DISCRETE retarget
                // (sprint release, power-attack/shout entry). No slow-start.
                // Factor < 1 keeps the exact spring monotonic (B below stays on
                // x's side of zero), so this never overshoots.
                //
                // GATED to the at-rest edge: re-applying the impulse every frame
                // on a CONTINUOUSLY moving target (height/side/zoom retargeting
                // while the player moves, adaptive-collision zoom near walls)
                // re-injects velocity each frame and over-drives the exact spring
                // into a constant jitter. The old 60u/s channel cap masked this;
                // raising it to 600 this session unmasked it. Once the spring is
                // already in flight it tracks the target smoothly on its own, so
                // we only kick when the channel is essentially settled â€” fast
                // continuous motion then gets a single kick at onset and rides
                // the pure spring afterward.
                if (std::abs(m.velocity) < kSnapVelocityThr) {
                    // At-rest edge = a DISCRETE retarget. This is exactly the
                    // moment Weight latches its scale for (see
                    // ChannelMotion::spanOmegaScale): sampled once here and
                    // held for the whole move, so a continuously-moving target
                    // can't re-sample a tiny span every frame and ratchet the
                    // channel rigid. The impulse then inherits the scaled
                    // omega, which is the free half of the feature â€” a big
                    // move gets a proportionally gentler kick instead of
                    // lurching on frame one.
                    if (!externalScale)
                        m.spanOmegaScale = spanOmegaScaleFor(newTarget - m.position, refSpan);
                    // [TRANS] â€” the ONLY window into Weight / Coordinate /
                    // Masking. All three change a duration and nothing else,
                    // so "I can't see a difference" and "it is doing nothing"
                    // are indistinguishable without this line. Fires once per
                    // DISCRETE retarget per channel, which is a handful of
                    // lines per state change.
                    {
                        const float span = newTarget - m.position;
                        if (std::abs(span) >= refSpan * 0.02f) {
                            const float omegaEff = omegaCh * m.spanOmegaScale;
                            spdlog::debug("[TRANS] {} span={:.1f} omega={:.2f} weightScale={:.2f} "
                                         "dur={:.2f}s grouped={}",
                                         chLabel, span, omegaCh, m.spanOmegaScale,
                                         4.0f / (std::max)(omegaEff, 0.01f),
                                         externalScale ? 1 : 0);
                        }
                    }
                    m.velocity += (newTarget - m.position) * omegaCh * m.spanOmegaScale *
                                  kRetargetImpulse;
                }
                m.target = newTarget;
            }
            const float omegaW = omegaCh * m.spanOmegaScale;
            // Bound the impulse spike so a very large retarget can't lurch in a
            // single frame; the exact spring paces everything after it. With the
            // channel caps at 600 this only ever engages on huge deltas â€” and
            // with Weight up it engages less still, since peak speed becomes
            // near-constant by construction.
            m.velocity = std::clamp(m.velocity, -maxSpeed, maxSpeed);

            // MONOTONICITY GUARD â€” the invariant this file already reasons in
            // terms of, finally enforced.
            //
            // The exact critically damped solution is
            //   x(t) = (x0 + B*t) * e^(-omega*t),   B = v0 + omega*x0
            // which crosses zero exactly when B's sign differs from x0's. The
            // retarget impulse above is sized (factor < 1) to keep B on x's side,
            // and the comment there says so â€” but that argument only covers the
            // impulse. It says nothing about OMEGA CHANGING UNDER A MOVING
            // CHANNEL, and when omega falls the same velocity that was safe at
            // the old stiffness can be far past what the new one can absorb.
            //
            // That is exactly what [ROTRET] measured on the shout return (see the
            // shout-return latch): omega 19.20 -> 2.70 in one frame at v=-35.6,
            // x=+5.13, and the channel sailed 8.5 degrees past its target and took
            // four seconds to crawl back. The latch fix upstream removes that
            // particular stiffness cliff; this removes the CLASS of it, for every
            // channel and every source of an omega change.
            //
            // Setting B to exactly zero is the fastest approach the new stiffness
            // permits without crossing (x decays as a pure e^(-omega*t)), so this
            // costs no speed it was entitled to keep. It is a no-op in normal
            // operation: the impulse produces |v| = omega*|x|*kRetargetImpulse,
            // already inside the bound, and a spring left alone never leaves it.
            {
                const float xErr = m.position - m.target;
                const float bTerm = m.velocity + omegaW * xErr;
                if (bTerm * xErr < 0.0f) m.velocity = -omegaW * xErr;
            }
            CriticalDampedSpringExact(m.position, m.velocity, m.target, omegaW, dt);

            // Sub-pixel snap eliminates asymptotic tail.
            if (std::abs(m.target - m.position) < kSnapPositionThr &&
                std::abs(m.velocity)            < kSnapVelocityThr) {
                m.position = m.target;
                m.velocity = 0.0f;
                // Move over â€” the next discrete retarget gets a fresh sample.
                m.spanOmegaScale = 1.0f;
            }
        };

        // Dialogue ENTRY edge: snap mZoom to the transition target so the
        // spring is at-target throughout. During dialogue ApplyZoom is
        // bypassed (engine zoom held at sSavedEngineCurrentZoom) and compY
        // is computed from effectiveZoom. With the spring in-flight, compY
        // transitions slowly from non-dialogue to dialogue value, and the
        // smoothstep lockstep ramp lerps a moving target â€” a dip-and-return
        // on posOffsetExpected.y that finishes after the face-lock cubic.
        // Snapping mZoom on entry makes compY constant from frame N.
        //
        // ENTRY only. The EXIT half of this snap is what the user saw as
        // "dialogue still snaps": [DLGX-EXIT] (2026-08-15 19:52-19:55 log)
        // shows side/height/fov easing over ~1.3s while zoom's position
        // teleported to the target between the first two samples â€” this
        // line. The 2026-05-23 rationale (let the ENGINE ease zoom out and
        // hold our spring constant to avoid a double-ease dip) died when
        // the 2026-06-27 exit work PINNED the engine zoom offsets: nothing
        // eased zoom at all, so the snap rendered raw. With the snap gone
        // the shared channel blend below captures startZoom = the live
        // dialogue zoom and eases it in lockstep with the other channels;
        // the engine side stays pinned, so no double-ease.
        if (dlgEdge) {
            mZoom.position = targetProfile.zoom;
            mZoom.velocity = 0.0f;
        }

        // Capture shared-smoothstep blend state on dialogue edges.
        // Replaces per-channel spring during the lockstep window with
        // a single smoothstep curve so all 5 channels arrive together.
        // Without this, channels with different travel sizes finish at
        // different times (e.g. fov with 55u travel trails pitch with
        // 7u by hundreds of ms even with matched omegas), producing
        // the "channels cut in line" feel the user reported 2026-05-24.
        // Captured AFTER the zoom edge-snap so startZoom reflects the
        // post-snap value.
        // THE "DIALOGUE LEAK", SOLVED 2026-08-16 â€” it was never dialogue.
        // User repro: "it happens when moving while exiting dialogue", and
        // the log proved them exactly right. Timeline from the 18:53 capture:
        // menu closes and the exit blend arms toward Sheathed (30,-20,15,110);
        // 0.9s later "StateResolver: sprint started" and the resolved profile
        // becomes sheathedSprint (0,-20,35,80) â€” but the blend below drives
        // positions to the targets it CAPTURED AT THE EXIT EDGE, so the camera
        // flew all the way to the sheathed framing first, and only when the
        // blend expired (t=1.5) turned around and travelled to the sprint
        // framing. That visible detour â€” the camera going somewhere wrong
        // right after a conversation, then correcting â€” is what has read as
        // "dialogue settings leaking into third person" all along.
        //
        // FIX: a resolved-target change DURING the blend re-arms it, exactly
        // like a preset switch. The re-arm below already carries each
        // channel's live analytic velocity, so the redirect is continuous â€”
        // the camera curves toward the new state instead of completing a
        // stale journey. Compared against the blend's own captured targets
        // (not the springs) so it fires once per real change, not per frame.
        const bool dlgBlendRetarget =
            m_dlgChanBlend.active && !dialogueActiveForProfile &&
            (std::abs(m_dlgChanBlend.targetSide   - targetProfile.sideOffset)  > 0.01f ||
             std::abs(m_dlgChanBlend.targetHeight - targetProfile.height)      > 0.01f ||
             std::abs(m_dlgChanBlend.targetZoom   - targetProfile.zoom)        > 0.01f ||
             std::abs(m_dlgChanBlend.targetFov    - targetProfile.fov)         > 0.01f ||
             std::abs(m_dlgChanBlend.targetPitch  - targetProfile.pitchOffset) > 0.01f);
        if (dlgBlendRetarget) {
            spdlog::debug("[DLGX-EXIT] mid-blend retarget (state changed during exit): "
                         "side {:.1f}->{:.1f} zoom {:.1f}->{:.1f} fov {:.1f}->{:.1f}",
                         m_dlgChanBlend.targetSide, targetProfile.sideOffset,
                         m_dlgChanBlend.targetZoom, targetProfile.zoom,
                         m_dlgChanBlend.targetFov,  targetProfile.fov);
        }
        if (dlgEdge || dlgExitEdge || dlgPresetSwitch || dlgBlendRetarget) {
            // MID-FLIGHT RE-ARM: carry each channel's current analytic
            // velocity into the new blend. The 17:30 log showed preset
            // switches arming in bursts 50-70ms apart; every restart pinned
            // the smootherstep back to t=0, whose velocity is ZERO â€” a
            // moving camera stopped dead and re-accelerated on each one
            // (the "snappy transitions" report). Same family as the noise
            // crossfade's mid-flight collapse: two-phase machinery is only
            // continuous between COMPLETED runs unless the re-arm path
            // hands over the live state.
            float vSide = 0.0f, vHeight = 0.0f, vZoom = 0.0f, vFov = 0.0f, vPitch = 0.0f;
            if (m_dlgChanBlend.active) {
                const float durPosOld = SettingsManager::DialogueBlendDuration(
                    (std::max)(0.05f, settings.dialogueMulPosition));
                const float durFovOld = SettingsManager::DialogueBlendDuration(
                    (std::max)(0.05f, settings.dialogueMulFOV));
                // d/dÏ„ of the quintic Hermite at the old blend's phase. The
                // start-velocity basis term matters too â€” the old blend may
                // itself have been a re-arm.
                const auto velAt = [&](float p0, float v0, float p1, float dur) {
                    const float tau = std::clamp(m_dlgChanBlend.elapsed /
                                                 (std::max)(0.001f, dur), 0.0f, 1.0f);
                    const float t2 = tau * tau, t3 = t2 * tau, t4 = t3 * tau;
                    const float dh1 = 30.0f * t2 - 60.0f * t3 + 30.0f * t4;   // d(pos basis)
                    const float dhv = 1.0f - 18.0f * t2 + 32.0f * t3 - 15.0f * t4;
                    return (p1 - p0) * dh1 / (std::max)(0.001f, dur) + v0 * dhv;
                };
                vSide   = velAt(m_dlgChanBlend.startSide,   m_dlgChanBlend.startVelSide,
                                m_dlgChanBlend.targetSide,   durPosOld);
                vHeight = velAt(m_dlgChanBlend.startHeight, m_dlgChanBlend.startVelHeight,
                                m_dlgChanBlend.targetHeight, durPosOld);
                vZoom   = velAt(m_dlgChanBlend.startZoom,   m_dlgChanBlend.startVelZoom,
                                m_dlgChanBlend.targetZoom,   durPosOld);
                vFov    = velAt(m_dlgChanBlend.startFov,    m_dlgChanBlend.startVelFov,
                                m_dlgChanBlend.targetFov,    durFovOld);
                vPitch  = velAt(m_dlgChanBlend.startPitch,  m_dlgChanBlend.startVelPitch,
                                m_dlgChanBlend.targetPitch,  durPosOld);
            }
            m_dlgChanBlend.startVelSide   = vSide;
            m_dlgChanBlend.startVelHeight = vHeight;
            m_dlgChanBlend.startVelZoom   = vZoom;
            m_dlgChanBlend.startVelFov    = vFov;
            m_dlgChanBlend.startVelPitch  = vPitch;
            m_dlgChanBlend.active   = true;
            // A mid-blend RETARGET is a redirect of the SAME exit, not a new
            // one: keep isExit and the Y/Z capture the close-edge block
            // already made this exit, or the posOffset Y/Z easing would drop
            // out halfway and hand those channels back mid-flight.
            if (!dlgBlendRetarget) {
                m_dlgChanBlend.isExit   = dlgExitEdge;
                // The close-edge block further down owns the Y/Z capture and
                // sets this back to true. Clearing it here means a blend armed
                // on a frame that never reaches that block (dialogue closing
                // straight into another menu) simply doesn't drive Y/Z,
                // instead of driving them from the last exit's stale numbers.
                m_dlgChanBlend.yzValid  = false;
            }
            m_dlgChanBlend.elapsed  = 0.0f;
            m_dlgChanBlend.duration = std::max(0.001f, kDialogueEntryLockstepDuration);
            m_dlgChanBlend.startSide    = mSide.position;
            m_dlgChanBlend.startHeight  = mHeight.position;
            m_dlgChanBlend.startZoom    = mZoom.position;
            m_dlgChanBlend.startFov     = mFOV.position;
            m_dlgChanBlend.startPitch   = mPitch.position;
            m_dlgChanBlend.targetSide   = targetProfile.sideOffset;
            m_dlgChanBlend.targetHeight = targetProfile.height;
            m_dlgChanBlend.targetZoom   = targetProfile.zoom;
            m_dlgChanBlend.targetFov    = targetProfile.fov;
            m_dlgChanBlend.targetPitch  = targetProfile.pitchOffset;
            // Exit-only Y captures populated in the close-edge branch
            // below (sSavedEngineY isn't in scope here â€” declared further
            // down in the function). isExit flag is set above; the fall-
            // through reads it together with startExpectedY/startActualY/
            // targetY which the close-edge fills in this same frame.
            if (dlgPresetSwitch) {
                spdlog::debug("[DLG-EDGE] PRESET-SWITCH dur={:.3f}", m_dlgChanBlend.duration);
            }
        }

        // Proximity pace upkeep: HookManager's dialogue block writes
        // dlgPaceMul every dialogue frame; once dialogue closes nothing
        // writes it, so relax it back to full speed here â€” the exit blends
        // (position below, the zoom ease in HookManager) start gentle next
        // to the speaker and pick up as the camera clears them.
        if (!dialogueActiveForProfile)
            settings.dlgPaceMul = (std::min)(1.0f, settings.dlgPaceMul + dt * 2.0f);

        if (m_dlgChanBlend.active) {
            // Advance on the proximity-paced clock, not wall time â€” the same
            // pace the aim blend runs on (see s_dialogueAimWarpT), so the two
            // stay coordinated while both ease off near a face.
            m_dlgChanBlend.elapsed += dt * std::clamp(settings.dlgPaceMul, 0.2f, 1.0f);
            // Per-channel durations. side/height/zoom/pitchOffset are the composed
            // shoulder offset and MUST share one window (the Position duration) so
            // the posOffset Y/Z composition stays coordinated â€” decoupling zoom here
            // reintroduces the "dead last" desync. FOV is applied via worldFOV on a
            // separate path, so it gets its own duration. Quintic smootherstep
            // (6t^5-15t^4+10t^3): zero velocity AND acceleration at both endpoints.
            const float durPos = SettingsManager::DialogueBlendDuration(
                                     (std::max)(0.05f, settings.dialogueMulPosition));
            const float durFov = SettingsManager::DialogueBlendDuration(
                                     (std::max)(0.05f, settings.dialogueMulFOV));
            // Quintic HERMITE with zero endpoint accelerations. The position
            // basis is exactly the old smootherstep, so with zero start
            // velocity (a fresh entry) this is bit-identical to the lerp it
            // replaces; a re-armed blend starts at the carried velocity
            // instead of stopping dead (see the arm block above).
            auto hermite = [](float p0, float v0, float p1, float dur, float elapsed) {
                const float d   = (std::max)(0.001f, dur);
                const float tau = std::clamp(elapsed / d, 0.0f, 1.0f);
                const float t2 = tau * tau, t3 = t2 * tau, t4 = t3 * tau, t5 = t4 * tau;
                const float h1 = 10.0f * t3 - 15.0f * t4 + 6.0f * t5;         // smootherstep
                const float hv = tau - 6.0f * t3 + 8.0f * t4 - 3.0f * t5;     // start-velocity
                return p0 + (p1 - p0) * h1 + v0 * d * hv;
            };
            mSide.position   = hermite(m_dlgChanBlend.startSide,   m_dlgChanBlend.startVelSide,
                                       m_dlgChanBlend.targetSide,   durPos, m_dlgChanBlend.elapsed);
            mHeight.position = hermite(m_dlgChanBlend.startHeight, m_dlgChanBlend.startVelHeight,
                                       m_dlgChanBlend.targetHeight, durPos, m_dlgChanBlend.elapsed);
            mZoom.position   = hermite(m_dlgChanBlend.startZoom,   m_dlgChanBlend.startVelZoom,
                                       m_dlgChanBlend.targetZoom,   durPos, m_dlgChanBlend.elapsed);
            mPitch.position  = hermite(m_dlgChanBlend.startPitch,  m_dlgChanBlend.startVelPitch,
                                       m_dlgChanBlend.targetPitch,  durPos, m_dlgChanBlend.elapsed);
            mFOV.position    = hermite(m_dlgChanBlend.startFov,    m_dlgChanBlend.startVelFov,
                                       m_dlgChanBlend.targetFov,    durFov, m_dlgChanBlend.elapsed);
            // Zero velocities and pin targets so the spring is in a clean
            // at-target state on the frame the blend completes â€” no
            // post-blend impulse kick.
            mSide.velocity   = 0.0f;  mSide.target   = m_dlgChanBlend.targetSide;
            mHeight.velocity = 0.0f;  mHeight.target = m_dlgChanBlend.targetHeight;
            mZoom.velocity   = 0.0f;  mZoom.target   = m_dlgChanBlend.targetZoom;
            mFOV.velocity    = 0.0f;  mFOV.target    = m_dlgChanBlend.targetFov;
            mPitch.velocity  = 0.0f;  mPitch.target  = m_dlgChanBlend.targetPitch;
            // Stay active until the LONGEST channel finishes (so the posOffsetActual
            // lockstep write window â€” gated on this â€” spans every channel).
            if (m_dlgChanBlend.elapsed >= (std::max)(durPos, durFov)) {
                m_dlgChanBlend.active = false;
            }
        } else {
            // ---- Weight is GROUP-SCOPED (2026-08-16) ----------------------
            // side / height / zoom / pitch together decide where the player
            // sits in frame, so Weight must stretch them as ONE move. Sizing
            // each channel independently was the mistake: identical speed
            // sliders used to guarantee identical settle times (that is what
            // makes a spring's duration span-independent), and per-channel
            // Weight broke that guarantee â€” it was the only thing pulling
            // equally-tuned channels apart. The short-tempered "Coordinate"
            // toggle that patched it afterwards is gone; the fix belongs
            // here, where the divergence was created.
            //
            // ONE scale for the group = the MOST-SLOWED member's, i.e. the
            // channel with the furthest to travel sets how long the move
            // takes. It is a shared MULTIPLIER, never a shared omega: the
            // per-channel speed sliders keep their exact relative meaning,
            // so "fast rotation, lazy zoom" still works and Weight simply
            // stretches the whole thing together.
            //
            // Latched on GROUP terms â€” only when every member is at rest AND
            // some member retargets â€” so a continuously-moving target cannot
            // re-size the window every frame. Members pass externalScale so
            // stepMotion uses this value instead of computing its own.
            // FOV and Rotation stay out, exactly as the dialogue blend leaves
            // them out: FOV rides worldFOV on its own path, rotation is
            // engine yaw.
            {
                struct GroupCh { ChannelMotion* m; float target; float omega; float ref; };
                const GroupCh grp[] = {
                    { &mSide,   targetProfile.sideOffset,  omegaSide, kRefSpanPos   },
                    { &mHeight, targetProfile.height,      omegaHght, kRefSpanPos   },
                    { &mZoom,   targetProfile.zoom,        omegaZoom, kRefSpanZoom  },
                    { &mPitch,  targetProfile.pitchOffset, omegaPit,  kRefSpanPitch },
                    // The dismount pull-back shares the group so the Weight
                    // scale is computed across it too — it is usually the
                    // LARGEST span in the move, and leaving it out let every
                    // other channel be paced by something 300 units smaller.
                    { &mMountY, sMountYTargetLive,        omegaZoom, kRefSpanMountY },
                };
                bool atRest = true, anyRetarget = false;
                for (const auto& c : grp) {
                    if (std::abs(c.m->velocity) >= kSnapVelocityThr) atRest = false;
                    if (c.target != c.m->target)                     anyRetarget = true;
                }
                if (atRest && anyRetarget) {
                    float groupScale = 1.0e9f;
                    for (const auto& c : grp) {
                        const float span = c.target - c.m->position;
                        // A channel barely moving must not set the pace for
                        // one crossing the room.
                        if (std::abs(span) < c.ref * 0.02f) continue;
                        groupScale = (std::min)(groupScale, spanOmegaScaleFor(span, c.ref));
                    }
                    if (groupScale < 1.0e8f) {
                        for (const auto& c : grp)
                            c.m->spanOmegaScale = groupScale;
                    }
                }
            }
            stepMotion(mSide,   "side",   targetProfile.sideOffset,  omegaSide, m_chanCapSide,   kRefSpanPos,   true);
            stepMotion(mHeight, "height", targetProfile.height,      omegaHght, m_chanCapHeight, kRefSpanPos,   true);
            stepMotion(mZoom,   "zoom",   targetProfile.zoom,        omegaZoom, m_chanCapZoom,   kRefSpanZoom,  true);
            stepMotion(mFOV,    "fov",    targetProfile.fov,         omegaFov,  m_chanCapFov,    kRefSpanFov);
            stepMotion(mPitch,  "pitch",  targetProfile.pitchOffset, omegaPit,  m_chanCapPitch,  kRefSpanPitch, true);
            // Same spring, same Weight scale, and now the same cap AND the
            // same reference span as the zoom it shares the screen with —
            // both converted into this channel's own game units, because
            // borrowing either one raw is how a slider stops meaning anything
            // on this edge. Target is the engine's pull-back while mounted and
            // 0 off the horse, so mount and dismount are one channel running
            // the same group in opposite directions.
            stepMotion(mMountY, "mounty", sMountYTargetLive,        omegaZoom, chanCapMountY,   kRefSpanMountY, true);

            // A [CAMCOVER] CONVERGENCE LIVED HERE FOR TWO BUILDS. IT IS GONE.
            //
            // It snapped side/height/zoom to their targets during the engine's
            // dismount lerp so the destination could not move underneath it,
            // and an FOV omega FLOOR with Weight pinned to 1.0 sat beside it.
            // Together they meant that on a dismount the per-channel transition
            // sliders and the Weight setting did NOTHING. User: *"it doesn't
            // use my transition settings"* — correct, and this was why.
            //
            // It was a per-edge clock wearing a different hat, and it is the
            // third one this camera has grown on a horse edge.
            //
            // The reversal it was built for is real and worth naming, because
            // the honest fix follows from it: the destination pose EXCLUDED the
            // mount pull-back, so a partially-eased destination sat CLOSER than
            // the finished one and the engine's lerp dived past it. Freezing
            // the destination hides that. Putting the pull-back BACK into the
            // destination removes it — as an ordinary channel, so the
            // destination walks monotonically from the mounted framing to the
            // on-foot framing and the lerp has nothing to overshoot.
            //
            // mMountY does exactly that now, on BOTH edges, in the same Weight
            // group as everything else. See
            // [[horse-edges-use-the-normal-machinery]].
        }

        // [FOVDIAG] One-shot forensic for the negative-worldFOV "inside-out"
        // overshoot. Reads the RAW FOV channel BEFORE the floor below so it
        // captures the actual out-of-range value plus the spring state that
        // produced it (pos/vel/target/omega/dt/cap). Self-arming, hard-capped
        // so it can't spam. TEMPORARY â€” remove together with the floor's root
        // cause once the overshoot mechanism is confirmed in a fresh repro.
        {
            static int s_fovDiagWindow = 0;
            static int s_fovDiagLogged = 0;
            const bool fovOut = !(mFOV.position > 1.5f && mFOV.position < 175.0f);
            if (fovOut && s_fovDiagWindow == 0 && s_fovDiagLogged < 120)
                s_fovDiagWindow = 30;
            if (s_fovDiagWindow > 0 && s_fovDiagLogged < 120) {
                --s_fovDiagWindow;
                ++s_fovDiagLogged;
                spdlog::warn(
                    "[FOVDIAG] mFOV pos={:.2f} vel={:+.1f} tgt={:.2f} | omegaFov={:.3f} "
                    "dt={:.4f} cap={:.0f} | profFov={:.2f}",
                    mFOV.position, mFOV.velocity, mFOV.target, omegaFov, dt,
                    m_chanCapFov, targetProfile.fov);
            }
        }

        // Guard the FOV channel against a downward overshoot past zero. The
        // Â±velocity clamp in stepMotion suppresses the spring's damping during
        // a large retarget, so a big high->low FOV step can drive the position
        // negative â€” and a negative worldFOV renders the world inside-out (see
        // ApplyFOV). Floor the channel and kill any remaining downward velocity
        // so it recovers FROM the floor instead of crawling back from deep
        // negative over the (possibly very soft) FOV omega. The floor sits well
        // below any real FOV (50-130), so this only ever engages on a genuine
        // overshoot.
        constexpr float kFovChannelFloor = 1.0f;
        if (mFOV.position < kFovChannelFloor) {
            mFOV.position = kFovChannelFloor;
            if (mFOV.velocity < 0.0f) mFOV.velocity = 0.0f;
        }

        currentProfile.sideOffset  = mSide.position;
        currentProfile.height      = mHeight.position;
        currentProfile.zoom        = mZoom.position;
        currentProfile.fov         = mFOV.position;
        currentProfile.pitchOffset = mPitch.position;
        velSideOffset = velHeight = velZoom = velFOV = velPitchOffset = 0.0f;

        // [DLGSNAP] â€” per-frame step meter for the dialogue channels. Two
        // structural fixes (Hermite re-arm, proximity pace) have shipped
        // against "dialogue still has snaps"; the next report must NAME the
        // channel instead of us theorising a third time. Logs any channel
        // moving further in ONE frame than ~3x its plausible blended rate,
        // with the blend state attached. Capped generously â€” a capped
        // diagnostic that can exhaust itself on noise goes mute exactly when
        // needed (the [FREEZE] lesson), so the threshold does the filtering,
        // not the cap.
        {
            static float sPrevSide = 0.0f, sPrevHeight = 0.0f, sPrevZoom = 0.0f;
            static float sPrevFov = 0.0f, sPrevPitch = 0.0f, sPrevRot = 0.0f;
            static bool  sPrevValid = false;
            static int   sSnapLogs  = 0;
            if (dialogueActiveForProfile && sPrevValid && dt > 0.0001f && sSnapLogs < 200) {
                const float stepScale = (std::max)(1.0f, dt * 60.0f);
                const float dSide  = std::abs(mSide.position   - sPrevSide);
                const float dHght  = std::abs(mHeight.position - sPrevHeight);
                const float dZoom  = std::abs(mZoom.position   - sPrevZoom);
                const float dFov   = std::abs(mFOV.position    - sPrevFov);
                const float dPitch = std::abs(mPitch.position  - sPrevPitch);
                const float dRot   = std::abs(cachedRotation   - sPrevRot);
                if (dSide  > 4.0f * stepScale || dHght > 4.0f * stepScale ||
                    dZoom  > 3.0f * stepScale || dFov  > 3.0f * stepScale ||
                    dPitch > 2.5f * stepScale || dRot  > 3.0f * stepScale) {
                    ++sSnapLogs;
                    spdlog::debug(
                        "[DLGSNAP] step side={:.2f} h={:.2f} zoom={:.2f} fov={:.2f} "
                        "pitch={:.2f} rot={:.2f} | blend={} elapsed={:.2f} pace={:.2f} dt={:.4f}",
                        dSide, dHght, dZoom, dFov, dPitch, dRot,
                        m_dlgChanBlend.active ? 1 : 0, m_dlgChanBlend.elapsed,
                        settings.dlgPaceMul, dt);
                }
            }
            sPrevSide  = mSide.position;
            sPrevHeight = mHeight.position;
            sPrevZoom  = mZoom.position;
            sPrevFov   = mFOV.position;
            sPrevPitch = mPitch.position;
            sPrevRot   = cachedRotation;
            sPrevValid = dialogueActiveForProfile;
        }

        if (m_diagDlgEdge.pending) {
            m_diagDlgEdge.sidePosAfter        = mSide.position;
            m_diagDlgEdge.sideTarget          = mSide.target;
            m_diagDlgEdge.sideVelAfter        = mSide.velocity;
            m_diagDlgEdge.heightPosAfter      = mHeight.position;
            m_diagDlgEdge.heightTarget        = mHeight.target;
            m_diagDlgEdge.heightVelAfter      = mHeight.velocity;
            m_diagDlgEdge.zoomPosAfter        = mZoom.position;
            m_diagDlgEdge.zoomTargetAfter     = mZoom.target;
            m_diagDlgEdge.zoomVelAfter        = mZoom.velocity;
            m_diagDlgEdge.fovPosAfter         = mFOV.position;
            m_diagDlgEdge.fovTarget           = mFOV.target;
            m_diagDlgEdge.fovVelAfter         = mFOV.velocity;
            m_diagDlgEdge.pitchPosAfter       = mPitch.position;
            m_diagDlgEdge.pitchTarget         = mPitch.target;
            m_diagDlgEdge.pitchVelAfter       = mPitch.velocity;
            m_diagDlgEdge.dt                  = dt;
            m_diagDlgEdge.lockstepActiveAfter = m_dialogueEntryLockstepActive;
        }

        // Per-channel arrival sampler. Emits one log per milestone after
        // dialogue entry showing |delta to target| + velocity for every
        // channel. Lets us see whether one channel arrives "dead last."
        // At most 5 logs per dialogue â€” not hot-path.
        static constexpr float kDiagDlgSampleTimes[] = {0.05f, 0.10f, 0.20f, 0.40f, 0.80f};
        static constexpr int   kDiagDlgSampleCount   = sizeof(kDiagDlgSampleTimes) / sizeof(kDiagDlgSampleTimes[0]);
        if (m_diagDlgSampleActive && dialogueActiveForProfile) {
            m_diagDlgSampleElapsed += dt;
            while (m_diagDlgSampleIdx < kDiagDlgSampleCount &&
                   m_diagDlgSampleElapsed >= kDiagDlgSampleTimes[m_diagDlgSampleIdx]) {
                const float tMs = kDiagDlgSampleTimes[m_diagDlgSampleIdx];
                spdlog::debug(
                    "[DLG-SAMPLE t={:.2f}s] "
                    "side d={:+6.2f} v={:+6.1f} | height d={:+6.2f} v={:+6.1f} | "
                    "zoom d={:+6.2f} v={:+6.1f} | fov d={:+6.2f} v={:+6.1f} | "
                    "pitch d={:+6.2f} v={:+6.1f} | lockstep={}",
                    tMs,
                    mSide.target   - mSide.position,   mSide.velocity,
                    mHeight.target - mHeight.position, mHeight.velocity,
                    mZoom.target   - mZoom.position,   mZoom.velocity,
                    mFOV.target    - mFOV.position,    mFOV.velocity,
                    mPitch.target  - mPitch.position,  mPitch.velocity,
                    m_dialogueEntryLockstepActive ? 1 : 0);
                ++m_diagDlgSampleIdx;
            }
            if (m_diagDlgSampleIdx >= kDiagDlgSampleCount) {
                m_diagDlgSampleActive = false;
            }
        }

        // Rotation: shortest-arc unwrap so -179 -> +179 takes the short way.
        {
            float rotTarget = targetProfile.rotation;
            float diff      = rotTarget - mRotation.position;
            while (diff >  180.0f) diff -= 360.0f;
            while (diff < -180.0f) diff += 360.0f;
            rotTarget = mRotation.position + diff;
            const float rotPosBefore = mRotation.position;
            const float rotVelBefore = mRotation.velocity;
            const float rotTgtBefore = mRotation.target;
            stepMotion(mRotation, "rot", rotTarget, omegaRot, kMaxChannelSpeed, kRefSpanRot);
            while (mRotation.position >  180.0f) mRotation.position -= 360.0f;
            while (mRotation.position < -180.0f) mRotation.position += 360.0f;
            currentProfile.rotation = mRotation.position;
            velRotation = 0.0f;

            // ----- [ROTRET] THE CAMERA ROTATION CHANNEL, LEAVING A SHOUT -----
            //
            // "Rotation seems to be overshooting when it returns back to
            // sheathed from my dragon aspect settings" — THIRD PERSON (user,
            // 2026-09-03). This is the camera's own Rotation channel, not the
            // noise's Rotation Shake; both were candidates and both are now
            // instrumented on the same run (see [SHOUT3P] in the noise
            // controller for the other one).
            //
            // WHAT COULD OVERSHOOT HERE. stepMotion is the exact closed-form
            // critically damped spring, which cannot cross its target from
            // rest — but it is preceded by a RETARGET IMPULSE
            // (m.velocity += span * omega * spanOmegaScale * kRetargetImpulse)
            // whose monotonicity argument holds only while the incoming
            // velocity is ~0. It is gated on |velocity| < kSnapVelocityThr for
            // exactly that reason, and the shout return is the case most likely
            // to break the assumption: the return can be armed while the
            // channel is still in flight toward the shout's rotation, and
            // m_shoutReturnActive then swaps the omega mid-move to the SHOUT's
            // transitionRotation. Two changes on one frame, one of them to the
            // stiffness itself.
            //
            // WHAT THE LOG DECIDES: `pos` crossing `tgt` and coming back is the
            // overshoot, measured, and `vel`/`omega` on that frame say which of
            // the two did it — a velocity spike names the impulse, a stiffness
            // step names the shout-return override. If pos approaches tgt
            // monotonically, this channel is exonerated and [SHOUT3P] carries
            // the verdict instead.
            {
                static bool  sPrevShout = false;
                static float sSince     = -1.0f;
                static int   sLogs      = 0;
                const bool shoutNow =
                    StateResolver::GetSingleton().GetSubState() == CameraSubState::Shout;
                if (sPrevShout && !shoutNow) sSince = 0.0f;
                sPrevShout = shoutNow;
                if (sSince >= 0.0f) {
                    sSince += dt;
                    if (sSince > 2.5f) {
                        sSince = -1.0f;
                    } else if (sLogs < 400) {
                        ++sLogs;
                        spdlog::debug("[ROTRET] t={:.0f}ms tgt={:.3f} pos={:.3f} vel={:.3f} "
                                     "(pre: tgt={:.3f} pos={:.3f} vel={:.3f}) omega={:.2f} "
                                     "scale={:.2f} err={:.3f} shoutRet={} shoutOmega={:.2f}",
                                     sSince * 1000.0f,
                                     mRotation.target, mRotation.position, mRotation.velocity,
                                     rotTgtBefore, rotPosBefore, rotVelBefore,
                                     omegaRot, mRotation.spanOmegaScale,
                                     mRotation.position - mRotation.target,
                                     m_shoutReturnActive ? 1 : 0, m_shoutReturnRotation);
                    }
                }
            }
        }

        // Spring helper for the lock-aim yaw spring (separate from camera
        // profile transitions â€” this is target-lock yaw, not channels).
        // EXACT (closed-form) integrator, NOT explicit Euler: during a target
        // SWITCH this spring is stiffened to lockAimOmega = 4/swDur, and the
        // rework made swDur distance-driven (floored at 0.08s â†’ omega up to
        // ~50). Explicit CriticalDampedSpring is only stable while omega*dt <
        // ~0.83; at omega 50 and dt up to 0.05 that hits omega*dt ~ 2.5, so the
        // damping term sign-flipped and currentLockAimYaw DIVERGED â€” [SWDIAG]
        // caught lAim ringing -43â†’+22â†’-86â†’+84â†’+213â†’â€¦ and running away to 400Â°+,
        // spinning the camera on every switch (the "stutter"). The exact form
        // is unconditionally stable at any omega/dt with identical settle feel,
        // so both this and the side-scale spring (which shares lockAimOmega)
        // stay put. See Spring.h â€” this is the failure mode it documents.
        auto springStep = [dt](float& cur, float& vel, float target, float omega) {
            CriticalDampedSpringExact(cur, vel, target, omega, dt);
        };
        // Lock-aim retains its existing feel via hardcoded constants. The
        // base (4.0 = former baseSpeed 0.5 Ã— 8) and the 1.5 multiplier
        // are baked in so the lock-aim spring is independent of the
        // Transitions menu sliders.
        constexpr float lockAimOmegaBase = 4.0f;
        constexpr float kLockAimMul      = 1.5f;
        // During a target SWITCH, drive the For-Honor framing springs (lock-aim
        // yaw + side-scale, below) at the SWITCH stiffness instead of the fixed
        // 6.0, so they settle in lockstep with the main yaw swing. Otherwise they
        // trail it (fixed ~0.67s settle vs the switch time) as a separate "fit
        // into slot" motion at the end. Held constant until ~2*swDur (fully
        // settled) so reverting to 6.0 is a no-op (no late adjustment).
        float lockAimOmega = lockAimOmegaBase * kLockAimMul;
        if (m_lockSwitching) {
            const float swDur = m_lockSwitchDur > 0.08f ? m_lockSwitchDur : 0.08f;
            lockAimOmega = 4.0f / swDur;
            m_lockSwitchT += dt;
            if (m_lockSwitchT >= 2.0f * swDur) m_lockSwitching = false;
        }

        // For-Honor framing: the side offset eases toward 0 as the enemy closes
        // so the enemy stays centered without an extreme camera swing. Computed
        // in the lock-aim block below and reused to shrink the camera POSITION
        // offset (effectiveSide) by the same amount, so framing and aim agree.
        // 1.0 when not locked / far away.
        float lockSideScale = 1.0f;

        // Target-lock biased-aim yaw â€” spring toward the geometric target
        // when locked, spring back to 0 when not. Kept here (not in
        // HookManager) so entering/exiting lock is smoothed by the same
        // spring system that handles every other profile transition.
        {
            // block runs every frame in both lock states, so the release can't
            // leak when the lock drops.
            float desiredLockAimYaw = 0.0f;
            if (resolver.IsTargetLocked()) {
                auto& tdm = TDMIntegration::GetSingleton();
                if (auto h = tdm.GetCurrentTarget()) {
                    if (auto tp = h.get()) {
                        const auto& pp    = player->GetPosition();
                        const auto  tpos  = tp->GetPosition();
                        const float dx   = tpos.x - pp.x;
                        const float dy   = tpos.y - pp.y;
                        const float d    = std::sqrt(dx * dx + dy * dy);
                        // AIM BIAS: global, unless the composed profile carries
                        // an override (2026-09-07). targetProfile starts as a
                        // copy of the resolved ENTRY and has already had the
                        // enemy's set channels spliced over it by
                        // SpliceEnemyTransitionOverride, so this single read IS
                        // the enemy > entry > global order. The per-enemy lookup
                        // that used to live here — a second resolution path that
                        // could disagree with the field splice about which layer
                        // won — is gone with it.
                        float biasValue = settings.targetLockAimBias;
                        if (targetProfile.transitionSetAimBias)
                            biasValue = targetProfile.transitionAimBias;
                        // NEGATIVE bias is meaningful and is why the range
                        // opens below zero: it drives the lock-aim the other
                        // way, putting the target on the OPPOSITE side of
                        // centre from your Side Offset. Without it a swapped
                        // shoulder had no matching aim, which is what made
                        // shoulder swap unusable while locked on.
                        const float bias = std::clamp(biasValue, -1.5f, 1.5f);
                        // Aspect-correct the side-offset for the aim solve so the
                        // lock-on yaw matches the visual offset on any aspect.
                        const float aspectSide = targetProfile.sideOffset * GetAspectFactor();

                        // FOR-HONOR FRAMING. The yaw that puts the enemy dead
                        // center is asin(side / d) -- it depends ONLY on the side
                        // offset and the enemy distance, NOT on zoom (the camera
                        // looks at the laterally-offset focus point, so zoom just
                        // slides it along the same sight line). The old
                        // atan2(side, back+d) under-rotated badly up close, leaving
                        // the enemy jammed off to one side (measured: needed ~70
                        // deg, produced ~15). But the TRUE centering angle explodes when
                        // the enemy gets as close as the offset is wide (asin -> 90
                        // deg = the top-down swing). So CAP the centering yaw and
                        // ease the side offset down to hold center within that cap
                        // -- exactly how For Honor slides behind you at knife range.
                        // lockSideScale is reused below to shrink the camera
                        // POSITION offset by the same factor, so framing and aim
                        // always agree.
                        constexpr float kLockMaxAimYaw = 0.384f;  // ~22 deg swing cap
                        const float sinMax   = std::sin(kLockMaxAimYaw);
                        const float fullSide = std::abs(aspectSide);
                        const float aimSideScale = (fullSide > 0.001f && d > 1.0f)
                            ? std::clamp((d * sinMax) / fullSide, 0.0f, 1.0f)
                            : 1.0f;
                        const float effSide   = aspectSide * aimSideScale;
                        // The camera's OWN shrink fades out with the centering
                        // it exists to serve. The shrink is the cost of holding
                        // the enemy centred at knife range â€” at Aim Bias 0 the
                        // camera is not centring anything, so charging that
                        // cost just collapsed the shoulder offset in melee for
                        // nothing (the enemy is meant to STAY off to the side
                        // there; that is what a low bias asks for).
                        //
                        // Only the CAMERA side is faded. The aim solve above
                        // keeps the unfaded aimSideScale, so centerYaw stays
                        // bounded by the 22-degree cap exactly as before â€”
                        // feeding a faded scale into the asin would let the
                        // ratio climb past sinMax and reopen the top-down
                        // swing the cap exists to prevent. Bias 1 (the
                        // default) is bit-for-bit unchanged.
                        const float biasMag = std::clamp(std::abs(bias), 0.0f, 1.0f);
                        lockSideScale = 1.0f + (aimSideScale - 1.0f) * biasMag;
                        // Guard the divide. When the locked target is basically
                        // on top of the player (d -> 0: an enemy clipping into
                        // you, a grab/killmove, or the frame a target dies or
                        // teleports), -effSide/d is 0/0 = NaN â€” and std::clamp
                        // does NOT scrub NaN, so it would flow into the spring
                        // below, latch forever, and corrupt the camera yaw every
                        // frame (the "world turns inside out" bug). Hold the aim
                        // (desiredLockAimYaw stays 0) until there's real
                        // separation; matches the lockSideScale d>1 guard above.
                        if (d > 1.0f) {
                            const float centerYaw = std::asin(std::clamp(-effSide / d, -1.0f, 1.0f));
                            // Aim Bias scales the centering: 1.0 = dead center,
                            // <1 leaves the enemy toward your side-offset side,
                            // >1 pushes it past center toward the opposite side.
                            desiredLockAimYaw = bias * centerYaw;
                        }
                    }
                }
            }
            springStep(currentLockAimYaw, velLockAimYaw, desiredLockAimYaw, lockAimOmega);
            // Defense-in-depth: this spring's output is written into the engine
            // camera yaw every frame, so a single non-finite value would latch
            // and corrupt the view permanently. If anything ever drives it
            // non-finite, reset it to neutral instead of propagating the NaN.
            if (!std::isfinite(currentLockAimYaw) || !std::isfinite(velLockAimYaw)) {
                currentLockAimYaw = 0.0f;
                velLockAimYaw     = 0.0f;
            }
        }

        // Re-baseline zoom on state change
        static CameraState lastZoomState = static_cast<CameraState>(-1);
        if (resolver.GetState() != lastZoomState) {
            lastZoomState = resolver.GetState();
            zoomBaseInitialized = false;
            lastZoom = 0.0f;
        }


        // Apply aspect correction once at the consumption boundary. All
        // downstream writes (dialogue posOffsetExpected.x, normal
        // posOffsetActual.x/Expected.x, adaptive blend) inherit it.
        const float aspectFactor = GetAspectFactor();
        float effectiveSide   = currentProfile.sideOffset * aspectFactor;
        // Match the lock-aim's For-Honor side ease: shrink the camera's lateral
        // offset by the same factor the aim used, so the framing the aim is
        // solving for is the framing the camera actually produces. Without this,
        // the aim would center for a reduced offset while the camera still sat at
        // the full offset -> enemy off-center again.
        //
        // Apply it through an EASED scale (target = lockSideScale while locked,
        // 1.0 otherwise â€” lockSideScale is already 1.0 when not locked) stepped
        // by the SAME spring as the lock-aim yaw. The previous code gated the
        // multiply on IsTargetLocked, so on lock-off the lateral offset jumped
        // from the shrunken value back to full in one frame = a visible sideways
        // jerk. Easing it glides back in lockstep with currentLockAimYaw.
        springStep(easedLockSideScale, velLockSideScale, lockSideScale, lockAimOmega);
        if (!std::isfinite(easedLockSideScale)) {
            easedLockSideScale = 1.0f;
            velLockSideScale   = 0.0f;
        }
        effectiveSide *= easedLockSideScale;
        float effectiveHeight = currentProfile.height;
        float effectiveZoom   = currentProfile.zoom;
        float effectiveFOV    = currentProfile.fov;
        float effectiveRot    = currentProfile.rotation;
        float effectivePitch  = currentProfile.pitchOffset;


        // Store effective rotation for HookManager to read next frame
        cachedRotation = effectiveRot;


        // Always maintain FOV regardless of menus
        ApplyFOV(a_camera, effectiveFOV);

        // Pitch is applied via HookedGetRotation every frame â€” keep it up
        // to date during dialogue too so the pitch slider drives the
        // camera while the Dialogue Menu is open.
        //
        // HEIGHT PIVOT (built 2026-08-20, REVERTED 2026-08-21) â€” do not rebuild.
        //
        // It added atan2(-height, measuredRadius) to the pitch offset so that
        // lowering the camera kept the player framed. The arithmetic was right
        // and the framing argument was right, but the RESULT was wrong to use:
        // it made the camera's angle depend on the Height slider, so every
        // entry with a different Height aimed somewhere different, and no
        // amount of tuning Pitch Offset could get a consistent look back.
        // Height must translate and ONLY translate; pitch belongs to Pitch
        // Offset alone. If the framing problem is revisited, it has to be
        // solved by moving the camera (an anchor / orbit change), never by
        // secretly rotating it.
        currentPitchOffset = effectivePitch;

        // Dialogue camera position channel. During the Dialogue Menu the
        // engine composes the final camera position from the ThirdPersonState
        // "over-shoulder" fields at offsets 0x5C/0x60/0x64 â€” exposed here
        // as posOffsetExpected.x/y/z (x=lateral, y=forward/back, z=vertical).
        // This is the same channel ACC/IACC write to. posOffsetActual and
        // the zoomOffset fields don't survive dialogue's engine path, so
        // we skip them and route zoom through the .y component (CombatAddY).
        // Track dialogue write edges so we can restore the engine's intended
        // posOffset.y when leaving dialogue. The engine writes that field
        // event-driven (only on state changes, not every frame), so once
        // we override it during dialogue, our value sticks until something
        // else overwrites â€” that's the "dialogue zoom stays after exit and
        // stacks with the categories zoom" bug.
        static bool  sLastWroteDialogueY = false;
        static float sSavedEngineY       = 0.0f;
        // Zoom-offset lock: the engine resets targetZoomOffset on POV
        // toggles. Our dialogue distance is driven by posOffsetExpected.y
        // and stacks ON TOP of whatever zoomOffset the engine has â€”
        // toggling POV mid-dialogue then back ends up with a different
        // (closer) baseline. Capture the baseline on the open edge and
        // re-write it every frame during dialogue so POV cycles can't
        // drift it.
        static float sSavedEngineTargetZoom  = 0.0f;
        static float sSavedEngineCurrentZoom = 0.0f;
        // Pre-dialogue posOffsetExpected values, captured on the open
        // edge alongside sSavedEngineY. Used as the ramp-start values
        // for the lockstep write so the camera transitions smoothly
        // from the engine's baseline to dialogue framing instead of
        // jumping (~38 units measured) in one frame.
        static float sSavedEngineX = 0.0f;
        static float sSavedEngineZ = 0.0f;
        bool wroteDialogueY = false;

        if (menuBlocking) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen("Dialogue Menu") && settings.dialogueEnabled && dialogueActiveForProfile) {
                constexpr float kGameUnitsPerSliderUnit = Defaults::GameUnitsPerSliderZoom;
                const float dlgY = -(effectiveZoom * kGameUnitsPerSliderUnit);

                // Capture the engine's intended posOffset.y AND the zoom
                // baseline ONLY on the open edge (first dialogue frame).
                // The engine writes posOffsetExpected.y event-driven (state
                // changes), not per frame; capturing on frame 2+ would
                // corrupt sSavedEngineY into our own dlgY. Same logic for
                // the zoom â€” capture only when we're about to start
                // overriding so we lock the user's pre-dialogue baseline.
                if (!sLastWroteDialogueY) {
                    sSavedEngineY            = tps->posOffsetExpected.y;
                    sSavedEngineX            = tps->posOffsetExpected.x;
                    sSavedEngineZ            = tps->posOffsetExpected.z;
                    sSavedEngineTargetZoom   = tps->targetZoomOffset;
                    sSavedEngineCurrentZoom  = tps->currentZoomOffset;
                    // POV-switch entry: the live engine y/zoom are mid-POV-
                    // transition (transient). Substitute the last steady
                    // gameplay-3p baseline so the exit blend (which eases
                    // posOffset.y back to sSavedEngineY) and the per-frame zoom
                    // lock both restore to the correct resting value instead of
                    // snapping. X/Z only seed the entry ramp (the switch-in feel,
                    // not the exit), so leave them on the live value.
                    // Pin the engine zoom to the GAMEPLAY (categories) zoom, NOT the
                    // live engine value. Done UNCONDITIONALLY: dlgPovSwitchEntry does
                    // not fire for a 1p->3p switch (the dialogue's rising edge already
                    // happened in 1p, so the 3p capture sees it as non-rising â€”
                    // confirmed [DLG-CAP] povSwitch=0), so gating on it missed exactly
                    // the case that needs fixing. For a normal entry the live value
                    // already IS the gameplay zoom, so this is a no-op; for a 1p->3p
                    // entry the live value is the transient post-switch zoom (-0.200),
                    // and pinning to it snaps the engine zoom to gameplay on EXIT
                    // (~49u dolly). The categories profile is resolved every frame
                    // (line ~1721) and is the true gameplay zoom the exit transitions
                    // to, so pinning to it makes the exit continuous at any blend
                    // speed (no special-case ease needed).
                    if (sLastCategoriesProfile) {
                        // -0.2f literal, NOT the member zoomBase: ApplyZoom (which
                        // sets zoomBase = -0.2) runs LATER in this function and, on a
                        // 1p->3p entry, has never run in 3p yet â€” so zoomBase is still
                        // 0 here, which gave a wrong gpZoom (confirmed [DLG-CAP]
                        // savedCur=0.110 instead of -0.090). -0.2 is the canonical
                        // vanilla fMinCurrentZoom ApplyZoom hardcodes.
                        // Zoom override REMOVED: pinning the engine zoom to the
                        // gameplay (categories) zoom made the parent state's zoom leak
                        // into the dialogue framing. Keep the captured (close) zoom;
                        // the engine zoom's EXIT transition is eased in the render hook
                        // instead ([DLG-EXIT-ZOOM]) so it is smooth at any blend speed.
                        (void)sLastCategoriesProfile;
                    }
                    if (dlgPovSwitchEntry && m_gameplay3pBaselineValid) {
                        sSavedEngineY           = m_gameplay3pPosY;
                        sSavedEngineTargetZoom  = m_gameplay3pTargetZoom;
                        sSavedEngineCurrentZoom = m_gameplay3pCurrentZoom;
                    }
                }

                // Re-write the captured zoom every frame. Engine POV
                // toggles touch these fields, but since we restore each
                // frame, the dialogue camera's distance baseline is
                // preserved across 3p â†” 1p switches.
                tps->targetZoomOffset  = sSavedEngineTargetZoom;
                tps->currentZoomOffset = sSavedEngineCurrentZoom;

                // Pitch compensation: the engine rotates posOffset by the
                // player's pitch (data.angle.x) when composing camera world
                // position. So writing posOffset.y = -200 in a pitched frame
                // doesn't pull the camera straight back â€” it pulls back AND
                // up/down. Symptom: opening dialogue while looking up dropped
                // the camera below the NPC ("looking at the sky" â€” really:
                // looking up at the face from below); looking down did the
                // opposite. Pre-rotate (y, z) by -pitch so the world-frame
                // contribution stays purely horizontal-back regardless of
                // what the face-lock blend has data.angle.x at this frame.
                // Compute raw comp{Y,Z} from current data.angle.x. Then
                // exponentially smooth them with a fixed time constant
                // before they feed the position-offset write path.
                //
                // Why exp smoothing on compY/compZ and not on data.angle.x:
                //   player->data.angle.x is driven by the engine's anim
                //   graph (~10 Hz ticks) and the 3p face-lock cubic blend
                //   in HookManager. Per-frame pitch jumps of 0.020-0.025
                //   rad at anim-tick boundaries multiply by effectiveHeight
                //   (~13-15) inside the sin() to produce compY drops of
                //   0.30-0.50 units in a single frame. Those drops
                //   propagate to posOffsetExpected.y â†’ posOffsetActual.y
                //   (lockstep write) â†’ visible camera-position REVERSAL
                //   each anim tick. That's the dialogue-entry stutter the
                //   user reported 2026-05-27.
                //
                // Frame-rate independence: alpha = 1 - exp(-dt/tau). The
                // time constant tau is fixed in seconds, so the smoothing
                // tracks wall-clock time identically at 30/60/144 fps.
                //   - At 30 fps (dt=0.033): alpha â‰ˆ 0.379 (38% per frame)
                //   - At 60 fps (dt=0.017): alpha â‰ˆ 0.212 (21% per frame)
                //   - At 144 fps (dt=0.007): alpha â‰ˆ 0.095 (9.5% per frame)
                // 70ms tau = anim-tick jumps spread over 3-5 render frames,
                // legitimate face-tracking corrections still complete well
                // inside the dialogue entry blend window (~700 ms).
                //
                // Reset state on dlgEdge so each new dialogue captures a
                // clean baseline (otherwise prior dialogue's smoothed
                // state would carry over into the entry frame).
                float cp = 1.0f, sp = 0.0f;
                if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                    const float pitch = p->data.angle.x;
                    cp = std::cos(pitch);
                    sp = std::sin(pitch);
                }

                // ---- "Look At The Player" reverse shot ----------------
                // The dialogue camera normally orbits the PLAYER and looks at
                // the speaker. The reverse shot is the exact mirror: the
                // camera orbits the SPEAKER and looks at the player, with the
                // same preset values, so the speaker becomes the
                // over-the-shoulder foreground and you are the one framed.
                //
                // The engine composes this camera from the player, so the
                // mirror is expressed as an added offset rather than a
                // different anchor. It has to be the FULL vector from you to
                // them, resolved into the camera's own axes â€” not a pull-back
                // along the view direction.
                //
                // That distinction is the whole correctness of this. Only when
                // the added offset is exactly (speaker - player), whatever the
                // yaw happens to be, does
                //     player + R(yaw)*(preset + extra)  ==  speaker + R(yaw)*preset
                // hold. Treating it as "pull back by the distance between us"
                // is only equivalent at ONE yaw â€” the exact conversational
                // axis â€” so the moment the aim solves anywhere else (which it
                // must, because the preset has a side offset) the camera stops
                // orbiting the speaker and the framing drifts off the mirror.
                // That was the earlier inaccuracy: the subject sat off-centre
                // by the side-offset angle instead of being framed the way the
                // forward shot frames the NPC.
                //
                // With the offset correct for any yaw, the aim can be solved
                // the ordinary way â€” point the camera at the player's head
                // from wherever it actually is â€” and it lands on exactly the
                // framing the forward shot produces, because it is the same
                // solve with the two subjects swapped. HookManager does that
                // half; see the aim target there.
                //
                // The transition between the two is a rigid ROTATION of the
                // whole rig about the midpoint between the two speakers, not a
                // slide. A slide would drag the camera straight through the
                // player on its way across. Rotating sweeps it around the
                // outside instead, and keeps the camera and its aim target a
                // full conversation-width apart the entire way, so the aim
                // never degenerates mid-move.
                float dlgYEff   = dlgY;
                float heightEff = effectiveHeight;
                float sideExtra = 0.0f;
                {
                    const bool wantReverse = DialogueLookPicker::ShouldLookAtPlayer();
                    {
                        // The sweep is a half-turn, so it needs long enough
                        // that the aim spring can keep up with it; otherwise
                        // the rig arrives before the view has finished turning.
                        // Floored rather than taken straight from the slider
                        // for that reason.
                        constexpr float kMinReverseSweep = 1.0f;
                        const float blendSecs = (std::max)(kMinReverseSweep,
                            SettingsManager::DialogueBlendDuration(
                                (std::max)(0.05f, settings.dialogueMulPosition)));
                        const float tau   = (std::max)(0.05f, blendSecs / 3.0f);
                        const float alpha = 1.0f - std::exp(-dt / tau);
                        m_dlgReverseBlend += ((wantReverse ? 1.0f : 0.0f) - m_dlgReverseBlend) * alpha;
                        if (!wantReverse && m_dlgReverseBlend < 0.0005f) m_dlgReverseBlend = 0.0f;
                        if ( wantReverse && m_dlgReverseBlend > 0.9995f) m_dlgReverseBlend = 1.0f;
                    }

                    auto* spk = DialogueLookPicker::GetActiveSpeakerRef();
                    auto* p   = RE::PlayerCharacter::GetSingleton();
                    if (spk && p) {
                        const auto pPos   = p->GetPosition();
                        const auto spkPos = spk->GetPosition();
                        const float mx = (pPos.x + spkPos.x) * 0.5f;
                        const float my = (pPos.y + spkPos.y) * 0.5f;
                        // Camera-yaw basis. posOffset is applied in the
                        // camera-yaw frame, so every world displacement below
                        // has to be projected onto it â€” this is the step that
                        // makes the identities hold at every yaw.
                        const float yaw = p->data.angle.z + tps->freeRotation.x;
                        const float fx = std::sin(yaw), fy = std::cos(yaw);
                        const float rx = fy,            ry = -fx;

                        // Dialogue ROTATION orbits the MIDPOINT of the
                        // conversation, not the player. The rotation channel
                        // itself still orbits the rig about the player (that
                        // is what freeRotation does); re-pivoting to M is a
                        // pure translation: C' - C = (R-I)(P-M), independent
                        // of where the camera is. Rotating about the player
                        // alone swings the whole conversation off-frame,
                        // which is why the slider read as "just the player".
                        const float rotRad = cachedRotation * (3.14159265f / 180.0f);
                        if (std::abs(rotRad) > 0.0005f) {
                            const float ux = pPos.x - mx, uy = pPos.y - my;
                            const float cR = std::cos(rotRad), sR = std::sin(rotRad);
                            // Same handedness as the yaw convention (fwd =
                            // (sin, cos)): x' = x c + y s, y' = -x s + y c.
                            const float exr = (ux * cR + uy * sR) - ux;
                            const float eyr = (-ux * sR + uy * cR) - uy;
                            sideExtra += exr * rx + eyr * ry;
                            dlgYEff   += exr * fx + eyr * fy;
                        }

                        if (m_dlgReverseBlend > 0.0001f) {
                            // Anchor sweeps from the player round to the
                            // speaker along the arc through the midpoint.
                            const float phi = m_dlgReverseBlend * 3.14159265f;
                            const float cphi = std::cos(phi), sphi = std::sin(phi);
                            const float vx = pPos.x - mx, vy = pPos.y - my;
                            const float ax = mx + vx * cphi - vy * sphi;
                            const float ay = my + vx * sphi + vy * cphi;

                            // Vertical rides straight across rather than
                            // around: head heights, so a mounted or very tall
                            // speaker frames you from where their eyes are
                            // instead of from their feet.
                            float pHeadZ = pPos.z + 120.0f;
                            float sHeadZ = spkPos.z + 120.0f;
                            if (auto* pr = p->Get3D())
                                if (auto* ph = pr->GetObjectByName("NPC Head [Head]"))
                                    pHeadZ = ph->world.translate.z;
                            if (auto* sr3 = spk->Get3D())
                                if (auto* sh = sr3->GetObjectByName("NPC Head [Head]"))
                                    sHeadZ = sh->world.translate.z;

                            const float ex = ax - pPos.x;
                            const float ey = ay - pPos.y;
                            sideExtra += ex * rx + ey * ry;
                            dlgYEff   += ex * fx + ey * fy;
                            heightEff += (sHeadZ - pHeadZ) * m_dlgReverseBlend;
                        }
                    }
                }

                // Both forward/vertical terms go in BEFORE the pitch
                // compensation, because they describe a world-frame
                // displacement â€” the same thing dlgY and effectiveHeight
                // describe â€” and the compensation is what converts that into
                // the local offset the engine wants. The lateral term is added
                // after, at the write, since pitch never rotates it.
                const float rawCompY = dlgYEff * cp - heightEff * sp;
                const float rawCompZ = dlgYEff * sp + heightEff * cp;
                static float sSmoothCompY = 0.0f;
                static float sSmoothCompZ = 0.0f;
                static bool  sSmoothCompInit = false;
                // 70 ms filter absorbing the ~10 Hz staircase in the anim
                // graph's pitch; without it that comes back as per-tick
                // position stutter. Seeded on the entry edge, then filters
                // every frame.
                const bool snapComp = dlgEdge || !sSmoothCompInit;
                if (snapComp) {
                    sSmoothCompY = rawCompY;
                    sSmoothCompZ = rawCompZ;
                    sSmoothCompInit = true;
                } else {
                    constexpr float kCompTau = 0.07f;   // 70 ms time constant
                    const float alpha = 1.0f - std::exp(-dt / kCompTau);
                    sSmoothCompY += alpha * (rawCompY - sSmoothCompY);
                    sSmoothCompZ += alpha * (rawCompZ - sSmoothCompZ);
                }
                const float compY = sSmoothCompY;
                const float compZ = sSmoothCompZ;

                // Entry BRIDGE: how far the engine's resting posOffset sits
                // from the dialogue-compensated value. See the write below for
                // why this is captured as an offset rather than re-eased as a
                // whole travel. Captured under the SAME condition as
                // sSavedEngineY above â€” the two are a matched pair, and taking
                // them from different frames would put the entry point
                // somewhere neither of them describes.
                static float sDlgBridgeY = 0.0f;
                static float sDlgBridgeZ = 0.0f;
                if (!sLastWroteDialogueY) {
                    sDlgBridgeY = sSavedEngineY - compY;
                    sDlgBridgeZ = sSavedEngineZ - compZ;
                }

                // Lockstep during the 0.22s entry-blend window so the
                // camera position lands in sync with the cubic face-
                // lock pitch (avoiding the engine's slow 2.7%/frame
                // lerp tail that causes the "down then up" entry feel).
                // After the window, write expected only â€” engine's
                // slow lerp absorbs per-frame face-lock pitch deltas
                // without amplifying them into visible steady-state
                // jitter (per reference_dialogue_posoffset_lockstep_
                // root_cause.md). Edge detection + 0.22s timer below.
                // Smoothstep-ramp the lockstep write from the engine's
                // pre-dialogue baseline to the dialogue compY values so
                // the camera transitions continuously instead of jumping
                // in one frame. Without this, posOffsetExpected.y was
                // observed to jump 0 -> -38.5 on the entry frame (logged
                // 2026-05-23), which the user perceived as a "jump zoom".
                // Outside the lockstep window we write the target values
                // directly; the spring on mZoom has already settled and
                // engine lerp absorbs steady-state face-lock deltas.
                // writeX/writeZ track effectiveSide/compZ directly. The
                // chanBlend above already smootherstep-blends mSide and
                // mHeight from spring-start â†’ dialogue target across the
                // same window, and the normal branch writes posOffsetExpected
                // .x/.z every gameplay frame from effectiveSide/effectiveHeight,
                // so sSavedEngineX/Z always equal those spring values at the
                // edge. Multiplying the chanBlend smootherstep by an additional
                // cubic-smoothstep lockstep ramp (writeX = lerp(sSavedEngineX,
                // effectiveSide(t), smoothstep(t))) produced a curve with only
                // ~25% travel at t=0.5 instead of 50% â€” visible as "side jumps
                // mid-blend" on far-side presets (e.g., side_offset = -45).
                //
                // Y and Z are different: both are pitch-compensated during
                // dialogue (compY = dlgY*cp - h*sp; compZ = dlgY*sp + h*cp),
                // a rotation that keeps the camera at a fixed world position
                // regardless of player pitch. The gameplay branch writes raw
                // posOffset.z = effectiveHeight (no pitch rotation) and the
                // engine writes posOffset.y event-driven, so NEITHER
                // sSavedEngineY nor sSavedEngineZ equals comp{Y,Z} at the
                // edge. Both must bridge from the saved engine value to the
                // dialogue comp value across the lockstep window, or the
                // camera snaps that delta in one frame. Z was previously
                // written as comp directly (writeZ = compZ): measured as a
                // ~9-unit world-Z drop on the gameplay->dialogue handoff
                // (the harsh entry, 2026-05-31 [DLG-HANDOFF] vs +#00). This
                // is the entry mirror of the exit-Z smoothstep already at
                // line ~2411. X is genuinely continuous (no pitch rotation),
                // so writeX = effectiveSide stays direct.
                // sideExtra is the lateral half of the reverse-shot anchor
                // sweep. Added here rather than folded into effectiveSide so
                // the entry lockstep below still ramps only the framing the
                // preset asked for.
                float writeX = effectiveSide + sideExtra;
                float writeY = compY;
                float writeZ = compZ;
                if (m_dialogueEntryLockstepActive && kDialogueEntryLockstepDuration > 0.0f) {
                    const float t = std::clamp(
                        m_dialogueEntryLockstepT / kDialogueEntryLockstepDuration, 0.0f, 1.0f);
                    // Quintic smootherstep â€” matches chanBlend at line 1665 and
                    // exit-Y at line 2411. Cubic smoothstep here was the last
                    // remaining curve mismatch in the position pipeline; with
                    // it changed, every dialogue axis (X/Y/Z entry, X/Y/Z exit,
                    // FOV, zoom, pitch) shares the SAME curve over the SAME
                    // window. Result: position is one coordinated motion.
                    const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                    // Decay the BRIDGE, don't re-ease the travel.
                    //
                    // lerp(saved, compY, s) looks right and is not: compY is
                    // built from mZoom/mHeight, which the channel blend above
                    // has ALREADY smootherstepped across this very window. Write
                    // compY(s) = B + s(C-B) and expand â€” lerp(A, compY(s), s)
                    // = A + s(B-A) + sÂ²(C-B). The framing travel (C-B), which is
                    // the large term, comes out eased TWICE. sÂ² is back-loaded:
                    // it has only 25% of the travel done at the halfway point,
                    // peaks around t=0.72, and then has to shed all of that
                    // velocity in the last quarter of the window â€” drift, rush,
                    // stop. X hit this exact wall (see the note above about
                    // "side jumps mid-blend") and was fixed by removing its
                    // second ease outright; Y and Z can't do that because they
                    // genuinely must bridge from the engine's resting value.
                    //
                    // So bridge the OFFSET and leave the travel alone: the
                    // constant (A-B) fades out over the window while compY
                    // carries its own single ease. Endpoints are identical to
                    // before â€” A at t=0, C at t=1 â€” and the middle is the
                    // smootherstep the rest of the pipeline is already on.
                    writeY = compY + sDlgBridgeY * (1.0f - s);
                    writeZ = compZ + sDlgBridgeZ * (1.0f - s);
                }

                // Note: a 60 u/s velocity cap on writeX/Y/Z used to live
                // here as a band-aid for a compY staircase. The staircase
                // was rooted in HookManager's DialogueNowSeconds() losing
                // float precision when subtracting two large since-epoch
                // values â€” fixed at the source by switching to double
                // (see comment on s_dialogueAimStartTime). The cap was
                // throttling legitimate channel-blend motion on long-
                // distance preset transitions and creating a velocity
                // discontinuity at cap-engage that read as "stutter or
                // two." sSmoothCompY's exponential smoothing (tau=70ms)
                // above provides the residual smoothing the cap was
                // meant to add.

                tps->posOffsetExpected.x = writeX;
                tps->posOffsetExpected.y = writeY;
                tps->posOffsetExpected.z = writeZ;
                // Lockstep-write posOffsetActual whenever EITHER the entry
                // lockstep ramp OR the channel blend is running. Without
                // the channel-blend half, mid-dialogue preset switches
                // updated only posOffsetExpected and let the engine's
                // slow lerp (~2.7%/frame, ~1.8s settle) catch up â€” the
                // camera visibly trailed the channel blend by ~1s. User
                // reported 2026-05-24: "first transition is way faster
                // than switching presets in dialogue."
                // posOffsetActual.x lock-step-writes EVERY dialogue frame.
                // The engine's internal lerp from Expected â†’ Actual runs
                // at ~2.7%/frame (~1.8s settle), so for dialogues shorter
                // than ~1.5s Actual.x never catches up to Expected.x. The
                // exit-frame normal-branch fall-through then writes
                // Actual.x = effectiveSide directly, snapping by the
                // Actual-Expected delta. The delta is invisible on
                // moderate side offsets but reads as a visible jump on
                // far-side presets (e.g., side_offset = -45). Locking
                // Actual.x to writeX every frame keeps them aligned at
                // the exit edge.
                // Y/Z stay gated to preserve the engine-lerp smoothing
                // of face-lock pitch jitter (per the original comment
                // above â€” Y/Z get pitch-compensated each frame, X doesn't).
                // Y and Z are now lock-stepped EVERY dialogue frame, exactly
                // like X, and the authority is never handed back.
                //
                // History: this was gated on "a blend is running". The frame it
                // went false, the engine's own Expectedâ†’Actual lerp (~2.7%
                // /frame) took over â€” roughly 66x more damping arriving in a
                // single frame. Nothing jumps, since the two agree at that
                // instant, but the rig goes from carrying every bit of
                // face-lock liveliness to almost none of it between one frame
                // and the next, which is what "a sudden lock into place at the
                // end of the transition" describes. Fading the handoff over
                // 0.45s only made it a slower lock; the handoff itself is the
                // artefact.
                //
                // The engine lerp was there to smooth per-tick face-lock pitch
                // jitter, but that job now belongs to sSmoothCompY/Z above (a
                // 70ms filter, seeded on entry) with the aim spring in
                // HookManager low-passing the pitch before it ever gets here.
                // Two smoothers in series, one of them ~1.8s, is what made the
                // settled shot feel embalmed. One smoother, one authority, no
                // handoff. X was moved to this model already â€” for the exit
                // edge rather than for feel â€” and Y/Z were simply left behind.
                tps->posOffsetActual.x = writeX;
                tps->posOffsetActual.y = writeY;
                tps->posOffsetActual.z = writeZ;
                wroteDialogueY = true;

                // Override player->data.angle.x with an exp-smoothed value.
                // The 3p face-lock in HookManager.cpp:1680 writes a smooth
                // cubic-blend value, but the engine's animation graph
                // ticks at ~10 Hz and clobbers data.angle.x with the
                // current pose pitch between face-lock and the camera-
                // matrix render â€” the camera VIEW direction then stair-
                // steps even though our position writes are smooth.
                //
                // Writing here at the very end of the dialogue branch is
                // later than the face-lock write AND later than any anim-
                // graph write that fires inside _originalThirdPersonUpdate.
                // The engine's camera-matrix computation downstream reads
                // this final value.
                //
                // Frame-rate independent: alpha = 1 - exp(-dt/tau).
                // 70 ms time constant matches the compY/compZ smoothing
                // so position and rotation arrive together. Reset on
                // dlgEdge so each new dialogue captures a clean baseline.
                if (player) {
                    const float rawAngX = player->data.angle.x;
                    static float sSmoothFinalAngX = 0.0f;
                    static bool  sSmoothFinalAngXInit = false;
                    if (dlgEdge || !sSmoothFinalAngXInit) {
                        sSmoothFinalAngX = rawAngX;
                        sSmoothFinalAngXInit = true;
                    } else {
                        constexpr float kAngXTau = 0.07f;
                        const float alpha = 1.0f - std::exp(-dt / kAngXTau);
                        sSmoothFinalAngX += alpha * (rawAngX - sSmoothFinalAngX);
                    }
                    player->data.angle.x = sSmoothFinalAngX;
                }

                // Per-frame capture for dialogue-entry stutter diagnosis.
                // Captured HERE so engine-side writes from this frame
                // (posOffsetExpected, posOffsetActual, FOV, zoom) are
                // visible alongside our channel-spring state. Buffered
                // in memory; flushed to spdlog ONCE when the window
                // expires (~0.3s after entry).
                if (m_diagDlgPerFrameActive) {
                    m_diagDlgPerFrameElapsed += dt;
                    if (m_diagDlgPerFrameCount < kDiagDlgPerFrameCap) {
                        auto& r = m_diagDlgPerFrameBuf[m_diagDlgPerFrameCount++];
                        // Zero the LATE-capture fields so a frame where no
                        // late hook fires is visually distinct (all zeros +
                        // lateWriteCount=0) from one where the hooks fired
                        // but recorded zero. Without this, stale values from
                        // a buffer slot's previous-dialogue use would bleed
                        // into the new capture.
                        r.lateWriteCount = 0;
                        r.cameraStateId  = 0;
                        r.crLocalTx = r.crLocalTy = r.crLocalTz = 0.0f;
                        r.crWorldTx = r.crWorldTy = r.crWorldTz = 0.0f;
                        r.crEulerX  = r.crEulerY  = r.crEulerZ  = 0.0f;
                        r.niLocalTx = r.niLocalTy = r.niLocalTz = 0.0f;
                        r.niWorldTx = r.niWorldTy = r.niWorldTz = 0.0f;
                        r.niEulerX  = r.niEulerY  = r.niEulerZ  = 0.0f;
                        r.playerPosX = r.playerPosY = r.playerPosZ = 0.0f;
                        r.elapsed     = m_diagDlgPerFrameElapsed;
                        r.dt          = dt;
                        r.sidePos     = mSide.position;   r.sideTgt   = mSide.target;
                        r.heightPos   = mHeight.position; r.heightTgt = mHeight.target;
                        r.zoomPos     = mZoom.position;   r.zoomTgt   = mZoom.target;
                        r.fovPos      = mFOV.position;    r.fovTgt    = mFOV.target;
                        r.pitchPos    = mPitch.position;  r.pitchTgt  = mPitch.target;
                        r.blendActive = m_dlgChanBlend.active;
                        r.lockstepActive = m_dialogueEntryLockstepActive;
                        r.blendT      = m_dlgChanBlend.active
                                      ? std::clamp(m_dlgChanBlend.elapsed / m_dlgChanBlend.duration, 0.0f, 1.0f)
                                      : -1.0f;
                        r.blendS      = (r.blendT >= 0.0f)
                                      ? r.blendT * r.blendT * r.blendT * (r.blendT * (r.blendT * 6.0f - 15.0f) + 10.0f)
                                      : -1.0f;
                        r.tpsExpectedX = tps->posOffsetExpected.x;
                        r.tpsExpectedY = tps->posOffsetExpected.y;
                        r.tpsExpectedZ = tps->posOffsetExpected.z;
                        r.tpsActualX   = tps->posOffsetActual.x;
                        r.tpsActualY   = tps->posOffsetActual.y;
                        r.tpsActualZ   = tps->posOffsetActual.z;
                        r.tpsCurZoom   = tps->currentZoomOffset;
                        r.tpsTgtZoom   = tps->targetZoomOffset;
                        auto* playerCamN = RE::PlayerCamera::GetSingleton();
                        r.worldFOV     = playerCamN ? playerCamN->worldFOV : 0.0f;
                        r.dataAngleX   = player ? player->data.angle.x : 0.0f;
                        r.dataAngleZ   = player ? player->data.angle.z : 0.0f;
                        r.freeRotX     = tps->freeRotation.x;
                        r.freeRotZ     = tps->freeRotation.y;  // engine field naming: .y is yaw freerot
                        r.compY        = compY;
                        r.effZoom      = effectiveZoom;
                        r.dlgY         = dlgY;
                    }
                    const bool windowExpired = (m_diagDlgPerFrameElapsed >= 1.50f) ||
                                               (m_diagDlgPerFrameCount >= kDiagDlgPerFrameCap);
                    if (windowExpired) {
                        // [DLG-PERFRAME] burst flush DISABLED. The flush emits
                        // ~200 synchronous spdlog::info calls in a single frame,
                        // a ~75-100ms stutter at 1.50s. Per
                        // feedback_no_log_in_hot_path.md, synchronous I/O in hot
                        // paths causes stutter. The capture buffer is still
                        // populated; the lightweight [DLG-HANDOFF] one-liner
                        // (logged on the entry edge) is the permanent guard
                        // against the gameplay->dialogue Z-snap regression fixed
                        // 2026-05-31. Re-enable this loop only for deep per-frame
                        // debugging.
                        m_diagDlgPerFrameActive = false;
                    }
                }

                if (m_diagDlgEdge.pending && m_diagDlgEdge.isEntry) {
                    spdlog::debug(
                        "[DLG-EDGE] ENTRY | "
                        "side {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                        "height {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                        "zoom {:.2f}->{:.2f} (delta {:+.2f}) tgt {:.2f}->{:.2f} vel={:+.1f} | "
                        "fov {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                        "pitch {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                        "tps actual.y={:.1f} expected.y={:.1f} compY={:.1f} curZoom={:.1f} tgtZoom={:.1f} savedY={:.1f} savedCur={:.1f} | "
                        "lockstep={} lockDur={:.3f} dt={:.4f} effZoom={:.2f} dlgY={:.1f}",
                        m_diagDlgEdge.sidePosBefore, m_diagDlgEdge.sidePosAfter, m_diagDlgEdge.sideTarget, m_diagDlgEdge.sideVelAfter,
                        m_diagDlgEdge.heightPosBefore, m_diagDlgEdge.heightPosAfter, m_diagDlgEdge.heightTarget, m_diagDlgEdge.heightVelAfter,
                        m_diagDlgEdge.zoomPosBefore, m_diagDlgEdge.zoomPosAfter,
                        m_diagDlgEdge.zoomPosAfter - m_diagDlgEdge.zoomPosBefore,
                        m_diagDlgEdge.zoomTargetBefore, m_diagDlgEdge.zoomTargetAfter,
                        m_diagDlgEdge.zoomVelAfter,
                        m_diagDlgEdge.fovPosBefore, m_diagDlgEdge.fovPosAfter, m_diagDlgEdge.fovTarget, m_diagDlgEdge.fovVelAfter,
                        m_diagDlgEdge.pitchPosBefore, m_diagDlgEdge.pitchPosAfter, m_diagDlgEdge.pitchTarget, m_diagDlgEdge.pitchVelAfter,
                        tps->posOffsetActual.y, tps->posOffsetExpected.y, compY,
                        tps->currentZoomOffset, tps->targetZoomOffset,
                        sSavedEngineY, sSavedEngineCurrentZoom,
                        m_diagDlgEdge.lockstepActiveAfter ? 1 : 0,
                        m_diagDlgEdge.lockstepDuration,
                        m_diagDlgEdge.dt, effectiveZoom, dlgY);
                    // Handoff: the LAST GAMEPLAY frame's final rendered camera
                    // state (snapshot taken by the late hook before this edge
                    // frame). Compare to dialogue frame +#00 below: a large
                    // delta in crWorld* or niEuler* is the gameplay->dialogue
                    // one-frame snap the per-frame buffer can't otherwise see.
                    if (m_dlgLastRendered.valid) {
                        spdlog::debug(
                            "[DLG-HANDOFF] last-gameplay-frame | cRootW=({:+.1f},{:+.1f},{:+.1f}) niE=({:+.3f},{:+.3f},{:+.3f}) wFOV={:.2f} state=0x{:x} | (compare to PERFRAME+#00)",
                            m_dlgLastRendered.crWorldTx, m_dlgLastRendered.crWorldTy, m_dlgLastRendered.crWorldTz,
                            m_dlgLastRendered.niEulerX,  m_dlgLastRendered.niEulerY,  m_dlgLastRendered.niEulerZ,
                            m_dlgLastRendered.worldFOV,
                            static_cast<unsigned>(m_dlgLastRendered.cameraStateId));
                    } else {
                        spdlog::debug("[DLG-HANDOFF] last-gameplay-frame snapshot not valid");
                    }
                    m_diagDlgEdge.pending = false;
                }
            }
            // Dialogue handed the camera to ANOTHER blocking menu without ever
            // passing through a gameplay frame â€” picking "let's trade" closes
            // the Dialogue Menu and opens the Barter Menu on the same frame,
            // and the same happens for gift-giving, training and persuasion
            // mini-games. This branch owns posOffset.y for the whole of
            // dialogue, and the close-edge block that gives it back lives past
            // the return below, so on that path nobody ever gave it back: the
            // camera stayed at the dialogue's pull-back distance for the rest
            // of the session, stacking on top of whatever the state's own
            // framing asked for. That is the dialogue settings "leaking into
            // normal settings".
            //
            // Hand it back HERE instead. No blend â€” the frames this covers are
            // behind a paused menu, so there is nothing to ease; the ordinary
            // close-edge path still owns the smooth exit whenever dialogue ends
            // into gameplay. Z goes to the resting composition (targetProfile's
            // height, no pitch compensation) because the compensation belonged
            // to the dialogue framing that just ended.
            if (sLastWroteDialogueY && !wroteDialogueY) {
                spdlog::debug("[DLG-EXIT] menu-to-menu handoff: restoring posOffset "
                             "y {:.1f}->{:.1f} z {:.1f}->{:.1f}",
                             tps->posOffsetExpected.y, sSavedEngineY,
                             tps->posOffsetExpected.z, targetProfile.height * aspectFactor);
                tps->posOffsetExpected.y = sSavedEngineY;
                tps->posOffsetActual.y   = sSavedEngineY;
                const float restZ        = targetProfile.height * aspectFactor;
                tps->posOffsetExpected.z = restZ;
                tps->posOffsetActual.z   = restZ;
                // An exit blend may already be armed for this frame (the
                // profile-side edge fires whether or not this branch runs).
                // Its Y/Z capture never happened, so make sure it can't try.
                m_dlgChanBlend.yzValid = false;
            }
            // PUBLISH THIS FRAME'S FRAMING FOR THE DRAW-EDGE GUARD (2026-09-04,
            // user: "sheathing and unsheathing my weapon while in dialogue ...
            // went to a completely different view for 1 frame"). This branch
            // returns before the snapshot at the end of Update, so for the
            // whole of a dialogue sLastFraming* still held the LAST GAMEPLAY
            // frame - and HookedProcessWeaponDrawnChange, which fires on the
            // in-dialogue sheathe (DialogueInputSink dispatches it from the raw
            // key), "restored" that stale gameplay framing over the dialogue
            // look for exactly one rendered frame before this branch wrote the
            // dialogue framing back. Publish what was actually left on the
            // fields, the same way the gameplay path does.
            if (wroteDialogueY) {
                sLastFramingX     = tps->posOffsetExpected.x;
                sLastFramingY     = tps->posOffsetExpected.y;
                sLastFramingZ     = tps->posOffsetExpected.z;
                sLastFramingZoomT = tps->targetZoomOffset;
                sLastFramingZoomC = tps->currentZoomOffset;
                sLastFramingValid = true;
            }
            sLastWroteDialogueY = wroteDialogueY;
            sUpdateRetMenuBlock.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Close edge: dialogue branch wrote y last frame, not this frame.
        // Restore the engine's intended y so the camera animates back to
        // its natural pose instead of holding the dialogue zoom forever.
        if (sLastWroteDialogueY) {
            const float exitEngineYBefore       = tps->posOffsetExpected.y;
            const float exitActualYBefore       = tps->posOffsetActual.y;
            const float exitCurZoomBefore       = tps->currentZoomOffset;
            const float exitTargetZoomBefore    = tps->targetZoomOffset;
            // posOffsetExpected.y is NOT instantly restored here. Capture
            // the dialogue's last-frame Y values + the engine's pre-
            // dialogue Y baseline into the chanBlend so the normal-branch
            // fall-through below smoothsteps both Expected.y and Actual.y
            // from the dialogue value to sSavedEngineY across the same
            // 0.97s window as the other channels. Previously this line
            // wrote `tps->posOffsetExpected.y = sSavedEngineY` instantly,
            // and the engine's slow internal Actualâ†Expected lerp (~2.7%/
            // frame, ~1.8s settle) took over â€” that mismatch made the
            // exit feel asymmetric on far-back presets: side returns to
            // shoulder in 0.97s while distance lags behind for another
            // second. Unified curve fixes that.
            m_dlgChanBlend.startExpectedY = exitEngineYBefore;
            m_dlgChanBlend.startActualY   = exitActualYBefore;
            m_dlgChanBlend.targetY        = sSavedEngineY;
            // Z mirrors Y: capture the last dialogue compZ (= dlgY*sp +
            // h*cp, pitch-compensated) as start; target is the post-
            // dialogue effectiveHeight (raw, no pitch comp). The chanBlend
            // smoothstep fades the comp out over the same 0.97s window
            // so the camera doesn't snap upward when the comp disappears.
            // aspectFactor is in scope here (line 1866) so we can compute
            // the post-dialogue effective value directly.
            m_dlgChanBlend.startExpectedZ = tps->posOffsetExpected.z;
            m_dlgChanBlend.startActualZ   = tps->posOffsetActual.z;
            m_dlgChanBlend.targetZ        = m_dlgChanBlend.targetHeight * aspectFactor;
            m_dlgChanBlend.yzValid        = true;
            sLastWroteDialogueY = false;

            // Channel-transfer fix: during dialogue, distance is driven by
            // posOffsetExpected.y while ApplyZoom is bypassed (engine zoom
            // stays at vanilla). On the first post-dialogue frame, ApplyZoom
            // re-engages and would write currentProfile.zoom (still ~ the
            // dialogue value because the spring needs time to decay) to the
            // engine zoom field, suddenly stacking ~200 game units on top
            // of the still-receding posOffsetActual.y â€” the visible "snap
            // farther, then smooth in". Snap zoom to the underlying state's
            // target so the zoom axis returns to vanilla immediately and
            // the engine's posOffsetActual.y smoothing handles the entire
            // distance return on its own.
            currentProfile.zoom = targetProfile.zoom;
            velZoom             = 0.0f;
            effectiveZoom       = currentProfile.zoom;

            if (m_diagDlgEdge.pending && !m_diagDlgEdge.isEntry) {
                spdlog::debug(
                    "[DLG-EDGE] EXIT  | "
                    "side {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                    "height {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                    "zoom {:.2f}->{:.2f} (delta {:+.2f}) tgt {:.2f}->{:.2f} vel={:+.1f} | "
                    "fov {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                    "pitch {:.2f}->{:.2f} tgt {:.2f} vel={:+.1f} | "
                    "tps actual.y={:.1f} expected.y(pre)={:.1f}->(post)={:.1f} curZoom={:.1f} tgtZoom={:.1f} savedY={:.1f} savedCur={:.1f} | "
                    "lockstep={} dt={:.4f} effZoom={:.2f} tgtProfZoom={:.2f}",
                    m_diagDlgEdge.sidePosBefore, m_diagDlgEdge.sidePosAfter, m_diagDlgEdge.sideTarget, m_diagDlgEdge.sideVelAfter,
                    m_diagDlgEdge.heightPosBefore, m_diagDlgEdge.heightPosAfter, m_diagDlgEdge.heightTarget, m_diagDlgEdge.heightVelAfter,
                    m_diagDlgEdge.zoomPosBefore, m_diagDlgEdge.zoomPosAfter,
                    m_diagDlgEdge.zoomPosAfter - m_diagDlgEdge.zoomPosBefore,
                    m_diagDlgEdge.zoomTargetBefore, m_diagDlgEdge.zoomTargetAfter,
                    m_diagDlgEdge.zoomVelAfter,
                    m_diagDlgEdge.fovPosBefore, m_diagDlgEdge.fovPosAfter, m_diagDlgEdge.fovTarget, m_diagDlgEdge.fovVelAfter,
                    m_diagDlgEdge.pitchPosBefore, m_diagDlgEdge.pitchPosAfter, m_diagDlgEdge.pitchTarget, m_diagDlgEdge.pitchVelAfter,
                    exitActualYBefore, exitEngineYBefore, tps->posOffsetExpected.y,
                    exitCurZoomBefore, exitTargetZoomBefore,
                    sSavedEngineY, sSavedEngineCurrentZoom,
                    m_diagDlgEdge.lockstepActiveAfter ? 1 : 0,
                    m_diagDlgEdge.dt, effectiveZoom, targetProfile.zoom);
                m_diagDlgEdge.pending = false;
            }
        }

        // Write BOTH posOffsetActual and posOffsetExpected. The engine
        // tries to lerp actual toward expected at ~0.027/frame (settle
        // time ~1.8s â€” verified via SPRINTTRACE2). That's far too slow
        // to feel responsive on profile transitions. Writing both fields
        // each frame neuters the engine's lerp; our own Layer-2 EMA
        // (chaseEMA above) provides the smoothing.
        tps->posOffsetActual.x   = effectiveSide;
        tps->posOffsetExpected.x = effectiveSide;
        tps->posOffsetActual.z   = effectiveHeight;
        tps->posOffsetExpected.z = effectiveHeight;

        // posOffset.y â€” the engine's fOverShoulderCombatAddY.
        //
        // Leaving this to the engine (the pre-2026-08-19 behaviour) meant the
        // drawn states silently carried an extra pull-back nothing in DDC
        // asked for: the log shows posOffsetActual.y = 0 sheathed and -100
        // the moment a weapon comes out. So two entries with byte-identical
        // Side / Height / Zoom / FOV rendered at different distances, which
        // is exactly what the user reported ("when sheathed and unsheathed are
        // the same exact settings, unsheathed is farther zoomed out"). It also
        // meant the same preset framed differently on two machines, because
        // the value is an INI setting (vanilla ships -25; this one is -100).
        //
        // DDC already owns lateral (x) and vertical (z) outright; distance is
        // the Zoom slider. Pinning y to 0 completes that â€” the profile is the
        // whole answer and nothing is added behind it.
        //
        // MOUNTS AND DRAGON RIDING ARE EXCLUDED from the pin-to-zero: those
        // states have their own engine framing that DDC's mount profiles were
        // tuned on top of.
        //
        // HORSEBACK WAS BRIEFLY PINNED TO ZERO (2026-08-24) AND IT WAS A BAD
        // REGRESSION â€” do not try it again. The reasoning was sound (routing
        // Horseback Melee / Archery meant the engine's combat addend now made
        // two mount entries disagree) but the cure was worse: on a horse the
        // addend is not a small trim, it is what holds the camera off an
        // animal much larger than the player. Zeroing it jammed the camera
        // onto the horse â€” user: "Horse camera settings are now completely
        // broken. I was stuck super zoomed in and could not fix it." The log
        // showed the mount profile resolving at zoom 0, i.e. nothing else was
        // providing any distance at all.
        {
            const auto stY = resolver.GetState();
            const bool mountedY =
                (stY == CameraState::Horseback || stY == CameraState::DragonRiding);

            // DISMOUNT EASE. The pin below sets y to 0 outright, and on a
            // mount y is the engine's addend — measured at -300 on this setup.
            // So the frame the player leaves the saddle, the camera dollied
            // 300 units forward instantly. That is the "transition back to
            // third person looks terrible and snaps" report, and [MOUNTEXIT+1]
            // caught it exactly: posExp=(8.0, 0.0, -3.2) one frame after a
            // mounted y of -300.
            //
            // Carry the mount's y across the boundary and decay it to 0 over
            // the same sort of window the profile channels use, so the dolly
            // happens WITH the framing change instead of a frame ahead of it.
            static bool sMountYEasing = false;

            // ----- [MOUNTEXIT-Y] THE ZOOM FOLD IS GONE. ---------------------
            //
            // It converted the mount's ~300-unit pull-back into the zoom channel
            // on the dismount edge, so DDC's zoom spring would carry the whole
            // distance to the on-foot framing. The reasoning was sound and the
            // arithmetic was right — it landed within 10 units of the mounted
            // distance every time — but it was answering a question that should
            // never have been asked. It only existed because [MOUNTEXIT-CUT] had
            // deleted the engine's dismount lerp, leaving nothing else to move
            // the camera across those 300 units.
            //
            // A spring easing 300 units to the on-foot framing IS a dolly. That
            // is the "it re-zooms in to the correct settings" half of the
            // report, and no amount of rate, curve or clock tuning was ever
            // going to make a 300-unit push-in read as anything else. The other
            // half — "it goes right up to the back of the player's head" — was
            // the one to three frames before the fold could land, which is why
            // moving the fold earlier (2026-08-26, first attempt) changed the
            // trace by nothing at all: h went 73.1 -> 67.7 -> 363.9 both times.
            //
            // The engine's transition covers this distance in ~267 ms with no
            // help. [CAMCOVER] now runs during it and points the destination
            // state at the on-foot framing, so vanilla's lerp lands on the DDC
            // pose. DDC supplies the endpoint and moves NOTHING.
            //
            // DO NOT REINTRODUCE A CARRY, AN EASE, A BLEND OR A FOLD ON THIS
            // EDGE. Every one of them has been built and removed at least once,
            // and each was built to patch the damage of the one before it. If
            // the dismount ever needs to be faster or slower, that is the
            // engine's transition, not a channel DDC animates.
            // ----- [MOUNTENTER-Y] THE OTHER END OF THE SAME BUG -------------
            //
            // Mounting slams posOffset.y from 0 to the engine's ~-300 addend in
            // ONE frame. Measured 2026-08-26 18:54, first frame of the mount:
            //
            //   [MOUNTENTER +1] posExp=(+46.7, -300.0, -18.7)  DIST=428.7
            //
            // y is already fully at -300 while side and height are still sitting
            // at their ON-FOOT values (46.7, -18.7) and only ease down over the
            // next second. So every other channel transitions and this one cuts:
            // a ~300-unit outward jump the instant the state swaps.
            //
            // THIS IS THE DISMOUNT BUG AT THE OTHER END. The dismount collapsed
            // 300 units inward in one frame because the pull-back vanished
            // instantly; the mount throws the camera 300 units outward because
            // it arrives instantly. Nineteen rounds went into one end without
            // anything ever measuring the other — mounting had no probe at all
            // until [MOUNTENTER] was added this session.
            //
            // The dismount is covered by vanilla's own kPCTransition lerp
            // ([[CAMCOVER]]). MOUNTING HAS NO TRANSITION STATE — it is
            // camState 9 -> 10 in a single step — so there is nothing to cover
            // and DDC has to carry this one itself.
            //
            // IT ALSO CAUSES THE COLLISION SNAPS. freeRotation.x decays 2.76 ->
            // 0 over ~72 frames, orbiting the camera ~158 degrees behind the
            // horse. Starting that orbit at 400+ units sweeps ~1100 units of arc
            // through the world and the engine's camera collision grabs it
            // repeatedly — four single-frame pull-ins of 67-223 units per mount,
            // confirmed against the player position (camera moved 223u on a
            // frame the player moved 3.6u). Easing the pull-back in keeps the
            // camera near the horse through the fast part of the swing, where
            // the arc is short. [[mounting-edge-collision-snaps]]
            //
            // WHY THIS IS NOT THE BANNED HORSEBACK Y-PIN. Pinning horseback y to
            // ZERO permanently was a bad regression on 2026-08-24 — "Horse
            // camera settings are now completely broken. I was stuck super
            // zoomed in and could not fix it." This does the opposite: it
            // converges ON the engine's own value, hands the field back the
            // moment it gets there, and cannot outlive its window. Three
            // independent stops — converged, frame cap, or the resolver leaving
            // Horseback — and every one of them ends with the engine's value
            // standing. The failure mode of a bug here is a slightly slow pull
            // back, never a camera stuck on the horse.
            //
            // Horseback ONLY. Dragon riding is deliberately untouched.
            constexpr int kMountEnterMaxFrames = 180;   // 3s hard cap, safety only

            const bool horsebackY = (stY == CameraState::Horseback);

            // Arming lives in NotifyMountEntry, called from the PRE-Update hook.
            // All this does is cancel: leaving Horseback, or a dismount
            // transition starting, ends the ease and hands y back immediately.
            if (!horsebackY || coveringTransition) {
                sMountEnterN = -1;
            }

            // ----- IT IS AN ORDINARY CHANNEL. THAT IS THE WHOLE DESIGN. -----
            //
            // User, and it is the instruction: *"the transition should use the
            // same machinery that all other transitions use."*
            //
            // TWO BESPOKE MECHANISMS WERE BUILT HERE AND BOTH ARE GONE. First a
            // free-running spring of its own, then a curve driven off the entry
            // orbit's progress with a frame floor and a smootherstep. Each
            // "fixed" the artifact the previous one left and each was a private
            // clock for one edge — the exact mistake this camera has made in
            // nearly every round, on both horse edges.
            //
            // What the mount actually needed was already sitting in the file.
            // mMountY IS a ChannelMotion. It is ALREADY a member of the Weight
            // group alongside side, height and zoom, and it ALREADY goes through
            // stepMotion with the same omega, the same velocity cap and the same
            // shared span scale. All that was missing was giving it a TARGET.
            //
            // Doing that fixes the dip the bespoke version left for free, and
            // for a reason worth stating: the group takes ONE scale, the
            // most-slowed member's, so the channel with the furthest to travel
            // sets how long the move takes. y travels ~300 units and side/height
            // travel ~46/18, so y now paces the group and the others stretch to
            // match instead of racing to the horseback zeros while y is still
            // near 0 — which is what dropped the camera to h~73, closer than it
            // sits on foot, for half a second.
            //
            // So the mount is a profile change like any other: one group, one
            // shared duration, every per-channel transition slider and the
            // Weight setting meaning exactly what they mean everywhere else.
            // The only special thing left is the SEED — position starts at 0 on
            // the entry frame, written pre-Update — and that is not a clock,
            // it is just where the channel starts from.
            // AND IT IS THE SAME CHANNEL ON THE WAY OUT.
            //
            // Mounted, the channel target IS the engine's pull-back; on foot it
            // is 0. So the dismount is just the same channel travelling the
            // other way, on the same group, the same sliders and the same
            // shared duration. That is what removed the need for the
            // [CAMCOVER] convergence: the destination pose now INCLUDES the
            // pull-back and walks monotonically from the mounted framing to the
            // on-foot framing, so vanilla's lerp has nothing to overshoot and
            // nothing has to be frozen to keep it honest.
            //
            // While mounted and settled, the live engine value is re-sampled
            // every frame rather than trusting what was captured at entry — the
            // engine REWRITES this addend when a weapon is drawn on horseback
            // (measured -300 sheathed, -150 drawn, via
            // HorseCameraState::ProcessWeaponDrawnChange). Without the re-sample
            // a dismount taken with a weapon out would start the channel from a
            // stale 300 and jump.
            // A dismount is in flight the moment the transition abort says so.
            // Until it finishes, this edge must behave as OFF the horse even
            // though the resolver has not caught up — otherwise the settled
            // re-sample below re-pins the channel to the mounted value and the
            // move does not start for one to three frames.
            if (sMountExitLatched && std::abs(mMountY.position) < 0.5f)
                sMountExitLatched = false;      // arrived — the engine owns y again
            if (sMountEnterN >= 0)
                sMountExitLatched = false;      // back on a horse — the entry ease wins

            if (horsebackY && !sMountExitLatched) {
                if (sMountEnterN >= 0) {
                    ++sMountEnterN;
                    const float y = mMountY.position;
                    if (!std::isfinite(y) ||
                        std::abs(y - sMountEnterY) < 2.0f ||
                        sMountEnterN >= kMountEnterMaxFrames) {
                        // Arrived. A HANDOFF, not a clock — the channel decides
                        // when it is done, and the engine owns y from here.
                        sMountEnterN = -1;
                    } else {
                        // The channel owns the value. Nothing is computed here.
                        sMountYTargetLive        = sMountEnterY;
                        tps->posOffsetExpected.y = y;
                        tps->posOffsetActual.y   = y;
                    }
                }
                if (sMountEnterN < 0) {
                    // Settled on the horse: track whatever the engine currently
                    // has, so the channel is loaded and correct for the dismount.
                    const float liveY = tps->posOffsetExpected.y;
                    if (std::isfinite(liveY) && liveY < -0.5f) {
                        sMountEnterY      = liveY;
                        sMountYTargetLive = liveY;
                        mMountY.position  = liveY;
                        mMountY.velocity  = 0.0f;
                    }
                }
            } else {
                // Off the horse — including DRAGON riding, which is deliberately
                // left to the engine exactly as before. Target 0; the channel
                // carries whatever pull-back it was holding back out on the
                // ordinary sliders, and once it arrives this is just the
                // long-standing pin-to-zero.
                sMountYTargetLive = 0.0f;
                sMountEnterY      = 0.0f;
                if (stY == CameraState::DragonRiding) {
                    mMountY.position = 0.0f;
                    mMountY.velocity = 0.0f;
                }
            }

            // THE CARRY IS DEAD. 2026-08-24.
            //
            // It existed to stop posOffset.y cutting from the mounted -300 to 0
            // in one frame. But the guard log finally showed what the engine was
            // actually handing back on the first third-person frame:
            //
            //   engine act=(50.0, 0.0, -20.0) zoom c=-0.100
            //
            // That IS the user's third person framing, already correct, already
            // there. The engine hands the camera back properly and DDC was
            // overwriting it with the horse's -300 and then dollying 263 units
            // inward over ~1.5s. The user described that, accurately, as zooming
            // into the player, and asked four times for it to "just go from
            // horseback to third person".
            //
            // The one-frame 275-unit lunge that the MOUNTEXIT-GUARD was built
            // for was CAUSED BY THIS CARRY — it was the gap between the engine
            // composing at y~0 and our writing y=-292. With no carry there is no
            // gap and nothing to lunge between, so the guard goes too.
            //
            // Do not reintroduce a carry, an ease, or a blend on this edge
            // without first re-reading what the engine already put in
            // posOffsetActual. The answer to "make the dismount smooth" was
            // never to animate it; it was to stop fighting a handover that was
            // already right.
            // RESTORED, but on the dismount clock and nothing else's.
            //
            // Measured from the CORRECT node (niCamera, not the pivot) the
            // engine's dismount is a 256-unit cut in one frame: h=389.1 while
            // mounted, h=122.3 on the very next frame. With the carry deleted
            // DDC stopped fighting that cut — which is why the push-in went
            // away — but the cut is what the user then felt as "it snaps".
            //
            // Something has to cover those 266 units or it is a cut. The carry
            // is that something. What made it read as a DOLLY before was
            // duration (1.5s on the zoom slider) and the fact that the framing
            // channels were travelling on their own separate clocks at the same
            // time. Both are fixed: it runs on kDismountOmega with every other
            // channel, so the mounted framing moves to the on-foot framing as
            // ONE motion.
            // `coveringTransition` forces this write during the engine's
            // dismount lerp even while the state resolver still reports
            // Horseback — it lags the engine's state swap by one to two frames.
            // The destination third-person state is what the lerp interpolates
            // TOWARD, so its posOffset.y has to be the on-foot 0 from the very
            // first transition frame or the endpoint moves under the lerp.
            if ((!mountedY || coveringTransition || sMountExitLatched) &&
                !(m_dlgChanBlend.active && m_dlgChanBlend.isExit)) {
                // THE DISMOUNT RIDES THE CHANNEL. Target is 0 off the horse, so
                // this is the mounted pull-back travelling back out through
                // stepMotion — same omega, same velocity cap, same shared Weight
                // scale as side/height/zoom, finishing WITH them because the
                // group is paced by its biggest mover.
                //
                // This is NOT the old carry. That was a bare (float,float) pair
                // driven by the first-order `springStep` helper while every
                // other channel ran CriticalDampedSpringExact. First-order
                // starts at MAXIMUM velocity, critically-damped starts at zero
                // and accelerates, so the carry rocketed off on frame one and
                // reached the player while the zoom had barely begun — "it goes
                // right up to the back of the player's head" — and the zoom
                // eased out behind it — "then it re-zooms in to the correct
                // settings." The rate was never the problem; the CURVE was.
                //
                // Two channels can only move as one motion if they are the same
                // KIND of motion. That is precisely what routing this through
                // stepMotion buys, and it is why this is safe now and was not
                // then. DO NOT re-implement it as a bespoke spring again.
                if (!std::isfinite(mMountY.position)) {
                    mMountY.position = 0.0f;
                    mMountY.velocity = 0.0f;
                }
                if (std::abs(mMountY.position) < 0.5f) {
                    mMountY.position = 0.0f;
                    mMountY.velocity = 0.0f;
                    sMountYEasing    = false;
                }
                const float writeY = mMountY.position;
                tps->posOffsetExpected.y = writeY;
                tps->posOffsetActual.y   = writeY;
            }
        }
        // (The polling MOUNTED WEAPON-DRAW EQUALISER that lived here was
        // removed 2026-08-24. It tried to latch the sheathed posOffset.y by
        // sampling frames where GetRiderWeapon() read None, and it never
        // captured the right one — [MOUNTY] reported delta 0.0 twice while
        // [MOUNTPROF] simultaneously showed -300 sheathed vs -150 drawn.
        // Replaced by a hook on HorseCameraState::ProcessWeaponDrawnChange,
        // which is the exact instant the engine rewrites the value. Do not
        // reintroduce a polled version: the edge is observable, so poll
        // nothing.)
        // EXCEPTION: while the post-dialogue chanBlend is active, we
        // drive Y through the same smootherstep as X/Z/FOV/Pitch so
        // exit completes in one synchronized motion instead of the
        // engine's ~1.8s slow lerp on Actual.y trailing the 0.97s
        // chanBlend on the other axes.
        if (m_dlgChanBlend.active && m_dlgChanBlend.isExit &&
            m_dlgChanBlend.yzValid && m_dlgChanBlend.duration > 0.0f) {
            const float t = std::clamp(
                m_dlgChanBlend.elapsed / m_dlgChanBlend.duration, 0.0f, 1.0f);
            const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            tps->posOffsetExpected.y =
                std::lerp(m_dlgChanBlend.startExpectedY, m_dlgChanBlend.targetY, s);
            tps->posOffsetActual.y =
                std::lerp(m_dlgChanBlend.startActualY,   m_dlgChanBlend.targetY, s);
            // Z too â€” fades out the pitch-compensation that was applied
            // during dialogue (compZ = dlgY*sp + h*cp). Without this,
            // raw effectiveHeight is written at exit frame, leaving the
            // dlgY*sp component (~-8 units at typical pitch) uncovered
            // â†’ camera snaps that distance upward in world frame.
            tps->posOffsetExpected.z =
                std::lerp(m_dlgChanBlend.startExpectedZ, m_dlgChanBlend.targetZ, s);
            tps->posOffsetActual.z =
                std::lerp(m_dlgChanBlend.startActualZ,   m_dlgChanBlend.targetZ, s);
        }
        dialogueExitDecayTime = 0.0f;
        dialogueLastY         = 0.0f;

        // [DLGX] Per-frame trace of the dialogue EXIT window.
        //
        // Two rounds of exit fixes have now been aimed at this from reasoning
        // alone, and the process lesson from the 1p-noise chase applies: when a
        // one-frame artifact survives a confident fix, stop theorising and log
        // every contributing channel on the exact frames it happens. One line
        // per frame for the length of the blend, at most a handful of
        // conversations, so a single log answers "which channel stepped, and on
        // which frame" instead of another guess.
        //
        // `dRend` is the frame-to-frame movement of the FINAL rendered camera
        // position â€” the thing the eye actually sees. A spike there with every
        // component moving smoothly means the step is downstream of all of
        // them; a spike that matches one component names the culprit outright.
        {
            static int   sDlgxFrames    = 0;
            static int   sDlgxSessions  = 0;
            static float sDlgxPrevRx = 0.0f, sDlgxPrevRy = 0.0f, sDlgxPrevRz = 0.0f;
            static bool  sDlgxPrevValid = false;
            if (dlgExitEdge && sDlgxSessions < 6) {
                sDlgxFrames    = 110;
                sDlgxPrevValid = false;
                ++sDlgxSessions;
            }
            if (sDlgxFrames > 0) {
                --sDlgxFrames;
                float dRend = -1.0f;
                if (m_dlgLastRendered.valid && sDlgxPrevValid) {
                    const float ddx = m_dlgLastRendered.crWorldTx - sDlgxPrevRx;
                    const float ddy = m_dlgLastRendered.crWorldTy - sDlgxPrevRy;
                    const float ddz = m_dlgLastRendered.crWorldTz - sDlgxPrevRz;
                    dRend = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                }
                if (m_dlgLastRendered.valid) {
                    sDlgxPrevRx = m_dlgLastRendered.crWorldTx;
                    sDlgxPrevRy = m_dlgLastRendered.crWorldTy;
                    sDlgxPrevRz = m_dlgLastRendered.crWorldTz;
                    sDlgxPrevValid = true;
                }
                spdlog::debug("[DLGX] f={:2d} dRend={:7.2f} | blend act={} yz={} t={:.3f}/{:.3f} "
                             "| side={:7.2f} h={:7.2f} zoom={:7.2f} fov={:6.2f} pitch={:7.2f} "
                             "| posY={:8.2f} posZ={:7.2f} engZoom={:.4f}/{:.4f} | dt={:.4f}",
                             110 - sDlgxFrames, dRend,
                             m_dlgChanBlend.active ? 1 : 0, m_dlgChanBlend.yzValid ? 1 : 0,
                             m_dlgChanBlend.elapsed, m_dlgChanBlend.duration,
                             mSide.position, mHeight.position, mZoom.position,
                             mFOV.position, mPitch.position,
                             tps->posOffsetActual.y, tps->posOffsetActual.z,
                             tps->currentZoomOffset, tps->targetZoomOffset, dt);
            }
        }

        // Mounted states join dragon riding in allowing negative zoom. Same
        // reason: the orbit centres on the MOUNT, so "vanilla closest" is
        // already well back from the rider and zoom 0 was a floor the user
        // could not get under. The 2026-08-16 warning below applies to the
        // ON-FOOT orbit (pulling below zero walks the camera through the
        // player's own body); a horse is big enough that the same slider
        // range just closes the gap the mount itself opened.
        {
            const auto stZ = resolver.GetState();
            ApplyZoom(tps, effectiveZoom,
                      stZ == CameraState::DragonRiding || stZ == CameraState::Horseback);
        }

        // Cache the steady gameplay-3p posOffset.y / zoom baseline so a later
        // mid-dialogue 1p->3p switch can restore to it instead of the engine's
        // transient mid-POV-transition value (see dlgPovSwitchEntry capture).
        // Only when this is a genuine steady gameplay frame â€” not while the
        // dialogue exit blend is still driving these fields.
        if (!m_dlgChanBlend.active) {
            m_gameplay3pPosY          = tps->posOffsetExpected.y;
            m_gameplay3pTargetZoom    = tps->targetZoomOffset;
            m_gameplay3pCurrentZoom   = tps->currentZoomOffset;
            m_gameplay3pBaselineValid = true;

            // Publish the same frame's full framing for the weapon-draw edge
            // guard in HookManager. Our writes land AFTER _originalThirdPersonUpdate
            // has already composed this frame, so they are what the NEXT frame
            // renders â€” which leaves a window for an engine event handler firing
            // in between to get one rendered frame of its own values. Read back
            // from the fields rather than from effectiveSide/Height so this is
            // literally what we left behind, blend exceptions included.
            sLastFramingX     = tps->posOffsetExpected.x;
            sLastFramingY     = tps->posOffsetExpected.y;
            sLastFramingZ     = tps->posOffsetExpected.z;
            sLastFramingZoomT = tps->targetZoomOffset;
            sLastFramingZoomC = tps->currentZoomOffset;
            sLastFramingValid = true;
        }

        // TEMPORARY [ZOOMDIAG] â€” chasing "the camera zooms out at random and
        // sheathing fixes it". Fires only when the RESOLVED zoom moves by more
        // than a slider notch, and names every layer that could have moved it,
        // so the log says which one latched instead of us guessing. Sheathing
        // re-resolves the state, which is why it clears whatever is stuck â€”
        // the line printed just before the sheathe is the culprit's fingerprint.
        // Strip once identified.
        {
            // Keyed on the RESOLVED TARGET, not the eased value â€” the target
            // steps discretely once per state/override change, so this is one
            // line per decision instead of one per frame of the ease.
            static float sLastDiagZoom = -1.0e9f;
            if (std::abs(targetProfile.zoom - sLastDiagZoom) > 0.5f) {
                sLastDiagZoom = targetProfile.zoom;
                spdlog::debug("[ZOOMDIAG] zoom={:.1f} (target={:.1f}) state={} sub={} pa={} paDir={} "
                             "lagHold={} shout={} locked={} tlSlot={} enemyOv={} bind={} prof=0x{:x}",
                             effectiveZoom, targetProfile.zoom,
                             static_cast<int>(resolver.GetState()),
                             static_cast<int>(resolver.GetSubState()),
                             resolver.IsPowerAttacking(),
                             static_cast<int>(resolver.GetPowerAttackDirection()),
                             resolver.IsAttackLagHolding(),
                             resolver.GetSubState() == CameraSubState::Shout,
                             resolver.IsTargetLocked(),
                             tlSlot ? static_cast<int>(*tlSlot) : -1,
                             sLastEnemyOverrideProfile != nullptr,
                             activeTLBinding != nullptr,
                             reinterpret_cast<std::uintptr_t>(selected));
            }
        }

        static CameraState    lastLoggedState    = static_cast<CameraState>(-1);
        static CameraSubState lastLoggedSubState = static_cast<CameraSubState>(-1);
        const auto curState    = resolver.GetState();
        const auto curSubState = resolver.GetSubState();

        // [MOUNTATK] â€” groundwork for "Horseback Melee Left/Right Attack".
        //
        // The side is NOT guessed. Vanilla mounted melee picks a left- or
        // right-side swing animation, and picking the wrong signal here is how
        // the dragon sub-states went wrong â€” so this logs the candidates and
        // commits to nothing until a capture says which one tracks the swing:
        //
        //   * the AIM-vs-HORSE yaw delta, the most likely driver (the engine
        //     chooses the side you are looking toward). Printed in degrees,
        //     signed, wrapped to +-180: negative = aiming left of the horse's
        //     forward, positive = right.
        //   * meleeAttackState, so the window the swing occupies is visible.
        //
        // The animation tags themselves come free: [GraphEvent] logs each tag
        // ONCE on first sight, so a ride with swings on both sides adds any
        // side-specific tag to the dump by itself.
        if (curState == CameraState::Horseback) {
            auto* plA = RE::PlayerCharacter::GetSingleton();
            RE::ActorState* asA = plA ? plA->AsActorState() : nullptr;
            const auto atkA = asA ? asA->actorState1.meleeAttackState
                                  : RE::ATTACK_STATE_ENUM::kNone;
            static auto sLastMountAtk = RE::ATTACK_STATE_ENUM::kNone;
            if (atkA != sLastMountAtk) {
                sLastMountAtk = atkA;
                float dYaw = 0.0f;
                bool  haveMountYaw = false;
                if (plA) {
                    RE::ActorPtr mountA;
                    if (plA->GetMount(mountA) && mountA) {
                        dYaw = plA->data.angle.z - mountA->data.angle.z;
                        while (dYaw >  3.14159265f) dYaw -= 6.28318530f;
                        while (dYaw < -3.14159265f) dYaw += 6.28318530f;
                        dYaw *= 180.0f / 3.14159265f;
                        haveMountYaw = true;
                    }
                }
                spdlog::debug("[MOUNTATK] meleeAttackState=0x{:X} | aim-vs-horse yaw={}{:.1f} deg "
                             "({}) | rider={}",
                             static_cast<int>(atkA),
                             haveMountYaw ? "" : "?", dYaw,
                             !haveMountYaw ? "no mount handle"
                                           : dYaw < 0.0f ? "aiming LEFT" : "aiming RIGHT",
                             static_cast<int>(resolver.GetRiderWeapon()));
            }
        }

        // [MOUNTPROF] â€” "when horseback and melee and archery have the same
        // settings, they have different angles."
        //
        // The engine-addend theory is DEAD: [MOUNTY] measured the mounted
        // posOffset.y at -150.0 both sheathed and drawn, delta 0.0. So the
        // engine does NOT re-frame a mounted rider on weapon-draw and the
        // difference is somewhere in DDC's own resolution.
        //
        // Nothing existing can see it, because the ordinary state line only
        // fires when the STATE changes and all three of these resolve as
        // Horseback â€” drawing a sword on a horse produces no log line at all.
        // This fires on the resolved-profile edge instead, and prints the
        // TARGET values (what the profile actually says after env / location /
        // enemy resolution) beside the EFFECTIVE ones (what the camera is
        // being driven to). If the targets differ, the three entries are not
        // really carrying the same settings and the divergence is upstream in
        // storage; if the targets match and the angles still differ, it is
        // downstream of the profile â€” noise, transitions, or the engine.
        if (curState == CameraState::Horseback) {
            static const CameraProfile* sLastMountProf = nullptr;
            static int                  sLastRider     = -1;
            const int rider = static_cast<int>(resolver.GetRiderWeapon());
            if (selected != sLastMountProf || rider != sLastRider) {
                sLastMountProf = selected;
                sLastRider     = rider;
                spdlog::debug("[MOUNTPROF] rider={} (0=none 1=melee 2=archery) zoomed={} "
                             "prof=0x{:x} | target(side={:.1f} h={:.1f} zoom={:.1f} fov={:.1f} "
                             "rot={:.1f} pitch={:.1f}) | eff(side={:.1f} h={:.1f} zoom={:.1f} "
                             "fov={:.1f} rot={:.1f} pitch={:.1f}) | posY={:.1f} sub={}",
                             rider, resolver.IsBowZoomed(),
                             reinterpret_cast<std::uintptr_t>(selected),
                             targetProfile.sideOffset, targetProfile.height,
                             targetProfile.zoom, targetProfile.fov,
                             targetProfile.rotation, targetProfile.pitchOffset,
                             effectiveSide, effectiveHeight, effectiveZoom,
                             effectiveFOV, effectiveRot, effectivePitch,
                             tps->posOffsetExpected.y,
                             static_cast<int>(curSubState));
            }
        }
        // DISMOUNT PROBE. "Getting off of a horse breaks the controls" has no
        // captured signature yet, and the freeze watchdog's own dump only
        // starts 1.5s after the player is already stuck â€” by which time the
        // transition that caused it is off the end of the ring. One line on
        // the Horseback -> anything edge, naming every claim and gate that
        // could survive the transition and eat movement input. Cheap: fires
        // once per dismount.
        // PRE-EDGE CAPTURE. [MOUNTEXIT+N] starts sampling one frame AFTER the
        // dismount, which is too late to say what the camera jumped FROM — the
        // y dolly was only caught because the mounted value happened to be
        // logged elsewhere. Keep the last mounted frame's rotation and framing
        // so the edge line can print both sides of it.
        static float sPreExitFreeX = 0.0f, sPreExitFreeY = 0.0f;
        static float sPreExitBodyYaw = 0.0f, sPreExitPosY = 0.0f;
        static float sPreExitZoomC = 0.0f;

        // ---- DENSE EDGE TRACE ---------------------------------------------
        // "It instantly snapped skyward" is a claim about where the camera
        // WAS, and the two-sample probe this replaces could not see it: it
        // printed frame 1 and frame 120 with nothing in between, and it never
        // recorded the camera's position at all. Two samples cannot tell a
        // one-frame pop from a two-second drift, and that IS the question.
        // See [[a-probe-that-cannot-see-its-subject]].
        //
        // m_dlgLastRendered is the always-on snapshot of the FINAL pre-render
        // transform, written from HookedNiCameraUpdateWorldData after every one
        // of our writes has landed. CameraController::Update runs earlier in
        // the frame, so what we read here is LAST frame's rendered camera:
        // wWorld/wEuler on row +N describe the frame the eye actually saw at
        // N-1. Same one-frame convention [DLGX] uses.
        //
        // dW is the frame-to-frame movement of that point and dZ its vertical
        // half. A spike in dZ with every offset column moving smoothly means
        // the rise is NOT the framing channels; a dZ spike that lands on the
        // same row posExp.y jumps names the y-carry outright.
        // THE COLUMN THAT MATTERS IS `dist`. Earlier revisions of this probe
        // logged only the camera's world position, which CANNOT answer "did it
        // zoom into the player" — during a dismount the player is being moved
        // too, so a smooth camera track and a collapsing camera-to-player gap
        // are indistinguishable in world coordinates. Both are sampled at the
        // same instant now (see DlgLastRendered), so `dist` is the real
        // subject: every close-up is a dip in it, no matter what caused it.
        struct MountEdgeRow {
            float wx = 0.0f, wy = 0.0f, wz = 0.0f;   // final rendered niCamera world
            // THE PIVOT, at the same instant. Added 2026-08-27 because five
            // days of this saga could not distinguish "the pivot moved" from
            // "the offset from the pivot moved", and those have completely
            // different fixes. niCamera - cameraRoot IS the framing offset;
            // cameraRoot alone is the anchor. Print both and the edge stops
            // being ambiguous.
            float crx = 0.0f, cry = 0.0f, crz = 0.0f;
            // THE MOUNT'S 3D, same instant. The decisive column: whichever
            // subject the camera holds a CONSTANT vector to across the edge is
            // the anchor. cam-to-player is already here (dist/distH); this adds
            // cam-to-mount so the two can be compared directly instead of
            // inferred. Zero when there is no mount.
            float mx = 0.0f, my = 0.0f, mz = 0.0f;
            float distM = 0.0f;
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;   // player world, SAME instant
            float dist = 0.0f, distH = 0.0f;         // camera->player, 3D and horizontal
            float ex = 0.0f, ey = 0.0f, ez = 0.0f;   // final rendered niCamera euler
            float frx = 0.0f, fry = 0.0f;            // freeRotation
            float bodyX = 0.0f, bodyZ = 0.0f;        // player angles
            float px = 0.0f, py = 0.0f, pz = 0.0f;   // posOffsetExpected
            float pay = 0.0f;                        // posOffsetActual.y (can diverge)
            float zoomC = 0.0f, zoomT = 0.0f, pitchZoom = 0.0f;
            float fov = 0.0f;
            int   stateId = -1;
        };
        auto captureMountRow = [&]() {
            MountEdgeRow r{};
            // The CHILD niCamera, not cameraRoot. The root is the pivot; the
            // pull-back is on the child, so a root-based distance cannot see
            // the zoom at all — which is exactly how three traces in a row
            // reported "smooth" for a dismount that visibly zoomed twice.
            r.wx      = m_dlgLastRendered.niWorldX;
            r.wy      = m_dlgLastRendered.niWorldY;
            r.wz      = m_dlgLastRendered.niWorldZ;
            // Last-known mount position persists past the dismount ON PURPOSE:
            // the whole question is whether the camera keeps a constant vector
            // to the HORSE across the edge, which cannot be asked if the column
            // goes blank the moment GetMount() stops answering.
            static float sMx = 0.0f, sMy = 0.0f, sMz = 0.0f;
            if (auto* plM = RE::PlayerCharacter::GetSingleton()) {
                RE::NiPointer<RE::Actor> mnt;
                if (plM->GetMount(mnt) && mnt) {
                    if (auto* m3d = mnt->Get3D()) {
                        sMx = m3d->world.translate.x;
                        sMy = m3d->world.translate.y;
                        sMz = m3d->world.translate.z;
                    }
                }
            }
            r.mx = sMx;  r.my = sMy;  r.mz = sMz;
            r.crx     = m_dlgLastRendered.crWorldTx;
            r.cry     = m_dlgLastRendered.crWorldTy;
            r.crz     = m_dlgLastRendered.crWorldTz;
            r.lx      = m_dlgLastRendered.plWorldX;
            r.ly      = m_dlgLastRendered.plWorldY;
            r.lz      = m_dlgLastRendered.plWorldZ;
            const float gx = r.wx - r.lx, gy = r.wy - r.ly, gz = r.wz - r.lz;
            r.dist    = std::sqrt(gx * gx + gy * gy + gz * gz);
            r.distH   = std::sqrt(gx * gx + gy * gy);
            const float mgx = r.wx - r.mx, mgy = r.wy - r.my, mgz = r.wz - r.mz;
            r.distM   = std::sqrt(mgx * mgx + mgy * mgy + mgz * mgz);
            r.ex      = m_dlgLastRendered.niEulerX;
            r.ey      = m_dlgLastRendered.niEulerY;
            r.ez      = m_dlgLastRendered.niEulerZ;
            r.fov     = m_dlgLastRendered.worldFOV;
            r.stateId = m_dlgLastRendered.cameraStateId;
            r.frx     = tps->freeRotation.x;
            r.fry     = tps->freeRotation.y;
            if (auto* plR = RE::PlayerCharacter::GetSingleton()) {
                r.bodyX = plR->data.angle.x;
                r.bodyZ = plR->data.angle.z;
            }
            r.px        = tps->posOffsetExpected.x;
            r.py        = tps->posOffsetExpected.y;
            r.pz        = tps->posOffsetExpected.z;
            r.pay       = tps->posOffsetActual.y;
            r.zoomC     = tps->currentZoomOffset;
            r.zoomT     = tps->targetZoomOffset;
            r.pitchZoom = tps->pitchZoomOffset;
            return r;
        };
        // a_tag is MOUNTEXIT or MOUNTENTER. Both edges get the same columns on
        // purpose — "the horse transition" is two transitions and only one of
        // them has ever been instrumented, which is why eight rounds of work
        // went into the dismount without anyone establishing that the dismount
        // was the edge being complained about.
        auto emitMountRow = [](const char* a_tag, int a_idx, const MountEdgeRow& r,
                               const MountEdgeRow& a_prev, bool a_prevValid) {
            float dW = -1.0f, dDist = 0.0f, dPiv = -1.0f, dOff = -1.0f;
            if (a_prevValid) {
                const float ddx = r.wx - a_prev.wx;
                const float ddy = r.wy - a_prev.wy;
                const float ddz = r.wz - a_prev.wz;
                dW    = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                dDist = r.dist - a_prev.dist;
                // dPiv = how far the ANCHOR moved. dOff = how far the camera
                // moved RELATIVE to its anchor, i.e. the framing offset. dW is
                // their sum and cannot tell them apart, which is precisely the
                // ambiguity that cost five days: "the pivot swapped" and "the
                // offset changed" look identical in dW and have nothing in
                // common as bugs. Whichever of these two carries the edge jump
                // is the one to fix; the other is not involved.
                const float pvx = r.crx - a_prev.crx;
                const float pvy = r.cry - a_prev.cry;
                const float pvz = r.crz - a_prev.crz;
                dPiv = std::sqrt(pvx * pvx + pvy * pvy + pvz * pvz);
                const float ofx = (r.wx - r.crx) - (a_prev.wx - a_prev.crx);
                const float ofy = (r.wy - r.cry) - (a_prev.wy - a_prev.cry);
                const float ofz = (r.wz - r.crz) - (a_prev.wz - a_prev.crz);
                dOff = std::sqrt(ofx * ofx + ofy * ofy + ofz * ofz);
            }
            spdlog::info("[{}{:+4d}] st={} DIST={:7.1f} (h={:7.1f}) dDist={:+7.1f} | "
                         "dW={:7.2f} DISTM={:7.1f} | "
                         "cam=({:+9.1f},{:+9.1f},{:+8.1f}) mnt=({:+9.1f},{:+9.1f},{:+8.1f}) "
                         "ply=({:+9.1f},{:+9.1f},{:+8.1f}) | "
                         "rend={:+.3f} wEuler=({:+.3f},{:+.3f}) freeRot=({:+.3f},{:+.3f}) | "
                         "posExp=({:+6.1f},{:+8.1f},{:+6.1f}) actY={:+8.1f} | "
                         "zoom c={:+.3f} t={:+.3f} pz={:+6.1f} fov={:5.1f}",
                         a_tag, a_idx, r.stateId, r.dist, r.distH, dDist, dW, r.distM,
                         r.wx, r.wy, r.wz, r.mx, r.my, r.mz, r.lx, r.ly, r.lz,
                         r.bodyX - r.fry, r.ey, r.ez, r.frx, r.fry,
                         r.px, r.py, r.pz, r.pay,
                         r.zoomC, r.zoomT, r.pitchZoom, r.fov);
        };
        // THE PRE-ROLL IS THE POINT NOW, NOT AN AFTERTHOUGHT.
        //
        // The window has been wrong in BOTH directions, one at a time:
        //  - it was 90 frames forward (~1.5s), shorter than the dismount, so
        //    late events fell off the end. Fixed by going to 240.
        //  - the pre-roll was EIGHT FRAMES (0.13s). The player presses dismount
        //    and the ANIMATION plays for about a second before the engine
        //    swaps the camera state, and the whole of that ran before frame -8.
        //
        // After the one-clock blend landed, the post-edge trace was a single
        // clean move (399.9 -> 123.0 in 18 frames, then flat for four seconds)
        // and the user still reported TWO zoom-ins. If it is not after the
        // edge, it is before it — and the pre-roll could not see it.
        //
        // 150 frames = ~2.5s of mounted frames, so the capture now starts well
        // before the dismount key press. Do not shrink either end again without
        // first saying which part of the event the new window excludes.
        constexpr int kMxRing        = 150;  // ~2.5s BEFORE the state change
        constexpr int kMxTraceFrames = 240;  // ~4s after
        constexpr int kMxMaxSessions = 2;
        static MountEdgeRow sMxRing[kMxRing]{};
        static int          sMxRingN     = 0;
        static MountEdgeRow sMxPrev{};
        static bool         sMxPrevValid = false;
        static int          sMxSessions  = 0;

        if (curState == CameraState::Horseback) {
            sPreExitFreeX   = tps->freeRotation.x;
            sPreExitFreeY   = tps->freeRotation.y;
            sPreExitPosY    = tps->posOffsetExpected.y;
            sPreExitZoomC   = tps->currentZoomOffset;
            if (auto* plPre = RE::PlayerCharacter::GetSingleton())
                sPreExitBodyYaw = plPre->data.angle.z;
            sMxRing[sMxRingN % kMxRing] = captureMountRow();
            ++sMxRingN;
        }

        static int sMountExitFrame = -1;
        // Which edge the live rows belong to, so MOUNTEXIT and MOUNTENTER can
        // share one emitter and still be greppable apart.
        static const char* sMxTag = "MOUNTEXIT";
        if (lastLoggedState == CameraState::Horseback && curState != CameraState::Horseback) {
            // World camera yaw is body + freeRotation.x on BOTH sides of the
            // edge (mounted, the rider's body angle is locked to the horse —
            // measured, see [MOUNTATK]). So this delta IS the visible yaw jump:
            // if it is large, the snap is rotation, not position.
            float postBodyYaw = 0.0f;
            if (auto* plPost = RE::PlayerCharacter::GetSingleton())
                postBodyYaw = plPost->data.angle.z;
            const float preWorldYaw  = sPreExitBodyYaw + sPreExitFreeX;
            const float postWorldYaw = postBodyYaw + tps->freeRotation.x;
            float yawJump = postWorldYaw - preWorldYaw;
            while (yawJump >  3.14159265f) yawJump -= 6.28318530f;
            while (yawJump < -3.14159265f) yawJump += 6.28318530f;
            spdlog::debug("[MOUNTEXIT-PRE] last mounted frame: freeRot=({:.3f},{:.3f}) "
                         "bodyYaw={:.3f} posY={:.1f} zoomC={:.3f} || first frame after: "
                         "freeRot=({:.3f},{:.3f}) bodyYaw={:.3f} posY={:.1f} zoomC={:.3f} "
                         "|| WORLD YAW JUMP = {:.1f} deg",
                         sPreExitFreeX, sPreExitFreeY, sPreExitBodyYaw, sPreExitPosY,
                         sPreExitZoomC,
                         tps->freeRotation.x, tps->freeRotation.y, postBodyYaw,
                         tps->posOffsetExpected.y, tps->currentZoomOffset,
                         yawJump * 180.0f / 3.14159265f);
            auto& tdmX = TDMIntegration::GetSingleton();
            auto* cmX  = RE::ControlMap::GetSingleton();
            spdlog::debug("[MOUNTEXIT] Horseback -> {} | tdm(avail={} locked={} yawOwned={} dmDisabled={}) "
                         "holders(lockYaw={} combatFace={} spim={} dlgBodyYaw={} qtDM={}) | "
                         "moveCtl={} lookCtl={} povCtl={} ctlMask=0x{:X} "
                         "unpauseBlocksMove={} unpausedMenus={}",
                         static_cast<int>(curState),
                         tdmX.IsAvailable(), tdmX.IsAvailable() && tdmX.IsTargetLocked(),
                         tdmX.HasYawControl(), tdmX.HasDirectionalMovementDisabled(),
                         IsLockYawTaken(), IsCombatFaceYawTaken(),
                         ShowPlayerInMenusController::GetSingleton().IsActive(),
                         HookManager::IsDialogueFacingBodyYaw(),
                         MenuUI::HasQuickTuneDMClaim(),
                         cmX ? (cmX->IsMovementControlsEnabled() ? 1 : 0) : -1,
                         cmX ? (cmX->IsLookingControlsEnabled()  ? 1 : 0) : -1,
                         cmX ? (cmX->IsPOVSwitchControlsEnabled() ? 1 : 0) : -1,
                         cmX ? static_cast<std::uint32_t>(cmX->enabledControls.underlying()) : 0u,
                         UnpauseManager::ShouldBlockPlayerMovement(),
                         UnpauseManager::GetUnpausedMenuCount());

            // Replay the mounted frames leading in, then arm the live trace,
            // so one block of log covers both sides of the edge with the same
            // columns. Capped at kMxMaxSessions dismounts per game session.
            if (sMxSessions < kMxMaxSessions) {
                ++sMxSessions;
                sMxPrevValid = false;
                const int have = std::min(sMxRingN, kMxRing);
                for (int i = have; i >= 1; --i) {
                    const auto& row = sMxRing[(sMxRingN - i) % kMxRing];
                    emitMountRow("MOUNTEXIT", -i, row, sMxPrev, sMxPrevValid);
                    sMxPrev      = row;
                    sMxPrevValid = true;
                }
                sMountExitFrame = 0;
                sMxTag          = "MOUNTEXIT";
            }
        }

        // ----- THE OTHER HALF OF "THE HORSE TRANSITION" ---------------------
        //
        // Arm the identical dense trace when the player gets ON. Nothing has
        // ever measured this edge — every probe in this file fires on the
        // dismount — so eight rounds of dismount work went by without anyone
        // being able to say what mounting does. Mounting is not even the same
        // SHAPE of event: the dismount runs kMount -> kPCTransition ->
        // kThirdPerson and is coverable, while the mount goes camState 9 -> 10
        // in ONE step with no transition state, so [CAMCOVER] cannot apply and
        // DDC's channels have to travel the whole way on their own springs.
        //
        // The one thing already visible here is a warning DDC raises itself:
        //   [SPINDIAG] dYaw=+169.4 dPitch=-5.8 ... camState=10
        // on the mount frame, every mount, in every log (-172.5 on 16:39).
        // freeRotation.x is 2.957 rad on entry — the camera sits 169 degrees
        // around from the mount's facing. That may be the legitimate swing
        // behind the horse, or it may be a whip. The columns below are what
        // decides it, and guessing is what has cost this session already.
        if (lastLoggedState != CameraState::Horseback &&
            curState == CameraState::Horseback && sMountExitFrame < 0) {
            if (sMxSessions < kMxMaxSessions) {
                ++sMxSessions;
                sMxPrevValid    = false;
                sMountExitFrame = 0;
                sMxTag          = "MOUNTENTER";
            }
        }

        // ----- [MOUNTCTL] CONTROL-MASK OBSERVATION. IT NEVER WRITES. -------
        //
        // A RESTORE WATCHDOG LIVED HERE FOR ONE BUILD ON 2026-08-26 AND IT
        // CRASHED THE GAME. Do not rebuild it from the reasoning below without
        // reading what its own first measurement said.
        //
        // The theory was: dismounting leaves ControlMap::enabledControls at 0x1
        // (kMovement and nothing else), which matches the user's *"I could not
        // move the camera and could only move around"*, so sample the mask while
        // mounted and put the missing bits back after a dismount.
        //
        // IT REFUTED ITSELF IN THE LINE IT LOGGED:
        //
        //   [MOUNTCTL] ... restored 0x7EE (mask 0x1 -> 0x7EF, mounted sample 0x0)
        //
        // `mounted sample 0x0` means the mounted sampler — which only latches
        // when kLooking is genuinely enabled — never latched once. The player
        // was mounted for six seconds and 150 ring rows prove that code path ran
        // every frame. **So kLooking was already disabled WHILE RIDING**, and
        // the whole premise ("the dismount disables controls") is wrong. The
        // mask reads 0x1 while mounted, while dismounting, and everywhere else
        // it has ever been sampled — which makes 0x1 look like this load
        // order's BASELINE rather than a lockout, and means DDC has no business
        // "repairing" it.
        //
        // It then wrote engine control state from inside the camera update hook.
        // ToggleControls dispatches a BSTEventSource<UserEventEnabled> to every
        // listener in the load order, so that is a re-entrant call into other
        // mods from a camera state Update. The game died about a second later.
        // TWO separate reasons to not do it: the premise was false, and the
        // mechanism was unsafe. If a repair is ever justified it must be
        // deferred through SKSE::GetTaskInterface()->AddTask, never called here.
        //
        // WHAT REPLACES IT: measurement only, so the next capture can settle
        // whether 0x1 is a baseline or a real lockout. Samples the mask on the
        // mount edge, on the dismount edge, and at a few points afterwards.
        // Writes nothing, ever.
        {
            static int sCtlObsFrames  = 0;
            static int sCtlObsLogged  = 0;
            static bool sCtlWasMounted = false;

            auto* cm = RE::ControlMap::GetSingleton();
            if (cm && sCtlObsLogged < 40) {
                const std::uint32_t mask =
                    static_cast<std::uint32_t>(cm->enabledControls.underlying());
                const bool mountedNow = (curState == CameraState::Horseback);

                auto emit = [&](const char* a_when) {
                    ++sCtlObsLogged;
                    spdlog::debug("[MOUNTCTL-OBS] {} | ctlMask=0x{:X} move={} look={} pov={} "
                                 "menu={} fight={} | menuBlocking={}",
                                 a_when, mask,
                                 cm->IsMovementControlsEnabled() ? 1 : 0,
                                 cm->IsLookingControlsEnabled()  ? 1 : 0,
                                 cm->IsPOVSwitchControlsEnabled() ? 1 : 0,
                                 cm->IsMenuControlsEnabled()     ? 1 : 0,
                                 cm->IsFightingControlsEnabled() ? 1 : 0,
                                 menuBlocking);
                };

                if (mountedNow && !sCtlWasMounted) {
                    emit("MOUNTED (edge)");
                } else if (!mountedNow && sCtlWasMounted) {
                    emit("DISMOUNT (edge)");
                    sCtlObsFrames = 1;
                } else if (sCtlObsFrames > 0) {
                    // +30 / +60 / +120 / +240 after the dismount. If the mask is
                    // identical at every one of these AND at the mounted edge,
                    // 0x1 is the baseline and there is nothing here to fix.
                    ++sCtlObsFrames;
                    const int n = sCtlObsFrames;
                    if (n == 30 || n == 60 || n == 120 || n == 240) {
                        emit(n == 30  ? "dismount +30"
                           : n == 60  ? "dismount +60"
                           : n == 120 ? "dismount +120"
                                      : "dismount +240");
                    }
                    if (n >= 240) sCtlObsFrames = 0;
                }
                sCtlWasMounted = mountedNow;
            }
        }
        // ROUND 2. The first capture came back with every DDC claim clean and
        // moveCtl=1 â€” so the dismount freeze is NOT input being eaten, and the
        // user's own workaround says where it really is: "going into first
        // person and then back into third fixes it". A POV round-trip runs
        // ThirdPersonState::Begin(), which resets freeRotation, the posOffsets
        // and the zoom. So one of those is stuck, and this samples all of them
        // for a few seconds after the dismount â€” the stuck value may not be
        // written until after the edge, which is why the edge line alone could
        // never have caught it.
        //
        // camState is printed as an INDEX INTO cameraStates[] rather than the
        // raw id, because the interesting possibility is that the engine left
        // the MOUNT camera state active on a player who is no longer mounted.
        if (sMountExitFrame >= 0) {
            ++sMountExitFrame;
            const auto row = captureMountRow();
            emitMountRow(sMxTag, sMountExitFrame, row, sMxPrev, sMxPrevValid);
            sMxPrev      = row;
            sMxPrevValid = true;

            // The ctlMask lockdown ([[dismount-breaks-look-input]]) is a
            // SEPARATE bug that the user traced to loading a save made while
            // mounted, but it corrupts any rotation reading taken during it —
            // the player cannot look, so a moving freeRotation is not theirs.
            // Print it once at the end of the window so a contaminated capture
            // can be recognised as one instead of adjudicated as camera work.
            if (sMountExitFrame >= kMxTraceFrames) {
                auto* cmS = RE::ControlMap::GetSingleton();
                int slot = -1;
                if (a_camera && a_camera->currentState) {
                    for (int i = 0; i < RE::CameraStates::kTotal; ++i) {
                        if (a_camera->cameraStates[i].get() == a_camera->currentState.get()) {
                            slot = i;
                            break;
                        }
                    }
                }
                spdlog::debug("[MOUNTEXIT-END] camSlot={} freeRotEnabled={} lookCtl={} "
                             "ctlMask=0x{:X} | zoom t={:.3f} c={:.3f} saved={:.3f} "
                             "pitchZoom={:.1f} | posAct=({:.1f},{:.1f},{:.1f})",
                             slot, tps->freeRotationEnabled ? 1 : 0,
                             cmS ? (cmS->IsLookingControlsEnabled() ? 1 : 0) : -1,
                             cmS ? static_cast<std::uint32_t>(cmS->enabledControls.underlying()) : 0u,
                             tps->targetZoomOffset, tps->currentZoomOffset,
                             tps->savedZoomOffset, tps->pitchZoomOffset,
                             tps->posOffsetActual.x, tps->posOffsetActual.y,
                             tps->posOffsetActual.z);
                sMountExitFrame = -1;
            }
        }

        if (curState != lastLoggedState || curSubState != lastLoggedSubState) {
            lastLoggedState    = curState;
            lastLoggedSubState = curSubState;
            const char* subName = "None";
            switch (curSubState) {
            case CameraSubState::Sprint:   subName = "Sprint"; break;
            case CameraSubState::Swimming: subName = "Swimming"; break;
            case CameraSubState::Attack:   subName = "Attack"; break;
            case CameraSubState::Sneak:    subName = "Sneak"; break;
            case CameraSubState::Shout:    subName = "Shout"; break;
            default:                    subName = "None";  break;
            }
            spdlog::debug("CameraController: state={} sub={} sideOffset={:.1f} height={:.1f} zoom={:.1f} fov={:.1f}",
                         static_cast<int>(curState), subName,
                         effectiveSide, effectiveHeight, effectiveZoom, effectiveFOV);
        }
    }

    void CameraController::ApplyFOV(RE::PlayerCamera* a_camera, float fov)
    {
        // Never write a non-finite FOV: a NaN/Inf here blows the projection to
        // infinity and never self-heals (the skip-check below fails open on
        // NaN, since NaN < 0.01 is false). Bail and keep the last good FOV.
        if (!std::isfinite(fov)) return;
        // worldFOV <= 0 inverts the projection â€” the world renders inside-out â€”
        // and, like a NaN, the engine never recovers from it on its own. A
        // channel-spring overshoot during a large DOWNWARD FOV retarget (e.g. a
        // shout pulling FOV down from a high value: the Â±velocity clamp in
        // stepMotion suppresses the damping term, so the position can sail past
        // the target and cross zero) is enough to flip the world. Floor to a
        // sane positive minimum so a transient overshoot can never invert the
        // projection. Mirrors the `> 0` guards on the bleedout / vanity FOV
        // writers in HookManager. (Diagnosed 2026-06-19: negative worldFOV
        // ~-11.5 captured by [SPINDIAG] during whirlwind-sprint at high FOV +
        // max looseness.)
        constexpr float kMinWorldFov = 1.0f;
        constexpr float kMaxWorldFov = 179.0f;
        fov = std::clamp(fov, kMinWorldFov, kMaxWorldFov);
        // Publish before the skip: the value is "the FOV this camera should be
        // at", and that is just as true on a frame where it already matches.
        // The Tween hold in HookedUpdateCameraPost reads it, and it must not go
        // stale simply because the FOV settled.
        sLastAppliedWorldFov      = fov;
        sLastAppliedWorldFovValid = true;
        if (std::abs(fov - a_camera->worldFOV) < 0.01f) return;
        a_camera->worldFOV = fov;
    }

    float CameraController::GetLastAppliedWorldFov(bool& a_outValid)
    {
        a_outValid = sLastAppliedWorldFovValid;
        return sLastAppliedWorldFov;
    }

    void CameraController::ApplyZoom(RE::ThirdPersonState* a_tps, float zoom, bool dragonMode)
    {
        const float profileOffset = zoom * 0.01f;

        if (!zoomBaseInitialized) {
            // Pin zoomBase to vanilla's fMinCurrentZoom (-0.2). That's the
            // engine value we observed at vanilla's closest zoom via the
            // diagnostic snapshot, so slider 0 â†’ vanilla closest exactly,
            // independent of the user's pre-plugin scroll-wheel position.
            zoomBase = -0.2f;
            zoomBaseInitialized = true;
            float finalTarget = zoomBase + profileOffset;
            // First-init only: prime BOTH so the engine doesn't lerp from a
            // wildly different starting position. Subsequent frames write
            // only targetZoomOffset and let the engine smooth currentZoomOffset.
            a_tps->targetZoomOffset  = finalTarget;
            a_tps->currentZoomOffset = finalTarget;
            lastWrittenTargetZoom    = finalTarget;
            lastAppliedZoomOffset    = profileOffset;
            lastZoom = zoom;
            return;
        }

        // Hard zoom lock: every frame we force both targetZoomOffset and
        // currentZoomOffset back to (vanilla-baseline + profile.zoom).
        //
        // Zoom-input hold detection for POV entry: the engine decrements
        // targetZoomOffset each frame while the user pulls R3 or scrolls.
        // We read the per-frame delta (engineTarget - lastWrittenTargetZoom)
        // to see if the user is actively inputting zoom-in. If that input
        // is sustained for ~0.3 seconds, call PlayerCamera::ForceFirstPerson
        // directly (bypasses zoom axis entirely, so visible zoom stays
        // locked). Fires at most once per sustained hold via the sFired
        // latch â€” resets the moment input stops, so each new pull is a
        // fresh gesture. A cooldown blocks additional fires for ~1s after
        // a toggle so the engine's transition can complete without
        // re-triggering.
        //
        // Dragon mode skips locking (needs negative zoom).
        lastZoom = zoom;

        const float engineTarget = a_tps->targetZoomOffset;
        const float inputDelta   = engineTarget - lastWrittenTargetZoom;

        static float sZoomInHoldTime = 0.0f;
        static bool  sFiredThisHold  = false;
        static float sFireCooldown   = 0.0f;

        if (sFireCooldown > 0.0f) sFireCooldown -= smoothDt;

        // POV toggle is handled by the R3-release input sink
        // (HookManager::R3ReleaseSink). That fires ForceFirstPerson /
        // ForceThirdPerson on the actual button-release edge. Nothing to
        // do here â€” the input delta we computed above is only used as a
        // diagnostic marker.
        (void)inputDelta;
        (void)sZoomInHoldTime;
        (void)sFiredThisHold;
        (void)sFireCooldown;

        float finalTarget = zoomBase + profileOffset;
        // Floor stays at vanilla-closest, ALWAYS. A 2026-08-16 experiment
        // opened it and that threw the camera "10 feet forward of the real
        // action", reading as a teleport: pulling the orbit distance below
        // zero walks the camera THROUGH the player and out the far side.
        // Never open this clamp for an orbit-model effect â€” a shot that
        // needs to sit closer than the player's own body needs direct
        // camera PLACEMENT, not zoom.
        const float zoomFloor = zoomBase;
        if (!dragonMode && finalTarget < zoomFloor) finalTarget = zoomFloor;
        // Write both target and current. Engine's zoom lerp rate (matches
        // posOffset's ~0.027/frame) is too slow to rely on for profile
        // transitions; the Layer-2 EMA on `pZoom` (see Update) already
        // smoothed `effectiveZoom` to here.
        a_tps->targetZoomOffset  = finalTarget;
        a_tps->currentZoomOffset = finalTarget;
        lastWrittenTargetZoom    = finalTarget;
        lastAppliedZoomOffset    = finalTarget - zoomBase;
        sLastAppliedEngineZoom      = finalTarget;
        sLastAppliedEngineZoomValid = true;
    }

    void CameraController::ApplyPitchOffset(RE::NiQuaternion& a_rotation)
    {
        if (currentPitchOffset == 0.0f) return;

        float pitch = currentPitchOffset * 0.01f;
        float half = pitch * 0.5f;
        float pw = std::cos(half);
        float px = std::sin(half);

        RE::NiQuaternion result;
        result.w = a_rotation.w * pw - a_rotation.x * px;
        result.x = a_rotation.w * px + a_rotation.x * pw;
        result.y = a_rotation.y * pw + a_rotation.z * px;
        result.z = a_rotation.z * pw - a_rotation.y * px;
        a_rotation = result;
    }

    float CameraController::AspectCorrection()
    {
        return GetAspectFactor();
    }

    void CameraController::UpdateForwardVector(RE::PlayerCharacter* a_player)
    {
        float yaw = a_player->GetAngleZ();
        forwardDir.x = std::sin(yaw);
        forwardDir.y = std::cos(yaw);
        forwardDir.z = 0.0f;
    }

    // Pulls the engine-composed cameraRoot + niCamera transforms into the
    // latest m_diagDlgPerFrameBuf row. Two callers per frame
    // (HookedUpdateCameraPost, HookedNiCameraUpdateWorldData); the second
    // overwrites the first's values, giving us truly-final pre-render state.
    // If cRoot* looks smooth but ni* staircases, the engine recompose
    // between hooks is the stutter source.
    void CameraController::CaptureDlgPerFrameLate(RE::PlayerCamera* a_camera,
                                                  DlgLateCaptureSite a_site)
    {
        // Always-on snapshot of the final rendered camera state, kept even
        // during gameplay so the dialogue entry edge can log the LAST
        // gameplay frame (handoff diagnosis). Runs before the dialogue-
        // active guard below.
        if (a_camera) {
            if (auto* cr = a_camera->cameraRoot.get()) {
                m_dlgLastRendered.crWorldTx = cr->world.translate.x;
                m_dlgLastRendered.crWorldTy = cr->world.translate.y;
                m_dlgLastRendered.crWorldTz = cr->world.translate.z;
                // FIND THE NiCamera, DO NOT ASSUME IT IS children[0].
                //
                // It was assumed for months, and children[0] is evidently
                // something else that sits exactly on the root: every
                // [MOUNTEXIT] row of the 2026-08-27 17:25 capture printed
                // cam=(...) and piv=(...) BYTE-IDENTICAL, so the derived dOff
                // column read 0.00 on every frame of a dismount the user was
                // watching snap. The same defect is why distRoot and distNi
                // have always printed identical in [TWEENCAM] — recorded in
                // [[a-probe-that-cannot-see-its-subject]] and left unfixed.
                //
                // CameraNoiseController::FindNiCamera has done this correctly
                // all along: walk the children and skyrim_cast. Same search
                // here, so the pivot and the rendered camera are two different
                // nodes and the offset between them is finally measurable.
                RE::NiAVObject* ni = nullptr;
                if (auto* asNode = cr->AsNode()) {
                    for (auto& child : asNode->GetChildren()) {
                        if (auto* c = child.get()) {
                            if (skyrim_cast<RE::NiCamera*>(c)) { ni = c; break; }
                        }
                    }
                }
                {
                    if (ni) {
                        ni->world.rotate.ToEulerAnglesXYZ(
                            m_dlgLastRendered.niEulerX,
                            m_dlgLastRendered.niEulerY,
                            m_dlgLastRendered.niEulerZ);
                        // The actual rendered camera point (see header note).
                        m_dlgLastRendered.niWorldX = ni->world.translate.x;
                        m_dlgLastRendered.niWorldY = ni->world.translate.y;
                        m_dlgLastRendered.niWorldZ = ni->world.translate.z;
                    }
                }
                if (auto* pcN = RE::PlayerCamera::GetSingleton()) {
                    m_dlgLastRendered.worldFOV = pcN->worldFOV;
                }
                // Same-instant player position, so camera-to-player distance is
                // measurable rather than inferred.
                if (auto* plW = RE::PlayerCharacter::GetSingleton()) {
                    const auto& pw = plW->GetPosition();
                    m_dlgLastRendered.plWorldX = pw.x;
                    m_dlgLastRendered.plWorldY = pw.y;
                    m_dlgLastRendered.plWorldZ = pw.z;
                }
                m_dlgLastRendered.cameraStateId =
                    a_camera->currentState ? static_cast<int>(a_camera->currentState->id) : -1;
                m_dlgLastRendered.valid = true;
            }
        }

        if (!m_diagDlgPerFrameActive) return;
        if (m_diagDlgPerFrameCount <= 0) return;
        if (!a_camera) return;

        auto& r = m_diagDlgPerFrameBuf[m_diagDlgPerFrameCount - 1];
        r.lateWriteCount += 1;
        // Tag of which hook wrote LAST (1 = UpdateCameraPost, 2 = NiCameraUpdateWorldData).
        // Lets us tell at a glance whether NiCamera::UpdateWorldData fired
        // for the player niCam this frame (expected: cameraStateId carries
        // the post-write site tag in its low byte).
        const int siteTag = static_cast<int>(a_site);

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            const auto& pp = player->GetPosition();
            r.playerPosX = pp.x;
            r.playerPosY = pp.y;
            r.playerPosZ = pp.z;
        }

        if (a_camera->currentState) {
            // Pack state-id in high bits and site-tag in low byte so a single
            // int field exposes both pieces (avoids growing the struct by another
            // word just for the tag).
            const int stateId = static_cast<int>(a_camera->currentState->id);
            r.cameraStateId = (stateId << 8) | (siteTag & 0xFF);
        } else {
            r.cameraStateId = (-1 << 8) | (siteTag & 0xFF);
        }

        auto* cameraRoot = a_camera->cameraRoot.get();
        if (!cameraRoot) return;

        r.crLocalTx = cameraRoot->local.translate.x;
        r.crLocalTy = cameraRoot->local.translate.y;
        r.crLocalTz = cameraRoot->local.translate.z;
        r.crWorldTx = cameraRoot->world.translate.x;
        r.crWorldTy = cameraRoot->world.translate.y;
        r.crWorldTz = cameraRoot->world.translate.z;
        cameraRoot->world.rotate.ToEulerAnglesXYZ(r.crEulerX, r.crEulerY, r.crEulerZ);

        if (auto* asNode = cameraRoot->AsNode();
            asNode && !asNode->GetChildren().empty())
        {
            if (auto* ni = asNode->GetChildren()[0].get()) {
                r.niLocalTx = ni->local.translate.x;
                r.niLocalTy = ni->local.translate.y;
                r.niLocalTz = ni->local.translate.z;
                r.niWorldTx = ni->world.translate.x;
                r.niWorldTy = ni->world.translate.y;
                r.niWorldTz = ni->world.translate.z;
                ni->world.rotate.ToEulerAnglesXYZ(r.niEulerX, r.niEulerY, r.niEulerZ);
            }
        }
    }
}
