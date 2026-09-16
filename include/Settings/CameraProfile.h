#pragma once

namespace DietDrCamera
{
    struct CameraProfile
    {
        // AUTHORED DEFAULTS (2026-08-22), lifted from the user's own tuned
        // sheathed profile in the "test" preset and made the shipped baseline
        // for the third-person and target-lock trees.
        //
        // What changed and why: these used to BE the engine's natural values
        // (sideOffset=30, height=-10, FOV=80, zoom=0), calibrated via the
        // Diagnostic → Log Camera Snapshot tool. The problem was zoom=0 — that
        // is the engine's PINNED CLOSEST distance (zoomBase=-0.2, 120 game
        // units from the head; see Defaults::VanillaCamDistance). Every entry
        // in the mod therefore shipped sitting at the minimum camera distance,
        // which is what "the defaults are way too zoomed in" was describing.
        // Slider zoom is 0-baselined and adds Defaults::GameUnitsPerSliderZoom
        // per unit, so 10 buys ~44 units of breathing room.
        //
        // The Vanilla*() factories below still record the ENGINE's values
        // unchanged — they are the reference, and they still seed the
        // categories that were deliberately left alone (mounts, werewolf,
        // vampire lord, dialogue). Vanity uses Default3p like Sheathed.
        float sideOffset  = 30.0f;
        float height      = -10.0f;
        float zoom        = 10.0f;
        float fov         = 80.0f;
        float rotation    = 0.0f;
        float pitchOffset = 0.0f;

        // Optional per-entry transition-speed override. When transitionOverride
        // is on, these per-channel multipliers replace the global Transitions
        // sliders for this profile (1.0 = near-instant; 0.5 = the default
        // smoothed feel). Edited via the "Transition Speed" popup / Quick Tune
        // Transitions box on each Categories / Target Lock entry.
        //
        // PER-SETTING OVERRIDES (2026-08-19). The block used to be all-or-
        // nothing: one "Enable Override" flag took every channel away from
        // the global Transitions sliders at once, so wanting a slower Zoom on
        // one entry meant re-authoring its Rotation, Position, FOV, Pitch,
        // Weight and Looseness to match the globals by hand. Each setting now
        // carries its own toggle, and an unticked setting keeps following the
        // global. `transitionOverride` survives as the MASTER — "does this
        // entry override anything at all" — kept in sync with the seven flags
        // by SyncTransitionOverride() at every edit site, so the runtime gates
        // and the amber button state read exactly as before.
        bool  transitionOverride  = false;
        bool  transitionSetRotation  = false;
        bool  transitionSetPitch     = false;
        bool  transitionSetPosition  = false;
        bool  transitionSetZoom      = false;
        bool  transitionSetFOV       = false;
        bool  transitionSetLooseness = false;
        bool  transitionSetWeight    = false;
        float transitionRotation  = 0.5f;
        float transitionPitch     = 0.5f;
        float transitionPosition  = 0.5f;
        float transitionZoom      = 0.5f;
        float transitionFOV       = 0.5f;
        float transitionLooseness = 0.0f;  // 0 = rigid (looseness default)
        // (Per-channel transition PERSONALITY — the shape of the chase — lived
        // here from 2026-08-14 to 2026-08-17. Removed with the feature; a
        // preset carrying the old transition_pers_* keys just ignores them.)
        // Per-entry "Weight" — how much this entry's move SIZE stretches its
        // duration (the global Transitions Weight, overridable here). A
        // werewolf should carry more mass than a sheathed walk, and that is a
        // property of the pose, not of the camera as a whole (user ruling
        // 2026-08-16). Group-scoped exactly like the global: one shared
        // multiplier across side/height/zoom/pitch, sized by whichever has
        // furthest to travel. Gated by transitionOverride with the rest of
        // this block. -1 is impossible via the UI; 0 = "this entry does not
        // scale duration by distance".
        float transitionWeight       = 0.0f;

        // AIM BIAS OVERRIDE — TARGET LOCK ONLY (2026-09-07, user request).
        //
        // Lock-on centring strength: 1.0 puts the locked target dead centre,
        // below 1 leaves it toward your Side Offset side, above 1 pushes past
        // centre. It was a global (settings.targetLockAimBias) plus a
        // per-enemy value living on EnemyFieldOverride, with no layer in
        // between — so biasing every bow state meant setting the same number
        // on all five creature columns AND every custom bind, which put it out
        // of reach in practice.
        //
        // It lives on CameraProfile now, inside the Transition Override block
        // and edited from that popup, which buys the layering for free: an
        // entry carries its own, an enemy override's profile carries the
        // enemy's, and SpliceEnemyTransitionOverride already overlays the
        // enemy's set channels onto the entry's. Enemy > entry > global, with
        // no new resolution path. The old EnemyFieldOverride::aimBias pair is
        // gone — one concept, one storage.
        //
        // Third Person entries carry the field (every copy path gets it free)
        // but never SHOW it: aim bias means nothing when you are not locked on.
        bool  transitionSetAimBias   = false;
        float transitionAimBias      = 1.0f;

        // Target Lock only. Signed proximity pitch; no global counterpart.
        // An enabled enemy value replaces the entry's, including explicit zero.
        bool  transitionSetPitchBias = false;
        float transitionPitchBias    = 0.0f;

        // SHOUT ENTRIES ONLY — "Lag" (seconds): after the shout ends, hold
        // the Shout camera this long before switching back to the
        // sheathed/unsheathed state (the shouts twin of Projectile Lag).
        // Lives on CameraProfile so every copy path — clipboard, env
        // variants, bindings, resets, the shared codec — carries it
        // automatically; non-shout entries simply never read or show it.
        // Consumed by StateResolver::PollShoutLinger via the value
        // CameraController publishes from the RESOLVED shout profile.
        float shoutLag = 0.0f;

        // Does this entry override ANY transition setting?
        [[nodiscard]] constexpr bool TransitionAnySet() const
        {
            return transitionSetRotation || transitionSetPitch ||
                   transitionSetPosition || transitionSetZoom ||
                   transitionSetFOV || transitionSetLooseness ||
                   transitionSetWeight || transitionSetAimBias || transitionSetPitchBias;
        }
        // Re-derive the master from the seven. Call after touching any of
        // them; every runtime gate still reads transitionOverride.
        constexpr void SyncTransitionOverride() { transitionOverride = TransitionAnySet(); }
        // Sets or clears the seven speed flags together. The "Override All"
        // checkbox that used to drive it was removed 2026-09-07; the popup's
        // Reset All and the loader are what call it now.
        // NOTE: Aim Bias is deliberately NOT part of this. "Override All" is
        // about how the camera MOVES; arming lock-on centring as a side effect
        // of a convenience button would change where the camera POINTS. Aim
        // Bias arms from its own row only.
        constexpr void SetTransitionAll(bool a_on)
        {
            transitionSetRotation  = a_on;
            transitionSetPitch     = a_on;
            transitionSetPosition  = a_on;
            transitionSetZoom      = a_on;
            transitionSetFOV       = a_on;
            transitionSetLooseness = a_on;
            transitionSetWeight    = a_on;
            transitionOverride     = a_on;
        }

        // The shipped baseline for every third-person and target-lock entry.
        // Spelled out rather than returning {} so that "what does DDC ship" is
        // answerable from one place even if the member initializers move again.
        static constexpr CameraProfile Default3p()
        {
            return { .sideOffset = 30.0f, .height = -10.0f, .zoom = 10.0f, .fov = 80.0f };
        }

        // Engine-natural values. These are a RECORD, not the shipped defaults —
        // they used to be both, until the authored defaults above split the two
        // apart. `Vanilla()` was `return {}` while those were the same thing;
        // it is now explicit so it keeps meaning "vanilla" regardless.
        static constexpr CameraProfile Vanilla()
        {
            return { .sideOffset = 30.0f, .height = -10.0f, .zoom = 0.0f, .fov = 80.0f };
        }

        static constexpr CameraProfile VanillaSheathed()
        {
            return { .sideOffset = 30.0f, .height = -10.0f, .zoom = 0.0f, .fov = 80.0f };
        }

        static constexpr CameraProfile VanillaCombat()
        {
            // Combat in vanilla: sideOffset=0, height=20, FOV=90 (engine boosts
            // from 80 to 90 when weapons are drawn). Snapshot 2026-04-25.
            return { .sideOffset = 0.0f, .height = 20.0f, .zoom = 0.0f, .fov = 90.0f };
        }

        // First-person dialogue baseline (user ruling 2026-08-31: "the
        // default fov for first person dialogue presets should be 80").
        // VanillaCombat's framing at the engine's undrawn base FOV — a 1p
        // dialogue look only drives FOV, and 90 is the drawn-weapons boost,
        // not how a conversation reads. Initializer AND WriteProfile
        // baseline for dialogueFirstPersonProfile, and the seed for every
        // newly created first-person dialogue look.
        static constexpr CameraProfile VanillaDialogue1p()
        {
            CameraProfile p = VanillaCombat();
            p.fov = 80.0f;
            return p;
        }

        // The werewolf family's shipped baseline (2026-08-30, user request:
        // "the werewolf entries need to have their default zoom set to 10").
        // VanillaCombat's framing with the same zoom-10 breathing room the
        // authored Default3p bought the humanoid tree — zoom 0 is the
        // engine's PINNED CLOSEST distance, wrong as a default for a body
        // that big. Initializer AND WriteProfile baseline for the werewolf
        // variants, including the named werewolf fields. Saving and loading
        // must use this same baseline.
        static constexpr CameraProfile WerewolfDefault()
        {
            return { .sideOffset = 0.0f, .height = 20.0f, .zoom = 10.0f, .fov = 90.0f };
        }

        static constexpr CameraProfile VanillaHorseback()
        {
            return { .sideOffset = 0.0f, .height = 0.0f, .zoom = 0.0f, .fov = 80.0f };
        }

        static constexpr CameraProfile VanillaDragonRiding()
        {
            return { .sideOffset = 0.0f, .height = 0.0f, .zoom = 0.0f, .fov = 80.0f };
        }

        // Authorship metadata: an override initialized from its parent owns
        // even default-valued settings. Full profile copies/reset carry this.
        bool parentSeeded = false;

        bool operator==(const CameraProfile&) const = default;
    };
}
