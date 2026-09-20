#pragma once

#include "vendor/PerlinNoise.hpp"
#include "Core/NoiseTransition.h"
#include "Core/FirstPersonNoise.h"

namespace DietDrCamera
{
    // Camera noise (v3, state-keyed). Layers additive Perlin shake onto
    // cameraRoot->local based on the player's current Categories state.
    // Resolution: read CameraController's last-resolved CameraProfile
    // pointer, look up the matching per-state NoiseProfile via
    // SettingsManager::ResolveStateNoise; if the state has no
    // customization (or the toggle is off) fall back to globalNoise.
    // Suppressed during 1p, paused menus, killmoves, and non-3p states.
    class CameraNoiseController
    {
    public:
        [[nodiscard]] static CameraNoiseController& GetSingleton();

        void InstallHook();  // trampoline TESCamera::Update post-call

        // Called from the thunk after the engine's camera update runs.
        void OnCameraUpdate(RE::TESCamera* a_camera);

        // NPC race changes use the player's corresponding transform/revert
        // entry, scaled by NPC Noise > Transformations. The event and poll
        // share previous-form memory so a race change is consumed once.
        static void NotifyNpcTransformBeat(bool a_vampireLord, const RE::NiPoint3& a_pos, bool a_revert = false);
        static void NotifyNpcRaceChange(RE::Actor* a_actor);
        // Existing arrow update/contact hooks supply actual flight positions.
        static void NotifyNpcArcheryFlight(RE::Projectile* a_projectile, const RE::NiPoint3& a_pos,
                                          bool a_terminal = false, bool a_hitPlayer = false);
        static void NotifyNpcMagicFlight(RE::Projectile* a_projectile, const RE::NiPoint3& a_pos,
                                        bool a_terminal = false, bool a_hitPlayer = false);
        static void NotifyNpcMagicShot(RE::ObjectRefHandle a_shooter, RE::FormID a_spell, RE::FormID a_projectile);

        // This frame's TOTAL rotation + translation the 3p noise stack
        // applied to cameraRoot (ambient layers, beats, head bob, Repulse
        // kicks). The rendered camera = clean * applied — but the ENGINE
        // aims and launches projectiles from the CLEAN basis; it never sees
        // these offsets. Aim/trace consumers that must match the actual
        // flight (the camera-aim snapshot, the charge-preview re-anchor)
        // recover the clean basis: rot_clean = rot_rendered * appliedᵀ,
        // pos_clean = pos_rendered − appliedTrans. False when nothing was
        // applied this frame (callers use the rendered values as-is).
        // Field-driven: the post-shot Repulse pitch kick was bending every
        // aim read for ~0.4s — "trouble with verticals from repulse".
        [[nodiscard]] static bool GetAppliedCameraOffset3p(RE::NiMatrix3& a_outRot,
                                                           RE::NiPoint3& a_outTrans);

        // The menu just CHANGED a noise slider (called by the noise slider
        // renderers every frame a value actually moved). While this is fresh,
        // a change in the resolved source's SIGNATURE is treated as the live
        // texture being retuned in place rather than a texture change to
        // hide: the crossfade does not re-arm, and the live layer simply
        // tracks the slider. Without it, every tick of a drag re-armed the
        // fade, and the mid-flight collapse handed the dominant layer the
        // incoming layer's freshly DECORRELATED clock — one Perlin phase jump
        // per tick, which on a Speed drag read as the noise briefly speeding
        // up dramatically before settling.
        static void NotifyLiveNoiseEdit();

    private:
        CameraNoiseController();

        [[nodiscard]] bool IsSuppressed(RE::PlayerCamera* a_pc, bool a_bypassAnimated = false) const;

        void ApplyPerlin(RE::TESCamera* a_camera, float a_dt, float a_ampMul, float a_speedMul,
                         float a_transMul, float a_rotMul, float a_freqMul);

        // Sample one ambient layer's displacement for this frame at the given
        // clock positions (does NOT advance the clocks). Two-band drift+jitter
        // model shared by ApplyPerlin (cinematic single layer, member clocks)
        // and ApplyAmbientCrossfade (two layers, per-layer clocks). Outputs a
        // translation offset plus a rotation as axis + angle.
        void SampleAmbientLayer(double a_clockTrans, double a_clockRot,
                                float a_sway, float a_tilt, float a_amp,
                                float a_driftJitter, float a_roughness,
                                RE::NiPoint3& a_outTrans, RE::NiPoint3& a_outRotAxis,
                                float& a_outRotTheta) const;

        // Apply curAmbient over the retained outgoing blend, weighted by
        // ambientXfade, to cameraRoot->local. Advances all audible layers'
        // clocks at their own Speeds. a_masterW scales every layer weight —
        // the cinematic path fade renders this side at partial weight while a
        // dragon/centurion impact fades in or out.
        void ApplyAmbientCrossfade(RE::TESCamera* a_camera, float a_dt, float a_masterW = 1.0f);

        // 1p variant. Computes this frame's noise rotation and stores it as
        // pending; ApplyPending1pNoise (renderer-side NiCamera::UpdateWorldData
        // hook, post-_original) multiplies it onto the freshly recomposed
        // world.rotate at every fire. That is the ONLY apply path — there is no
        // direct write. Every fire recomposes a clean base first, so the
        // re-apply is idempotent: it cannot double and cannot drop, no matter
        // how many recomposes fire in a frame or when (cast head-bone
        // propagation, Quick Tune FOV/zoom preview, FOV transition springs).
        // The earlier direct/deferred hybrid predicted which frames had a
        // post-write recompose and mode-switched — every prediction scheme
        // produced either a frozen shake (missed wipe) or a boundary jerk
        // (mode transition); this removes the prediction entirely.
        // a_pitchKickRad: optional one-shot pitch-up rotation in radians,
        // applied as an absolute (non-Perlin) bias for cannon-style
        // recoil impulses. 0 = no kick. a_kickAxis: camera-local axis of
        // that kick — {1,0,0} is a pure nod; per-shot rolls tilt it so
        // consecutive casts land up-left / up-right / near-straight.
        // a_useDeferredApply: retained for call-site compatibility; unused.
        void ApplyPerlin1p(float a_dt, float a_ampMul, float a_speedMul,
                           float a_rotMul, float a_freqMul,
                           float a_pitchKickRad   = 0.0f,
                           const RE::NiPoint3& a_kickAxis = RE::NiPoint3{ 1.0f, 0.0f, 0.0f },
                           bool  a_useDeferredApply = false);

        // Scans nearby dragons/centurions and advances the cinematic-shake
        // envelope (asymmetric attack/decay) one frame. POV-agnostic so both
        // the 3p and 1p apply paths can drive cinematic shakes. Updates the
        // dragonShake* members; returns the current shake amplitude and writes
        // the springed speed to a_outSpeed.
        // a_fp selects the FIRST PERSON half of every creature source
        // (2026-09-06 per-view split); the envelope itself is shared.
        float UpdateDragonShake(RE::PlayerCharacter* a_player, float a_dt, float& a_outSpeed,
                                bool a_fp);

        // Direction-aware target-switch kick (lock-on cinematic flourish).
    public:
        // Called from HookedNiCameraUpdateWorldData (post-engine
        // UpdateWorldData) so the noise rotation is the LAST write to
        // niCam->world.rotate each frame — survives the engine's
        // head-bone propagation that overwrites earlier writes during
        // cast animations. No-op when no pending noise this frame.
        void ApplyPending1pNoise(RE::NiCamera* a_niCam);

        // This frame's 1p noise as a single rotation delta, for callers that
        // own the camera matrix and must compose noise themselves (the 1p
        // dialogue face-lock assigns world.rotate outright, which would wipe a
        // direct noise write — it multiplies this delta onto its matrix instead
        // so the locked view keeps its life). Returns false when there's no
        // meaningful noise this frame (out left untouched).
        [[nodiscard]] bool Get1pNoiseDelta(RE::NiMatrix3& a_out) const;

        // Multiply this frame's cached 1p noise delta onto a_niCam's current
        // world.rotate and refresh its world-to-screen matrix. Used by the
        // dialogue face-lock's release tail to keep noise alive while the engine
        // re-takes the camera and this render hook is still wiping the direct-
        // path write. No-op when there's no noise this frame, or when a deferred
        // pending apply already owns it (ApplyPending1pNoise handles that case).
        void Reapply1pNoiseOnTop(RE::NiCamera* a_niCam);

    private:
        // Per-frame pending 1p noise state. ApplyPerlin1p computes the
        // composed rotation and stores it here; ApplyPending1pNoise reapplies it
        // after each clean engine recompose until the next frame replaces it.
        RE::NiMatrix3 pendingNoiseRotation{};
        bool         pendingNoiseValid  = false;

        // Head Bobbing, first person. Kept SEPARATE from the noise pending
        // above because the two are independently switchable: a player can run
        // head bob with every noise slider at zero, and on those frames the 1p
        // branch never reaches the noise apply at all.
        //
        // ROTATIONAL, like every other 1p effect here. A translational bob is
        // the obvious implementation and it is the one that does not work:
        // ApplyPerlin1p's header note records that `niCam->world.translate` is
        // clobbered by the engine's head-bone-driven update, which is why 1p
        // shake has always been pure rotation. Pitch about local X, roll about
        // local Y — the same axis convention the FoF recoil kick uses.
        // Re-applying is idempotent (each recompose rebuilds a clean base), so
        // multiple UpdateWorldData fires in a frame can neither double it nor
        // drop it.
        float pendingBobPitch = 0.0f;
        float pendingBobRoll  = 0.0f;
        float pendingBobYaw   = 0.0f;
        bool  pendingBobValid = false;

        // Set by ApplyPending1pNoise when the late hook just applied
        // pending noise this frame. Read by ApplyPerlin1p's direct-
        // write path so the steady-state direct write is suppressed
        // on the animated→steady boundary frame (where the late hook
        // is consuming the last animated pending). Cleared at end of
        // OnCameraUpdate so the next frame starts fresh.
        bool         pendingJustApplied = false;

        // This frame's composed 1p noise rotation (all textures, then pitchKick), cached
        // every frame by ApplyPerlin1p — direct OR deferred path — so the dialogue
        // face-lock can multiply it onto its render matrix. Validity is cleared on
        // frames with no noise so a stale delta can't keep shaking a locked view.
        RE::NiMatrix3 m1pNoiseDelta{};
        bool          m1pNoiseDeltaValid = false;

        // Perlin — 6 independent seeds (3 axes x 2 uses: trans + rot).
        dietdrcamera::Perlin1D perlinTransX;
        dietdrcamera::Perlin1D perlinTransY;
        dietdrcamera::Perlin1D perlinTransZ;
        dietdrcamera::Perlin1D perlinRotX;
        dietdrcamera::Perlin1D perlinRotY;
        dietdrcamera::Perlin1D perlinRotZ;
        double timeTrans = 0.0;
        double timeRot   = 0.0;
        // Two-band noise character for the current frame's apply. Set from the
        // resolved NoiseProfile (+ player state) right before ApplyPerlin /
        // ApplyPerlin1p so the generator doesn't need extra signature args.
        // (Read only by the cinematic single-layer ApplyPerlin path and the 1p
        // path; the ambient crossfade passes its per-layer character directly.)
        float curDriftJitter = 0.35f;  // 0=drift .. 1=jitter
        float curRoughness   = 0.45f;  // fractal persistence

        // --- Ambient state-noise crossfade (transition rework) ---
        // In steady state only curAmbient applies (ambientXfade == 1). When the
        // resolved noise source changes, its actual outgoing blend is retained
        // and ambientXfade resets to 0, then ramps back to 1 — cross-fading the
        // outgoing texture out while the incoming one fades in. Crucially, each
        // layer keeps its OWN clock advancing at its OWN Speed for the whole
        // fade, so the blend never passes through the high-amplitude /
        // low-frequency "wallow" a single eased-tempo clock produced when two
        // states had very different Speed values. This is what lets each state
        // carry its own feel and still transition smoothly into any other.
        struct AmbientLayer
        {
            float  amp         = 0.0f;    // effective amplitude (post shout/roar envelope + enable gate)
            float  speed       = 0.0f;    // effective clock rate (Speed)
            float  sway        = 0.0f;    // Position Shake weight (translation)
            float  tilt        = 0.0f;    // Rotation Shake weight
            float  driftJitter = 0.35f;   // 0 = drift, 1 = jitter
            float  roughness   = 0.45f;   // fractal persistence
            double clockTrans  = 0.0;     // independent translation clock
            double clockRot    = 0.0;     // independent rotation clock
            // World-space unit vector from the camera toward whatever is
            // CAUSING this layer, with how hard to bias the sampled motion
            // onto it. Zero bias (or a zero vector) is plain isotropic noise,
            // which is what every layer without a world source wants.
            RE::NiPoint3 srcDir{};
            float        srcBias = 0.0f;
        };
        AmbientLayer curAmbient;
        NoiseTransitionHistory<AmbientLayer> ambientOutgoing;
        // Representative values for duration selection and diagnostics only.
        // Rendering uses the real outgoing layers in ambientOutgoing.
        AmbientLayer prevAmbient;
        // One-shot additive "beat" layer (Cinematic Effects → Sheathing /
        // Unsheathing). Sampled at full weight ON TOP of the crossfade, sharing
        // curAmbient's clocks so it is phase-locked to the live texture. It has
        // to be a third layer rather than a bump to curAmbient/prevAmbient:
        // prevAmbient is only reassigned on a signature change, so adding to it
        // every frame would accumulate, and adding to curAmbient alone would
        // scale the beat by ambientXfade — which is exactly 0 at the moment a
        // weapon draw changes state and re-arms the crossfade. Its own amp is
        // already enveloped, so at rest it contributes nothing and there is no
        // discontinuity when it engages. Rebuilt every frame by OnCameraUpdate.
        AmbientLayer drawBeat;
        bool         drawBeatValid = false;
        // Same construction for the Werewolf / Vampire Lord transformation
        // shake. It used to run through applyEnvelope, which sets
        // cinematicShake and swaps the WHOLE frame from the two-layer ambient
        // crossfade to the single-layer ApplyPerlin - a different clock, a
        // different drift/jitter mix, a different roughness. That swap is a
        // texture discontinuity at the moment the shake engages and another
        // when it lets go, which is exactly what a transformation reads as:
        // two jolts bracketing the effect rather than one swell. Additive
        // layer, own clock, envelope that starts and ends at zero - the
        // frame's texture never changes, only its size.
        AmbientLayer transBeat;
        bool         transBeatValid = false;
        // Fourth additive layer, shared by the three event-driven beats
        // (Vampire Lord Bats, Reanimation, Summoning). Deliberately NOT folded
        // into transBeat: a reanimation or a summon can land while a
        // transformation envelope is still running, and sharing one layer
        // would make the louder of the two silence the other outright. Same
        // construction and the same guarantees (own clock, envelope starting
        // and ending at exactly zero, so no discontinuity either way).
        AmbientLayer eventBeat;
        bool         eventBeatValid = false;
        // Dragon / centurion impact shake. Additive, like every other
        // cinematic source. It used to be a whole-frame PATH SWAP — while a
        // dragon was loud the ambient crossfade was weighted to nothing and
        // the frame was redrawn on the dragon's clock and character — so a
        // flyover silently rewrote whatever your state noise was doing,
        // shouts included. As its own layer it decorates instead of replacing.
        AmbientLayer dragonBeat;
        bool         dragonBeatValid = false;
        // Continuous additive blend (proximity ambience / NPC casting). Two
        // texture slots + an internal mini-crossfade — a miniature of
        // curAmbient/prevAmbient: amp modulates freely per-frame (it is NOT
        // part of the ambient signature), but a change of dominant SOURCE
        // snapshots cur into prev and refades, so the texture never steps.
        // Both slots' clocks advance every frame (like the beat layers) so no
        // slot ever samples a frozen Perlin phase. Unlike the beats it does
        // NOT copy curAmbient first — a world source must not inherit the
        // live state's tilt/sway floor.
        struct ContinuousBlend
        {
            AmbientLayer cur;
            AmbientLayer prev;
            float        xfade  = 1.0f;   // 1 = all cur
            int          domKey = -1;     // dominance signature (prox entry idx / npc school id)
            bool         valid  = false;  // any amp this frame (apply gate)
        };
        ContinuousBlend npcBlend;

        // Normal / power attack. Fired by the swing rather than resolved as a
        // state, so it punctuates whatever you were already doing instead of
        // replacing it until you sheathe.
        //
        // A two-layer blend, not one layer: power and normal attacks are
        // different cells, and a combo swaps between them mid-envelope. Held in
        // a single layer, that swap replaced amp/tilt/sway/speed/roughness in
        // ONE frame while the envelope carried on smoothly — a hard texture cut
        // inside a continuous gesture, which is the worst place to put one.
        // Same primitive the NPC-casting layer uses: the outgoing cell keeps
        // playing on its own clock while the incoming one fades in.
        ContinuousBlend attackBlend;

        // First-person history uses the same interruption handling as 3p.
        // Outgoing textures retain their parameters, clocks and unfinished fade
        // progress. The current layer is still the live spring-eased state.
        FirstPersonNoiseLayer fp1pCurrent; // Last actually sampled live layer.
        NoiseTransitionHistory<FirstPersonNoiseLayer> fp1pOutgoing;
        FirstPersonNoiseSources fp1pSources;
        // Outgoing RMS amplitude for the apply gate and diagnostics.
        float  fp1pPrevAmp   = 0.0f;
        float  fp1pXfade     = 1.0f;   // 1 = all current
        // 0 = all prevAmbient, 1 = all curAmbient. Seeded at 0 so the very
        // first noise application after a load gently fades in from the
        // (silent) prevAmbient instead of snapping to full amp; it reaches 1
        // within ~0.2s and stays there until a real state transition re-arms it.
        float ambientXfade = 0.0f;
        // How fast ambientXfade closes. The phase advances LINEARLY at this
        // rate (phase units per second, so 1/rate = total fade seconds) and the
        // apply sites map it through smoothstep + equal-power sin/cos weights —
        // zero-slope onset and landing, constant perceived energy through the
        // middle. The old exponential phase + linear weights had its steepest
        // change on the transition frame AND dipped ~30% mid-fade (linear
        // crossfade of two uncorrelated textures), which read as a pump at
        // every state change. An ordinary state change wants this snappy; a
        // shout handing its texture back to the parent state wants the user's
        // Fade Duration instead, because it is coming down off a peak several
        // times the parent's own level and 0.35s of that reads as a cut.
        static constexpr float kAmbientXfadeDur         = 0.35f;
        static constexpr float kAmbientXfadeRateDefault = 1.0f / kAmbientXfadeDur;
        float ambientXfadeRate = kAmbientXfadeRateDefault;

        // Signature of last frame's resolved source profile (RAW fields, before
        // the shout amplitude ride) — a change beyond epsilon arms a crossfade.
        // Using raw Speed/character/amp means a shout's per-frame envelope
        // doesn't masquerade as a state change, while two states that differ
        // only in how loud they are still get a crossfade rather than a step.
        float ambientSigSpeed = 0.0f;
        float ambientSigSway  = 0.0f;
        float ambientSigTilt  = 0.0f;
        float ambientSigDrift = 0.0f;
        float ambientSigRough = 0.0f;
        float ambientSigAmp   = 0.0f;
        bool  ambientHasSig   = false;

        std::chrono::steady_clock::time_point lastTick;
        bool                                  hasLastTick = false;

        // Smoothed dragon-shake amp/speed. Rising edges snap on
        // instantly so impulses (bite, fireball, landing) read as
        // sharp impacts; falling edges decay using a per-kind tau so
        // a bite snaps off but a landing keeps rumbling. The kind
        // that produced the current peak is held in dragonShakeKind
        // so the decay matches the source even after the engine has
        // cleared the underlying state.
        // PER VIEW, not shared (2026-09-07). One envelope meant a shake built
        // in third person carried its amplitude into first person and decayed
        // there — measured at [FPNOISE] amp=4.771 drag=4.771 on the first
        // rendered 1p frame with every centurion FP Intensity at 0, which is
        // an amplitude the 1p pass cannot produce and the 3p pass had left
        // behind. Index [0] = third person, [1] = first person; each view's
        // envelope answers only to its own half of the sliders, so setting a
        // source's 1p Intensity to 0 is an off switch in that view even
        // mid-shake.
        float        dragonShakeAmp[2]   = { 0.0f, 0.0f };
        float        dragonShakeSpeed[2] = { 0.0f, 0.0f };
        std::uint8_t dragonShakeKind[2]  = { 0, 0 };  // matches anonymous DragonShakeKind enum
    };
}
