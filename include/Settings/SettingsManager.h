#pragma once

#include "Shouts/ShoutRegistry.h"
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>

#include "Camera/EventBeatDefs.h"
#include "Camera/NpcNoise.h"
#include "Core/HitShake.h"
#include "Core/DamageReaction.h"
#include "Settings/CameraProfile.h"
#include "Settings/PresetFormat.h"
#include "Settings/ProjectileTracing.h"
#include "Settings/CameraCollisionSettings.h"
#include "Settings/Defaults.h"
#include "Settings/DialogueLook.h"
#include "Settings/MeleeWeaponOverrides.h"
#include "Settings/ItemBindingIdentity.h"
#include "Core/CombatFraming.h"
#include "Core/CinematicViews.h"

#include <vector>

#include <toml++/toml.hpp>

namespace RE
{
    class TESObjectREFR;
    class Actor;
}

namespace DietDrCamera
{
    // Forward declarations — actual definitions live in Camera/StateResolver.h.
    // SettingsManager only needs the names to declare PickMagicProfile().
    enum class MagicSchool;
    enum class CastType;
    enum class DragonAction;

    class SettingsManager
    {
    public:
        [[nodiscard]] static SettingsManager& GetSingleton();

        void Load();
        void Save();
        // (SaveDefault() lived here until 2026-08-25. It generated a
        // hand-authored default TOML that had drifted out of step with the
        // code's own defaults — a second, contradictory answer to "what does
        // DDC ship", which sparse preset storage cannot tolerate. The shipped
        // defaults are now defined in exactly one place: the field
        // initializers and CameraProfile::Default3p.)
        void Validate();
        void ResetAllToVanilla();

        // Serialization primitives used by both the main config (Save /
        // Load) and the Presets system. BuildSaveTable produces a fully
        // populated toml::table from the current in-memory state — every
        // field that Save() writes is included. ApplyTable applies a
        // parsed toml::table back to in-memory state (mirrors Load's
        // body, no file IO). PresetManager calls these directly so any
        // new settings field auto-flows into presets. BuildSaveTable is
        // called ONLY by the preset writer now — the global config (Save())
        // persists just hotkeys + the active preset name, never settings.
        toml::table BuildSaveTable();
        bool        ApplyTable(const toml::table& tbl);

        // ── Preset storage format ──────────────────────────────────────────
        // Written by BuildSaveTable as [meta].format. This is a SCHEMA
        // number, not a mod version: increment for added fields as well as changed
        // meanings so older builds cannot discard newer authored settings.
        // Readers must preserve earlier public formats. No stamp is pre-1.0.
        //
        // Bumping this does not migrate anything by itself — it is the flag a
        // migration reads. Read PRESET-COMPAT.md before touching it.
        static constexpr int kPresetFormatVersion = kCurrentPresetFormat;
        // The format the preset/config currently in memory was loaded from
        // (0 = the file carried no stamp). Runtime-only; never written back.
        int presetFormatLoaded = 0;

        CameraCollisionSettings cameraCollision;
        // Dormant scaffolding for additional layer buckets (not currently exposed).
        bool  disableCollisionTrees  = false;  // kTrees
        bool  disableCollisionProps  = false;  // kClutter + kProps + kClutterLarge
        bool  disableCollisionActors = false;  // kBiped + kCharController + kBipedNoCC
        // Environment gate for adaptive. Mutually exclusive; neither = everywhere.

        // (Vanilla pitch zoom-out — the engine pushing the camera back as you
        // look toward the ground — is suppressed unconditionally in
        // HookManager::ApplyPitchZoomOut. It shipped as a toggle for one
        // build, 2026-08-22, and became permanent the same day once confirmed:
        // it is a correction, not a preference. The old `pitch_zoom_out`
        // preset key is ignored on load.)

        // Per-channel spring speed multipliers. Each multiplies a fixed
        // baseline omega for its channel; 1.0 = today's default feel.
        // mulPosition covers both side and height (one UI knob labeled
        // "Position Speed"). The previous overall "Base Speed" slider
        // was removed — its function is now covered by adjusting these
        // per-channel sliders together.
        float transitionMulRotation = 0.5f;
        float transitionMulPitch    = 0.5f;
        float transitionMulPosition = 0.5f;
        float transitionMulZoom     = 0.5f;
        float transitionMulFOV      = 0.5f;
        // (Per-channel transition PERSONALITY — Smooth / Heavy / Glide — lived
        // here from 2026-08-14 to 2026-08-17. The whole selector was cut; a
        // transition is the critically-damped chase Smooth always was, and the
        // speed sliders above are the only shaping left. Core/Spring.h keeps
        // the general damped form for any future experiment.)

        // "Weight" — move-size-aware duration. A spring settles in ~4/omega
        // regardless of how far it travels, so today a 4-unit height nudge and
        // a 90-unit zoom pull take exactly as long as each other; peak speed
        // scales with distance instead of duration doing so. A real operator's
        // hands are speed-bounded, so a bigger move takes longer.
        //
        // Scales a DISCRETE retarget's omega by (refSpan/span)^(0.5*weight),
        // latched at the retarget edge (see ChannelMotion::spanOmegaScale) and
        // clamped so nothing becomes instant or glacial. refSpan is
        // per-channel and chosen so a TYPICAL move lands on exactly today's
        // omega — existing tuning is preserved at normal magnitudes; only the
        // extremes move. 0 = off = today's behaviour exactly, so an absent
        // preset key needs no migration.
        float transitionWeight      = 0.0f;   // 0.0..1.0

        // NOTE: Weight is GROUP-SCOPED — one shared multiplier across side /
        // height / zoom / pitch, sized by whichever has furthest to travel.
        // A short-lived "Coordinate" toggle (2026-08-16) existed to re-sync
        // those channels after per-channel Weight pulled them apart; it was
        // removed the same day once the divergence was fixed at its source.
        // The old `transition_coordinate` key is simply ignored on load.

        // "Masking" (2026-08-16) was built, measured and CUT the same day —
        // it hit its ceiling on half of all moves and was still imperceptible,
        // and it made transitions non-reproducible. Do not re-propose. The
        // `transition_masking` key is ignored on load.

        // "Arc" (a transient pull-back bowing the middle of a move) was built
        // and CUT the same day — user did not like it. Do not re-propose.

        // Camera looseness — SmoothCam-style world-position follow lag. 0
        // disables (rigid camera, default). >0 makes the camera trail the
        // player's position by an EMA-smoothed amount, with a distance-
        // adaptive lerp rate. Higher values = more visible lag.
        float cameraLooseness       = 0.0f;   // 0.0..1.0

        // ("Height Pivot" — a tilt of atan(height/distance) folded into the
        // pitch offset so a lowered camera kept the player framed — shipped as
        // a dial 2026-08-20, was made unconditional the same day, and was
        // REVERTED 2026-08-21: it made the camera ANGLE depend on the Height
        // slider, so entries with different Heights aimed differently and the
        // look could not be tuned back. Height translates; pitch is Pitch
        // Offset's alone. The `height_pivot` key is ignored on load.)

        // "Look Limits" (Look Up / Look Down clamp on the pitch ARC) was built
        // and REVERTED the same day, 2026-08-20 — it was a misread of "pitch
        // feels super wide", which turned out to be about how far the camera
        // TRAVELS when pitching, not how far the angle goes. The
        // `look_up_limit_deg` / `look_down_limit_deg` keys are ignored on load.

        // "Dead Zone" (anchor holds still until the player leaves a radius)
        // and "Lead" (anchor aims ahead along travel) were both proposed
        // 2026-08-16. Lead was declined outright; Dead Zone was built and cut
        // the same day. Do not re-propose either. The `camera_deadzone` key is
        // ignored on load.

        // Cinematic effects (Extras → Cinematic Effects).
        CombatFraming::Tuning combatFraming{};
        // Deferred feature: preserve stored definitions without activating them.
        std::vector<CinematicViews::View> cinematicViews;
        // Dragons — five independent shake triggers fired by nearby
        // dragons, each with its own enable + amp + speed + range.
        // Camera Noise base is independent — these layer on top.
        //   Breath   — sustained fire/frost beam (Concentration spell, ~2s)
        //   Fireball — projectile cast (FireAndForget spell)
        //   Bite     — melee attack swing / hit / followthrough + anticipation
        //   Landing  — touchdown impulse on airborne→grounded transition
        //   Takeoff  — wing-flap impulse on launch
        // Intensity (amp) defaults to 0 = effect off. There is no enable toggle:
        // each source is on whenever its Intensity > 0 (see CameraNoiseController).
        bool  dragonShakeBreathEnabled    = false;   // legacy/unused (amp-gated now)
        float dragonShakeBreathAmp        = 0.0f;
        float dragonShakeBreathSpeed      = 1.0f;
        float dragonShakeBreathRange      = 3000.0f;
        bool  dragonShakeProjectileEnabled = false;
        float dragonShakeProjectileAmp    = 0.0f;
        float dragonShakeProjectileSpeed  = 1.0f;
        float dragonShakeProjectileRange  = 3000.0f;
        bool  dragonShakeBiteEnabled      = false;
        float dragonShakeBiteAmp          = 0.0f;
        float dragonShakeBiteSpeed        = 1.0f;
        float dragonShakeBiteRange        = 3000.0f;
        bool  dragonShakeTailEnabled      = false;
        float dragonShakeTailAmp          = 0.0f;
        float dragonShakeTailSpeed        = 1.0f;
        float dragonShakeTailRange        = 3000.0f;
        bool  dragonShakeWingEnabled      = false;
        float dragonShakeWingAmp          = 0.0f;
        float dragonShakeWingSpeed        = 1.0f;
        float dragonShakeWingRange        = 3000.0f;
        bool  dragonShakeLandingEnabled   = false;
        float dragonShakeLandingAmp       = 0.0f;
        float dragonShakeLandingSpeed     = 1.0f;
        float dragonShakeLandingRange     = 3000.0f;
        bool  dragonShakeTakeoffEnabled   = false;
        float dragonShakeTakeoffAmp       = 0.0f;
        float dragonShakeTakeoffSpeed     = 1.0f;
        float dragonShakeTakeoffRange     = 3000.0f;


        // Dwarven Centurion shake — three independent triggers fired by
        // any nearby Centurion (vanilla race + named variants like the
        // Forgemaster). Walk fires while the centurion is moving; Melee
        // fires on swing/hit/follow-through; Steam fires while a
        // Concentration cast is active (the steam-vent attack).

        // FIRST PERSON halves of the creature shakes (user request
        // 2026-09-06, the same split Sheathe / Unsheathe and the jump arc
        // took). One detection pass, one envelope, two sets of numbers; the
        // view being rendered picks which set the winning source is sized
        // with. Range splits too, so a rumble that is right at 3000 units
        // over the shoulder can be pulled in for a view sitting on the eye.
        // Initializers match the third-person ones above, so a preset that
        // says nothing about these behaves like an untouched install.
        float dragonShakeBreathAmpFp   = 0.0f;
        float dragonShakeBreathSpeedFp = 1.0f;
        float dragonShakeBreathRangeFp = 3000.0f;
        float dragonShakeProjectileAmpFp   = 0.0f;
        float dragonShakeProjectileSpeedFp = 1.0f;
        float dragonShakeProjectileRangeFp = 3000.0f;
        float dragonShakeBiteAmpFp   = 0.0f;
        float dragonShakeBiteSpeedFp = 1.0f;
        float dragonShakeBiteRangeFp = 3000.0f;
        float dragonShakeTailAmpFp   = 0.0f;
        float dragonShakeTailSpeedFp = 1.0f;
        float dragonShakeTailRangeFp = 3000.0f;
        float dragonShakeWingAmpFp   = 0.0f;
        float dragonShakeWingSpeedFp = 1.0f;
        float dragonShakeWingRangeFp = 3000.0f;
        float dragonShakeLandingAmpFp   = 0.0f;
        float dragonShakeLandingSpeedFp = 1.0f;
        float dragonShakeLandingRangeFp = 3000.0f;
        float dragonShakeTakeoffAmpFp   = 0.0f;
        float dragonShakeTakeoffSpeedFp = 1.0f;
        float dragonShakeTakeoffRangeFp = 3000.0f;

        bool  centurionShakeWalkEnabled  = false;
        float centurionShakeWalkAmp      = 0.0f;
        float centurionShakeWalkSpeed    = 1.0f;
        float centurionShakeWalkRange    = 3000.0f;
        bool  centurionShakeMeleeEnabled = false;
        float centurionShakeMeleeAmp     = 0.0f;
        float centurionShakeMeleeSpeed   = 1.0f;
        float centurionShakeMeleeRange   = 3000.0f;
        bool  centurionShakeSteamEnabled = false;
        float centurionShakeSteamAmp     = 0.0f;
        float centurionShakeSteamSpeed   = 1.0f;
        float centurionShakeSteamRange   = 3000.0f;

        // Werewolf transformation shake — fires on the rising edge of the
        // player becoming a werewolf. Envelope is slow rise → climax peak
        // → quick fade out. 0 = disabled, higher = more intensity.
        float werewolfTransformIntensity = 0.0f;
        // Oscillation rate of the transform shake. Used to be baked in
        // (speedMin/freqMax pairs hardcoded per source) back when the shake
        // rode the single-layer cinematic path and had no layer of its own to
        // clock. Now that it is an additive ambient layer it has a real clock,
        // so the rate is a slider like every other noise source's.
        float werewolfTransformSpeed = 1.6f;      // 0.1..5
        // Vampire Lord transformation shake — same envelope, separate
        // intensity slider, fires on the rising edge of the player
        // becoming a Vampire Lord.
        float vampireLordTransformIntensity = 0.0f;
        float vampireLordTransformSpeed = 0.7f;   // 0.1..5

        // Revert — the OUT edge (leaving the beast form), split from the
        // transform so it tunes independently. Presets from before the split
        // are seeded with their transform values at load, so the shipped
        // behaviour (both edges shared one tuning) carries over untouched.
        float werewolfRevertIntensity    = 0.0f;
        float werewolfRevertSpeed        = 1.6f;  // 0.1..5
        float vampireLordRevertIntensity = 0.0f;
        float vampireLordRevertSpeed     = 0.7f;  // 0.1..5

        // Vampire Lord "Bats" power — the swarm-dash lesser power the
        // Vampire Lord perk tree grants (Dawnguard DLC1VampireBats). Fires
        // on the frame the power's magic effect lands on the player, so it
        // is armed by an engine signal rather than a dash-detection guess
        // and needs no animation tags. Player-centric: no range.
        float vampireLordBatsIntensity = 0.0f;
        float vampireLordBatsSpeed     = 1.4f;    // 0.1..5

        // Reanimation shake — fires when a corpse near you is raised by ANY
        // reanimate spell. Matched on the magic effect's ARCHETYPE
        // (kReanimate), not on a form list, so every mod's necromancy spell
        // drives it with no per-mod support. Proximity-scaled: the beat is
        // measured from the raised body, and Range is how far away it can
        // still be felt.
        float centurionShakeWalkAmpFp   = 0.0f;
        float centurionShakeWalkSpeedFp = 1.0f;
        float centurionShakeWalkRangeFp = 3000.0f;
        float centurionShakeMeleeAmpFp   = 0.0f;
        float centurionShakeMeleeSpeedFp = 1.0f;
        float centurionShakeMeleeRangeFp = 3000.0f;
        float centurionShakeSteamAmpFp   = 0.0f;
        float centurionShakeSteamSpeedFp = 1.0f;
        float centurionShakeSteamRangeFp = 3000.0f;

        float reanimateShakeIntensity = 0.0f;
        float reanimateShakeSpeed     = 0.9f;     // 0.1..5
        float reanimateShakeRange     = 1200.0f;  // game units
        float reanimateShakeIntensityFp = 0.0f;
        float reanimateShakeSpeedFp     = 0.9f;
        float reanimateShakeRangeFp     = 1200.0f;

        // Summoning shake — fires when anything is summoned through a
        // conjuration portal (atronachs, familiars, dremora, bound weapons'
        // creature cousins...). Archetype-matched on kSummonCreature for the
        // same reason as reanimation, and measured from the summoner so the
        // beat lands with the portal rather than with wherever the creature
        // finishes rising. Fires for YOUR summons and other casters' alike.
        float summonShakeIntensity = 0.0f;
        float summonShakeSpeed     = 1.1f;        // 0.1..5
        float summonShakeRange     = 1500.0f;     // game units
        float summonShakeIntensityFp = 0.0f;
        float summonShakeSpeedFp     = 1.1f;
        float summonShakeRangeFp     = 1500.0f;

        // Slow Time Noise — slows the camera noise down alongside the world
        // while time is slowed. Camera noise runs on wall-clock time, so
        // without this the shake keeps its ordinary tempo through a Slow Time
        // shout while everything else crawls, which is the one thing that
        // gives the effect away.
        //
        // Driven by the engine's ACTUAL global time multiplier rather than by
        // detecting the shout, which is what makes the word-count scaling
        // automatic: a three-word Slow Time sets a lower multiplier than a
        // one-word, so the noise slows further with no word table here. It
        // also means any other source of slowed time gets the same treatment.
        //
        // 0 = off (noise ignores slowed time). 1 = noise slows exactly as much
        // as the world does. In between, part way.
        float slowTimeNoiseStrength = 0.0f;   // 0..1

        // Sheathing / Unsheathing shake — one beat fired on the weapon-state
        // edge into EITHER the draw or the put-away animation. 0 = disabled.
        // Applies in both third and first person.
        float weaponDrawNoiseIntensity = 0.0f;
        // How long the beat is HELD at full before Fade Duration takes it
        // down, in seconds. The original build had no hold at all — a 0.05s
        // ramp straight into a 0.12s floor decay, i.e. the whole beat was over
        // in about a sixth of a second, which is what read as "doesn't last
        // long enough". Total beat length is ~0.06 + this + Fade Duration.
        float weaponDrawNoiseDuration = 0.30f;   // 0..3 s
        // The beat's own Perlin rate. It used to inherit the live state's
        // Speed, so drawing in a state whose noise Speed is 0 (very common —
        // most states ship silent) advanced the sample clock not at all: the
        // amplitude swelled and fell over a frozen sample, producing a slow
        // lean rather than a shake. The layer now runs its own clock at this
        // rate, continuously, so a beat is always motion.
        float weaponDrawNoiseSpeed = 1.0f;       // 0.1..5

        // FIRST PERSON half of the same beat — its own Intensity, Duration,
        // Speed and character (user request 2026-09-06: "sheathe/unsheathe
        // should just have separate third person and first person sliders").
        // One source fired by one weapon-state edge; the runtime picks WHICH
        // set of numbers to shape it with from the view you are in. They had
        // to split because the two views do not render the same beat: 3p adds
        // it as its own layer with a position channel and the camera an arm's
        // length away, while 1p is rotation-only and sits on the eye, so one
        // Intensity cannot mean the same thing in both ("feels great for
        // third with the current settings but is way too strong for first").
        // Same units, same ranges, same initializers as the third-person half
        // above, so a preset that says nothing about the 1p keys behaves like
        // an untouched install: silent until asked for.
        float weaponDrawNoiseIntensityFp = 0.0f;
        float weaponDrawNoiseDurationFp  = 0.30f;   // 0..3 s
        float weaponDrawNoiseSpeedFp     = 1.0f;    // 0.1..5

        // Attack Lag (Cinematic Effects) — seconds to keep the attack camera
        // after a PROJECTILE ATTACK ends, before transitioning back to that
        // state's BASE profile. Only the attack→base transition is delayed
        // (attack→sprint/sneak/block/sheathe/new-attack all commit
        // immediately), and only in third person. 0 = off. Applied by
        // StateResolver::UpdateAttackLag.
        //
        // Scoped to attacks that actually launch something: the hold only
        // arms when a real release event fired during the attack. Melee is
        // deliberately absent — a swing has nothing in flight to ride out.
        float attackLagMagic   = 0.0f;   // spell + STAFF shots (Magic and Staves
                                         // states; concentration excluded)
        float attackLagArchery = 0.0f;   // Bow + Crossbow shots


        // Projectile Repulse (Cinematic Effects) — 3p sibling of the 1p FoF
        // release recoil (per-cell "Repulse" slider): a short cannon-recoil
        // pitch impulse on the 3p camera when a projectile leaves the player.
        // Archery = bow/crossbow arrowRelease; Magic = fire-and-forget /
        // ritual spell (and staff) release. Same 0–3 multiplier semantics as
        // the 1p slider against the shared 3.5° peak / 0.28s quartic shape.
        // 0 = off. Applied in CameraNoiseController's 3p path only.
        float projectileRepulseArchery = 0.0f;
        float projectileRepulseMagic   = 0.0f;

        // Jumping (Cinematic Effects) — 3p airborne camera-noise arc, single
        // Intensity dial. The shape is HARDCODED (launch burst → quiet apex →
        // wind-rush that builds with fall speed); the slider only scales it.
        // 0 = off. See the jump block in CameraNoiseController::OnCameraUpdate.
        float jumpNoiseAmp = 0.0f;
        // Falling (Cinematic Effects) — scales the airborne half of the same
        // arc (the quiet-apex bridge + the wind-rush that builds with fall
        // speed), split out from Jumping so each phase has its own dial.
        // 0 = off.
        float fallNoiseAmp = 0.0f;
        // Landing thud — the same Repulse / Thud-Snap pair the noise entries
        // carry, fired at the CONFIRMED touchdown of a real jump or fall and
        // scaled by how hard the landing actually was. Both views. 0 = off.
        float jumpRepulse     = 0.0f;   // 0..1
        float jumpRepulseFeel = 0.5f;   // 0 = dead-blow thud .. 1 = sharp snap

        // FIRST PERSON halves of the three jump-arc entries (user request
        // 2026-09-06, the same split the Sheathe / Unsheathe source took):
        // one arc, one set of triggers, two sets of numbers, and the view you
        // are in picks which set shapes it. Split for the reason that source
        // split — 1p renders this as rotation on the eye where 3p renders it
        // as a layer with a position channel at arm's length, so one dial
        // cannot mean the same thing in both. Same units, ranges and
        // initializers as the third-person fields above, so a preset that
        // says nothing about them behaves like an untouched install.
        float jumpNoiseAmpFp     = 0.0f;
        float fallNoiseAmpFp     = 0.0f;
        float jumpRepulseFp      = 0.0f;   // 0..1
        float jumpRepulseFeelFp  = 0.5f;

        // Paragliding master toggle (Cinematic Effects; mod support). The
        // profile / noise members live further down, past the NoiseProfile
        // definition — see the block beside globalNoise.
        bool paraglideEnabled = false;
        // Glide-landing thud fade (seconds-ish): how long the touchdown
        // shake takes to die out. 0.5 = the stock kick tempo exactly;
        // higher = a slower, longer roll-off. Only glide landings read it —
        // ordinary jump/fall thuds keep the stock envelope.
        float paraglideLandFade = 0.5f;

        // Entry clipboard hotkeys (Presets). Bound like every other DDC
        // hotkey — a DX scancode, or kInputGamepadTag | button bit. Active
        // only while a DDC menu page is on screen: copy the settings of the
        // entry the cursor is hovering, and paste them over another one.
        // 0 = unbound.
        std::uint32_t entryCopyKey  = 0;
        std::uint32_t entryPasteKey = 0;

        // Head Bobbing (Cinematic Effects) — a gait-locked camera cadence, in
        // BOTH views. Unlike Camera Noise this is not a Perlin texture: it is a
        // deterministic footfall cycle whose rate comes from how fast the player
        // is actually moving, so walking, running and sprinting each get their
        // own tempo for free and standing still gets nothing at all. Vertical
        // dips at twice the stride rate (one per footfall); the sideways sway
        // and the roll run at the stride rate, so left and right steps lean
        // opposite ways.
        //
        // Separate Intensity per view because the two are felt very
        // differently: in first person the camera IS the head, so a bob that
        // reads as a gentle amble in third person is nausea-inducing up close.
        // Both default to 0 (off) — an effect this pervasive must be opted into.
        // TWO CONTROLS, one per view, and nothing else. Cadence comes from
        // real footfalls so a Speed slider only ever made it disagree with the
        // feet, and the sway/roll balance is part of what a walk IS — both were
        // knobs whose right answer never changed, so they are gone.
        float headBobIntensity   = 0.0f;   // 0..5, third person / mounted
        float headBobIntensityFp = 0.0f;   // 0..5, first person

        // Stair Smoothing (Cinematic Effects, EXPERIMENTAL) — low-passes the
        // camera's VERTICAL follow so Bethesda's stepped character controller
        // stops reading as a roller coaster.
        //
        // The character controller resolves a step by teleporting the body up
        // the whole riser in one frame, so walking a staircase moves the player
        // in a hard sawtooth: flat, jump, flat, jump. The camera follows the
        // body exactly, so the whole view does it too. This keeps a smoothed
        // copy of the player's Z and offsets the camera by however far that
        // trails, which cancels the sawtooth while leaving horizontal motion
        // and mouse-look completely untouched.
        //
        // Strength is the time constant: 0 = off, 1 = heaviest smoothing.
        // Limit caps how far the camera may trail in world units, so a real
        // elevation change (a ramp, a hillside) saturates at a small constant
        // offset nobody can see instead of sinking the camera into the floor.
        float stairSmoothStrength = 0.0f;
        float stairSmoothLimit    = 24.0f;

        // Flee Framing (Cinematic Effects) — keeps the character in frame
        // when running TOWARD the camera (fleeing while facing back at
        // enemies): high Looseness lets the follow-lag offset push the camera
        // into the approaching player, shoving them off screen. This fades
        // the applied follow-lag toward rigid in exactly that geometry (a
        // third tighten factor beside the target-lock and dialogue ones),
        // scaled by approach speed. Fast turns retain protection through their
        // momentum; release discards hidden lag and restores ordinary following.
        // 0 = off, 1 = preserve normal distance at walking/running approach speeds.
        float fleeFramingStrength = 0.0f;

        // Incoming damage feedback, independently tuned for each view.
        DamageReaction::Tuning damageReaction, damageReactionFp;

        // Per-source cinematic-shake "character" — the shouts Camera Noise
        // sliders minus Breathing. rotShake = Rotation Shake (rotation weight),
        // posShake = Position Shake (translation weight), driftJitter 0..1
        // (0=slow drift, 1=fast jitter), roughness 0..1 (fractal persistence).
        // Defaults reproduce each source's prior hand-tuned per-kind feel so
        // existing setups don't change until edited.
        struct CinematicShakeChar {
            float rotShake     = 2.2f;
            float posShake     = 1.4f;
            float driftJitter  = 0.6f;
            float roughness    = 0.45f;
            float fadeDuration = 0.25f;  // shake fade-out tail / decay (seconds)
        };
        // Named per-kind defaults (reused by reset). Heavy = landing/takeoff/
        // centurion walk+melee; Sharp = projectile/bite/tail/wing; Sustained =
        // breath/centurion steam. fadeDuration reproduces the prior per-kind
        // decay tau (Sharp 0.16, Heavy 1.10, Sustained 0.25, transforms 1.5).
        // Blank slate: every effect starts with a zero shake character (and zero
        // Intensity), so it produces nothing until the user builds it up. These
        // are the source char defaults, the "Reset To Default" targets, and the
        // TOML diff base, so zeroing them here zeroes all three.
        static constexpr CinematicShakeChar kCharSustained{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr CinematicShakeChar kCharSharp    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr CinematicShakeChar kCharHeavy    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr CinematicShakeChar kCharWerewolf { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr CinematicShakeChar kCharVampLord { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        // Sheathe/unsheathe is the one source that does NOT start from a blank
        // slate. Its beat is additive on top of the live state's texture, and
        // its Rotation/Position Shake are max()'d against that state's — so
        // with an all-zero character AND a silent state (the common case:
        // most states ship with no noise at all) the layer had amplitude but
        // zero rotation and translation weight, and rendered literally
        // nothing no matter how high Intensity went. That is the other half of
        // "barely felt". A real character here means Intensity alone is enough
        // to feel the beat; Intensity still defaults to 0, so the effect is
        // still off until asked for.
        static constexpr CinematicShakeChar kCharDraw     { 2.4f, 1.2f, 0.55f, 0.50f, 0.35f };
        // Bats / Reanimation / Summoning ride the SAME additive layer as the
        // sheathe beat and max() their Rotation/Position Shake against the live
        // state's, so a blank-slate character would render nothing on the silent
        // states most setups ship with — the exact trap documented above. They
        // get real characters for the same reason the draw beat does; Intensity
        // still defaults to 0, so all three stay off until asked for.
        //   Bats      — a fast, jittery flurry: rotation-led, short tail.
        //   Reanimate — a low, rough shudder from the ground: slow drift, long tail.
        //   Summon    — a portal swell: middling drift, position-led, medium tail.
        static constexpr CinematicShakeChar kCharBats      { 2.6f, 1.0f, 0.80f, 0.60f, 0.30f };
        static constexpr CinematicShakeChar kCharReanimate { 1.6f, 1.8f, 0.25f, 0.55f, 0.90f };
        static constexpr CinematicShakeChar kCharSummon    { 1.8f, 2.0f, 0.45f, 0.50f, 0.70f };
        CinematicShakeChar dragonShakeBreathChar     = kCharSustained;
        CinematicShakeChar dragonShakeProjectileChar = kCharSharp;
        CinematicShakeChar dragonShakeBiteChar       = kCharSharp;
        CinematicShakeChar dragonShakeTailChar       = kCharSharp;
        CinematicShakeChar dragonShakeWingChar       = kCharSharp;
        CinematicShakeChar dragonShakeLandingChar    = kCharHeavy;
        CinematicShakeChar dragonShakeTakeoffChar    = kCharHeavy;
        CinematicShakeChar centurionShakeWalkChar    = kCharHeavy;
        CinematicShakeChar centurionShakeMeleeChar   = kCharHeavy;
        CinematicShakeChar centurionShakeSteamChar   = kCharSustained;

        CinematicShakeChar werewolfTransformChar     = kCharWerewolf;
        CinematicShakeChar vampireLordTransformChar  = kCharVampLord;
        CinematicShakeChar werewolfRevertChar        = kCharWerewolf;
        CinematicShakeChar vampireLordRevertChar     = kCharVampLord;
        CinematicShakeChar vampireLordBatsChar       = kCharBats;
        CinematicShakeChar reanimateShakeChar        = kCharReanimate;
        CinematicShakeChar summonShakeChar           = kCharSummon;

        // First-person twins of the characters above. Position Shake is
        // stored but never read in 1p (no positional channel), so the FP
        // halves of these editors do not show that row.
        CinematicShakeChar dragonShakeBreathCharFp     = kCharSustained;
        CinematicShakeChar dragonShakeProjectileCharFp = kCharSharp;
        CinematicShakeChar dragonShakeBiteCharFp       = kCharSharp;
        CinematicShakeChar dragonShakeTailCharFp       = kCharSharp;
        CinematicShakeChar dragonShakeWingCharFp       = kCharSharp;
        CinematicShakeChar dragonShakeLandingCharFp    = kCharHeavy;
        CinematicShakeChar dragonShakeTakeoffCharFp    = kCharHeavy;
        CinematicShakeChar centurionShakeWalkCharFp    = kCharHeavy;
        CinematicShakeChar centurionShakeMeleeCharFp   = kCharHeavy;
        CinematicShakeChar centurionShakeSteamCharFp   = kCharSustained;
        CinematicShakeChar reanimateShakeCharFp        = kCharReanimate;
        CinematicShakeChar summonShakeCharFp           = kCharSummon;
        CinematicShakeChar weaponDrawNoiseChar       = kCharDraw;
        // First-person twin of the row above. Position Shake is stored but
        // never read: the 1p path has no positional channel, so the FP half's
        // editor does not show that slider.
        CinematicShakeChar weaponDrawNoiseCharFp     = kCharDraw;

        // --- Table-driven event beats (EventBeatDefs.h) ---------------------
        // One tuning row per BeatId: Intensity (0 = off, the ship default for
        // every source, matching Bats), Speed, Range (positional beats only)
        // and the additive-layer character. The static table carries the
        // hold / positional flag / defaults; only the tunables live here so
        // the whole family loads, saves, and resets through three loops.
        struct BeatTuning
        {
            float              intensity = 0.0f;
            float              speed     = 1.0f;
            float              range     = 0.0f;
            // How much the shake points at the thing that caused it. 0 = the
            // old isotropic wobble; 1 = fully radial. Only does anything when
            // the source recorded a world position. Redistributes direction
            // only — it never changes how loud the beat is.
            float              direction = 0.65f;
            CinematicShakeChar chr{};
        };
        [[nodiscard]] static BeatTuning DefaultBeatTuning(std::size_t a_idx)
        {
            const auto& d = kEventBeatDefs[a_idx];
            return BeatTuning{ 0.0f, d.defSpeed, d.defRange, 0.65f,
                               CinematicShakeChar{ d.defChar.rotShake, d.defChar.posShake,
                                                   d.defChar.driftJitter, d.defChar.roughness,
                                                   d.defChar.fadeDuration } };
        }
        [[nodiscard]] static std::array<BeatTuning, kEventBeatCount> MakeDefaultBeatTunings()
        {
            std::array<BeatTuning, kEventBeatCount> out{};
            for (std::size_t i = 0; i < kEventBeatCount; ++i) out[i] = DefaultBeatTuning(i);
            return out;
        }
        std::array<BeatTuning, kEventBeatCount> eventBeats = MakeDefaultBeatTunings();
        [[nodiscard]] const BeatTuning& EventBeatTuning(BeatId a_id) const
        {
            return eventBeats[static_cast<std::size_t>(a_id)];
        }
        [[nodiscard]] bool AnyEventBeatActive() const
        {
            for (const auto& b : eventBeats)
                if (b.intensity > 0.0001f) return true;
            return false;
        }

        // Proximity ambience (fires, divine presences, the Soul Cairn portal,
        // the Eye of Magnus, the College magelights) was removed on request.

        // True when ANY noise entry anywhere (plain / indoor / location
        // copies) carries a non-zero per-entry Repulse — the repulse block's
        // per-frame master gate, replacing the old two global sliders.
        [[nodiscard]] bool AnyRepulseConfigured() const
        {
            auto scan = [](const std::unordered_map<std::string, NoiseProfile>& m) {
                for (const auto& [k, p] : m)
                    if (p.repulse > 0.001f) return true;
                return false;
            };
            // Landing thud shares the block. THIRD PERSON only: this gate
            // guards the 3p repulse machinery, and the 1p thud is armed and
            // rendered inside the 1p branch, which returns long before it.
            if (jumpRepulse > 0.001f) return true;
            if (scan(stateNoise) || scan(stateNoiseIndoor)) return true;
            for (const auto& lo : locationOverrides)
                if (scan(lo.stateNoise)) return true;
            return false;
        }

        // --- NPC Noise (Cinematic Effects) ----------------------------------
        // Magic amount scales ordinary NPC magic noise sources: their
        // concentration casts (inheriting the player's tuned magic entries),
        // their shouts (inheriting the shout entries),
        // and — as a pure on/off gate — the NPC-cast Reanimation/Summoning
        // beats. DEFAULT 1.0: reanimate/summon already fire for NPC casters,
        // and a 0 default would silently mute that shipped behaviour. 0 is
        // this source's off-switch. Beast forms use Transformations below.
        // Ranges are internal constants, not sliders.
        // Default 0 = off (user ruling 2026-08-19). See the save site: the key
        // is emitted UNCONDITIONALLY so this default is never inherited by a
        // preset that was written with the old 1.0 baseline in mind.
        float npcNoiseIntensity = 0.0f;   // 0..3
        // First-person half (2026-09-07). Every other cinematic source grew one
        // on 2026-09-06; this layer and the two NPC beats it scales were the
        // three that did not, so their first-person loudness could not be set
        // independently at all. The scan itself is view-agnostic — it produces
        // an UNSCALED amplitude and each view multiplies by its own half at
        // read time, which is the shape that keeps a 0 here from touching the
        // other view's state.
        float npcNoiseIntensityFp = 0.0f;   // 0..3
        float npcShoutNoiseIntensity = 0.0f;    // 0..3
        float npcShoutNoiseIntensityFp = 0.0f;  // 0..3

        // Nearby melee swings inherit the matching melee attack noise entry.
        // Separate from NPC magic, with independent POV amounts; opt-in.
        float npcMeleeNoiseIntensity = 0.0f;    // 0..3
        float npcMeleeNoiseIntensityFp = 0.0f;  // 0..3
        // Passing arrows/bolts use their own cinematic texture and distance falloff.
        float npcArcheryNoiseIntensity = 0.0f;    // 0..3
        float npcArcheryNoiseIntensityFp = 0.0f;  // 0..3
        // Both beast forms, including transformation/revert and combat noise.
        float npcTransformNoiseIntensity = 0.0f;    // 0..3
        float npcTransformNoiseIntensityFp = 0.0f;  // 0..3

        // Combat enter / exit pulse. On the rising edge of player combat
        // state, briefly widens FOV + pulls zoom back beyond the new
        // combat profile target, then settles into the combat profile.
        // On falling edge, opposite (smaller magnitude). Bell-curve
        // envelope of `combatPulseDuration` seconds. 0 intensity = off.
        float combatPulseIntensity = 0.0f;     // 0..5; 0 = disabled
        float combatPulseDuration  = 0.8f;     // seconds, full envelope length

        bool  disableVanityCamera = false;    // prevents the idle-triggered spinning vanity camera

        // Show Player In Menus — when a relevant UI menu opens, force
        // the camera to third-person and frame the player from the
        // front so they're visible inside the menu rather than off-
        // screen behind the UI panel. Each supported menu gets its
        // own enable toggle AND its own framing values, so e.g.
        // Inventory can sit close in front while Barter pulls back to
        // include the merchant.
        struct ShowPlayerInMenuEntry
        {
            bool  enabled = false;
            // Only apply the CUSTOMIZATION (the framing below) while the
            // player is riding a dragon. Unpause / allow-movement / camera
            // control are unaffected — this scopes the camera work only, which
            // is what sitting beside Enable Customization means. Off = the
            // framing applies everywhere, exactly as before.
            bool  dragonOnly = false;
            // CAMERA-RELATIVE, matching CameraProfile's Side Offset / Height /
            // Zoom (2026-08-23). Positive X moves the CAMERA right, positive Z
            // moves the CAMERA up, positive Y moves the CAMERA further away —
            // so a bigger Height raises the camera here exactly as it does on
            // every other page.
            //
            // These were SUBJECT-relative until 2026-08-23 (positive Height
            // moved the PLAYER up on screen, i.e. the camera DOWN), which read
            // as the slider working backwards. The apply sites negate on the
            // way in; presets written before the flip are migrated on load —
            // see menuOffsetsCameraRelative.
            float offsetX = 0.0f;
            float offsetY = 150.0f;
            float offsetZ = 0.0f;
            // Rotation around the player. 0 = directly behind the
            // player's facing direction (their back — the default, user
            // ruling 2026-08-15), π = directly in front.
            float yaw = 0.0f;
            // Fixed FOV during the menu — pinned so weapon-state
            // profile FOVs don't change it.
            float fov = 90.0f;
            // Skyrim-Souls-style: when on, the engine doesn't pause the
            // game while this menu is open. World, NPCs, time-of-day all
            // keep simulating. Implemented by clearing the menu's
            // kPausesGame flag at construction (HookManager).
            bool  unpauseGame = false;
            // Whether the player can walk while this unpaused menu is
            // open. Default FALSE (user ruling 2026-08-19: "the menus tab
            // shouldn't automatically enable the allow movement toggle") —
            // ticking Unpause Game used to arrive with movement already
            // allowed, which is a second decision the user never made.
            // Barter was already false for its own reason (the dialogue
            // camera wants the player rooted); that override in
            // SettingsManager::Load is now redundant but harmless and is
            // kept as documentation of the intent. Consulted by
            // UnpauseManager::ShouldBlockPlayerMovement.
            //
            // SAFE TO CHANGE: allow_movement is emitted UNCONDITIONALLY by
            // saveEntry, so every preset ever written carries its own
            // explicit value and none of them inherit this default.
            bool  allowMovement = false;
            // Whether mouse / right-stick camera look passes through
            // while this unpaused menu is open. Default off; useful for
            // Tween / Inventory / Magic / Container where the user
            // might want to orbit the menu camera. Not exposed for
            // Barter (dialogue camera owns it) or Wait (can't unpause).
            bool  allowCameraControl = false;
            // Per-menu "force 3p from 1p" — when on AND the player was
            // in first person at the moment this menu opened, the
            // controller switches the camera to 3p with the configured
            // framing. When off, 1p stays 1p (vanilla view; unpause
            // still active if set). Replaces the previous single global
            // toggle so each menu can decide independently.
            bool  force3pFromFirstPerson = false;
        };
        ShowPlayerInMenuEntry showPlayerInInventory;
        ShowPlayerInMenuEntry showPlayerInContainer;
        ShowPlayerInMenuEntry showPlayerInBarter;
        ShowPlayerInMenuEntry showPlayerInMagic;
        ShowPlayerInMenuEntry showPlayerInTween;
        ShowPlayerInMenuEntry showPlayerInWait;
        ShowPlayerInMenuEntry showPlayerInFavorites;


        // Archery Projectile Tracing — covers both bows and crossbows.
        // When enabled, draws the predicted impact point as a moving
        // reticle and a fading trajectory arc while the string is taut.
        // Suppressed automatically while TDM target-lock is active.
        bool  archeryTracingEnabled  = false;

        // Spell Projectile Tracing — same reticle + trajectory system,
        // applied to FoF missile spells. Auto-gates by projectile type
        // (MissileProjectile only — Flames/Sparks/beams excluded) AND
        // by the spell having any Hostile-flagged effect (Soul Trap
        // and utility spells excluded). Works for staves automatically
        // since they fire the spell's projectile through the same path.
        bool  spellTracingEnabled    = false;

        // Projectile reticle appearance — affects the ImGui-drawn "+"
        // markers used for bow live prediction, spell live prediction,
        // and in-flight spell shots. RGB in [0,1]; size multiplies the
        // default 9-px arm; thickness in device pixels.
        float projectileReticleColorR     = 1.0f;
        float projectileReticleColorG     = 1.0f;
        float projectileReticleColorB     = 1.0f;
        float projectileReticleSizeScale  = 1.0f;   // 0.5..5.0
        float projectileReticleThickness  = 2.0f;   // 1.0..10.0

        // Legacy preset field retained for round-tripping older files.
        // Runtime smoothing is fixed at 0.30s in CrosshairManager.
        float archeryTracingSmoothTau = 0.015f;

        // Sneak-eye relocation offset, applied while the player is
        // sneaking with a bow/crossbow equipped. Stage units, additive
        // to the SWF's captured baseline. Negative X = leftward,
        // negative Y = upward.
        float sneakMeterOffsetX = -500.0f;
        float sneakMeterOffsetY = -100.0f;

        // Existing keys remain the third-person settings. Format 11 gives
        // first person its own values; older presets seed both views alike.
        ProjectileTracingSettings projectileTracingFp;
        ProjectileTracingSettings GetProjectileTracing(bool firstPerson) const
        {
            return firstPerson ? projectileTracingFp : ProjectileTracingSettings{
                archeryTracingEnabled, spellTracingEnabled, projectileReticleSizeScale,
                projectileReticleThickness, sneakMeterOffsetX, sneakMeterOffsetY};
        }

        // Diagnostic flag (NOT persisted): when true, every camera-override
        // path in the plugin short-circuits so the camera renders pure
        // vanilla. Used to capture the engine's natural targetZoomOffset /
        // posOffset / FOV values via the "Log Camera Snapshot" button so
        // the "Reset All to Vanilla" baseline can be calibrated to actual
        // vanilla. Resets to false on every plugin load.
        bool  diagnosticSuspendOverrides = false;

        // (Per-state 1p profiles + FirstPersonProfile struct are
        // declared further down, after NoiseProfile is in scope.)


        // Independent third-person idle entry, with the same defaults as Sheathed.
        CameraProfile vanityCamera = CameraProfile::Default3p();
        float vanityIdleSeconds = 120.0f;

        // Death camera — only FOV and hold duration are user-facing. The
        // other profile channels (position/zoom/rotation/pitch) fight with
        // bleedout's randHeading + animated-bone logic and were dropped.
        // deathCameraHoldDuration writes fPlayerDeathReloadTime so the
        // bleedout-to-reload-prompt delay matches the slider.
        // (Kill Camera — vanilla-killcam disablers, Custom Angles and the
        // guaranteed-execution testing toggle — REMOVED 2026-08-17. Old
        // presets' killcam_* keys are ignored on load.)

        float deathCameraFov          = 90.0f;   // 40..140
        float deathCameraHoldDuration = 5.0f;    // seconds, 1..30, vanilla ~5
        // When true, deathCameraHoldDuration is ignored — the bleedout
        // camera lasts indefinitely (fPlayerDeathReloadTime pushed to
        // a day) until the skip hotkey fires the reload manually.
        bool  deathCameraInfiniteDuration = false;
        bool  deathCameraFreeLook     = false;   // mouse rotates bleedout camera
        // Free-look input is permanently inverted on both axes (hard-coded in
        // HookManager); no per-axis toggle.
        // Skip-the-rest hotkey. When in BleedoutCameraState, pressing this
        // triggers BGSSaveLoadManager::LoadMostRecentSaveGame immediately
        // instead of waiting for the Hold Duration timer to expire. Same
        // kInputGamepadTag encoding as the other hotkey fields.
        std::uint32_t deathCameraSkipKey = 0;
        // Death-cam slow motion. On death the global time multiplier eases down
        // toward (1 - strength/100), holds, then eases back to 1.0, spanning
        // Duration seconds (real time). Strength is a PERCENT (0..90); 0 = off.
        // Duration default is 0 (user ruling 2026-08-19) and 0 ALSO means off —
        // the arm sites in HookedBleedoutBegin require both > 0.
        float deathCameraSlowmoStrength = 0.0f;   // 0..90 (%), 0 = off
        float deathCameraSlowmoDuration = 0.0f;   // seconds, 0..15, 0 = off
        // Fade-out hotkey (mirrors ragdollCamFadeKey, 2026-08-15): pressing it
        // during the death camera eases the slow motion back to normal early.
        // Distinct from the SKIP key, which reloads the save.
        std::uint32_t deathCamFadeKey = 0;

        // --- Ragdoll cinematic ---
        // The engine routes a recoverable knockdown (Unrelenting Force, paralysis,
        // a giant's club, …) through BleedoutCameraState too. When enabled, DDC
        // applies the same free-look + slow motion as the death cam during a
        // ragdoll — but NEVER the load-save prompt. Mirrors the Death Camera.
        bool  ragdollCamFreeLook       = false;   // mouse/stick rotates the ragdoll camera
        float ragdollCamFov            = 90.0f;   // 40..140
        float ragdollCamSlowmoStrength = 0.0f;    // 0..90 (%), 0 = off
        float ragdollCamSlowmoDuration = 0.0f;    // seconds, 0..15, 0 = off
        // Fade-out hotkey: pressing it during a ragdoll eases the slow motion back
        // to normal early. MAY share the same key as deathCameraSkipKey — the input
        // sink dispatches by context (real death -> reload, ragdoll -> fade).
        std::uint32_t ragdollCamFadeKey = 0;
        // Stage 2 (declared now, behavior wired later): hold DDC's third-person
        // camera resolved to the PARENT state (e.g. Melee Power Attack -> Melee
        // Unsheathed) instead of the engine bleedout camera during a ragdoll.
        bool  ragdollCamHoldParentState = false;

        // Dialogue camera — free-form 6-slider profile applied while the
        // Dialogue Menu is open. Third-person path routes sideOffset/height/
        // zoom through posOffsetExpected (0x5C/0x60/0x64); rotation goes
        // through freeRotation.x, pitch through ApplyPitchOffset, FOV
        // through worldFOV. First-person path piggybacks on the FirstPersonState
        // hook — fov drives the 1p world FOV while the menu is open.
        // (Vanilla Skyrim does NOT force 3p during dialogue, so both paths
        // are live depending on which POV the player opened dialogue in.)
        // Always on — the dialogue camera (both POVs) no longer has an
        // enable toggle; these are forced true (load is ignored).
        bool          dialogueEnabled            = true;
        // Legacy single-profile field. Still loaded from old configs so we
        // can migrate it into the new dialogueBuckets system. New code
        // should read from dialogueBuckets / activeDialogueLook instead.
        CameraProfile dialogueProfile            = CameraProfile::VanillaCombat();
        bool          dialogueFirstPersonEnabled = true;
        // Legacy single-profile field, see dialogueProfile note above.
        CameraProfile dialogueFirstPersonProfile = CameraProfile::VanillaDialogue1p();

        // Dialogue-only per-channel transition speeds. Replace the global
        // Transitions sliders for every spring during dialogue (3p main
        // spring, 1p FOV, face-lock). 1.0 = same speed as base; tune up
        // for snappier dialogue, down for slower / more cinematic. The
        // Dialogue menu exposes these as a dedicated set so dialogue
        // feel is independent of the global Transitions menu.
        // Default ~0.2 (20% of the 0.05..1.0 range) — a slow, cinematic
        // dialogue blend. The old 1.0 default sat at the fast end (~0.25s),
        // which read as a snap into the dialogue framing.
        float dialogueMulRotation = 0.2f;
        float dialogueMulPitch    = 0.2f;
        float dialogueMulPosition = 0.2f;
        float dialogueMulZoom     = 0.2f;
        float dialogueMulFOV      = 0.2f;
        // (Dialogue transition personalities removed 2026-08-17 with the rest.)

        // Map a dialogue speed slider (0.05..1.0) to a blend duration in seconds.
        // The old mapping was 0.22/mul -- hyperbolic, so 0.5 already felt very fast
        // (0.44s) and 0.1 was a ~2.2s crawl, cramming the usable range into the top.
        // This is a perceptually-even linear ramp: fast end ~0.25s, slow end ~0.80s.
        // SHARED by every dialogue blend site (position lockstep in CameraController,
        // face-lock pitch/yaw in HookManager) so the channels can never desync --
        // a desync would finish one channel early and leave another crawling.
        static float DialogueBlendDuration(float mul)
        {
            float n = (mul - 0.05f) / 0.95f;
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;
            // n=0 -> 2.50s (slow), n=1 -> 0.13s (fast). The fast end is short
            // enough that the smootherstep ease-out tail is imperceptible, so a
            // max-speed dialogue transition reads as crisp rather than "fast then
            // crawl" (the curve itself is unchanged -- a fast-end curve swap to
            // cubic made no perceptible difference; the tail was a duration issue).
            // Shared by ALL dialogue blends (Aim, Position, FOV) so the aim pitch
            // settles together with the position blend -- the camera height composes
            // that pitch, so a separate (slower) Aim mapping left the height drifting
            // after the position settled.
            return 2.50f + n * (0.13f - 2.50f);
        }


        // Legacy global random flag. Migrated on load into the per-category
        // arrays below when schema version < 3. New code reads the arrays;
        // this stays around only so old configs upgrade cleanly.
        bool dialogueRandomEnabled = false;

        // Per-category random toggles. Indexed by DialogueCategory
        // (Outdoors/Indoors/Dragons/Creatures/SpecificNPC). When the
        // entry flag is set, the dialogue's bucket-pick on OPEN rolls a
        // random preset within the matching candidate set. When the
        // option flag is set, each dialogue option click re-rolls the
        // active preset. Replaces the single global dialogueRandomEnabled
        // so users can opt in per category (e.g., dragons random, NPCs not).
        std::array<bool, static_cast<std::size_t>(DialogueCategory::Count)> dialogueRandomOnEntry{};
        std::array<bool, static_cast<std::size_t>(DialogueCategory::Count)> dialogueRandomOnOption{};

        // Re-roll the active dialogue preset on every new NPC line. Sibling
        // of dialogueRandomOnOption (which fires when YOU pick a topic);
        // this one fires on each response the NPC delivers, so a long
        // multi-line answer cuts between framings instead of holding one.
        bool dialogueSwitchOnNpcLine = false;
        // Companion toggle (2026-08-15): when on, an NPC line ESTIMATED
        // under 5 seconds (subtitle length heuristic, see the npc-line edge
        // in HookManager) keeps the current preset instead of switching.
        bool dialogueSkipShortNpcLines = false;

        // Automatic preset rotation while a conversation is open. Each time
        // the timer fires, a new interval is rolled uniformly in
        // [min, max] seconds. Max at 0 disables the whole thing; min is
        // clamped to max so an inverted pair can't stall it.
        float dialogueAutoSwitchMin = 0.0f;   // seconds, 0..60
        float dialogueAutoSwitchMax = 0.0f;   // seconds, 0..60; 0 = off

        // ONE master random toggle: every conversation open rolls a random
        // preset, whatever the per-category flags say. (Mid-conversation
        // switches — option picks, NPC lines, the timer — are already
        // random by construction; the open pick was the sequential hole.)
        bool dialogueRandomEveryTime = false;

        // Minimum time a preset must HOLD before any automatic switch may
        // replace it (seconds; 0 = off). Suppresses — not defers — switches
        // that land inside the window, which is exactly the option-pick →
        // NPC-line double-switch the 17:30 log showed 50-70ms apart. The
        // manual cycle keys are exempt and restart the window.
        float dialogueMinShotSec = 0.0f;      // 0..10

        // New preset system: 5 categories × 2 POVs = 10 buckets, each with
        // a list of named DialogueLook entries. Selected at runtime by the
        // SpeakerClassifier (Phase 2). Schema version bump in [general]
        // marks migrated configs so we don't re-migrate the legacy fields.
        std::array<DialogueBucket, kDialogueBucketCount> dialogueBuckets{};
        int           dialogueSchemaVersion          = 3;
        std::uint32_t dialogueCycleNextKey           = 0;     // DXScanCode, 0 = unbound
        std::uint32_t dialogueCyclePrevKey           = 0;

        // Categories shoulder-swap hotkey. Same encoding scheme as the
        // dialogue hotkeys (DXScanCode for keyboard, kInputGamepadTag |
        // XINPUT button mask for gamepad). Bound via the inline Shoulder
        // Swap button on each Categories page; pressing the bound key
        // in-game fires SwapCategoriesShoulders().
        std::uint32_t categoriesShoulderSwapKey      = 0;

        // Preset cycle hotkey. Pressing the bound key in-game advances to
        // the next saved preset (wraps around) and shows a notification of
        // the new preset's name. Bound via the Presets section.
        std::uint32_t presetCycleNextKey             = 0;

        // Quick Tune hotkey. Pressing the bound key in-game opens a small
        // top-left overlay panel containing the sliders for whatever
        // camera profile is currently driving the camera. Pauses the
        // game and captures input (same as the regular DDC menu), but
        // skips the full Categories tree — straight to "edit what's on
        // screen right now." Press the key again to close.
        std::uint32_t quickTuneHotkey                = 0;

        // Live state — not serialized. Set by DialogueLookPicker on
        // dialogue open; CameraController and the face-lock read from
        // here. Pointer-into-vector hazard mitigated by storing
        // (bucket, look) indices instead and resolving on read.
        int           activeDialogueBucketIdx        = -1;    // -1 = none
        int           activeDialogueLookIdx          = -1;

        // Currently-loaded global preset (from PresetManager). Tracked so
        // the Update button can write back to the same file the user loaded.
        // Empty = no preset loaded; live state lives only in settings.toml.
        std::string   activePresetName;
        DialogueLook*       GetActiveDialogueLook();
        const DialogueLook* GetActiveDialogueLook() const;
        // Fill in any DialogueLook::uid still at 0. Cheap (20 buckets), safe to
        // call every frame; run at load and from the Dialogue page so a preset
        // written before uids existed picks them up the first time it is seen.
        void AssignDialogueLookUids();
        // Seed the environmental catch-all dialogue buckets (Outdoors 3p/1p,
        // Indoors 3p/1p, Horseback 3p) with a "Default" look when empty, so
        // every path into a runnable state — file load, preset load, fresh
        // install, Reset All — starts those boxes with a preset. Never
        // touches a bucket that already has looks. Dragons / Creatures /
        // Specific NPCs stay empty on purpose: empty means "inherit down
        // the resolver chain", and a seed there would block it.
        void EnsureDialogueDefaultLooks();
        // This look's framing at the place the player is standing in, or
        // nullptr. Narrowest place wins, same as every other per-entry lookup.
        [[nodiscard]] CameraProfile* ActiveLocationDialogueLook(std::uint32_t a_uid);
        [[nodiscard]] const CameraProfile* ActiveLocationDialogueLook(std::uint32_t a_uid) const;
        // Same resolved framing for camera application and Quick Tune editing.
        [[nodiscard]] CameraProfile* ResolveDialogueProfile(bool a_firstPerson);
        [[nodiscard]] const CameraProfile* ResolveDialogueProfile(bool a_firstPerson) const;
        // Controls-layer unlock: relabel movement IDEvents inside
        // MenuControls::ProcessEvent so WASD/left-stick keep reaching
        // PlayerControls' MovementHandler while the Dialogue Menu is open.
        // Mirrors DME's event-relabel approach (no actorState writes).
        bool          dialogueMovementEnabled    = false;

        // Workstation / furniture FOV — applies while the engine is in
        // kFurniture or kAnimated (crafting benches use kAnimated for the
        // scripted zoom-in; chairs/beds use kFurniture). Classified per
        // occupied-furniture form via TESFurniture keywords + WorkBenchData.
        // Only FOV is exposed; the engine rotation storage for these states
        // isn't hookable without RE work.

        // Camera noise (v3, state-keyed): Perlin-overlay on cameraRoot->local
        // with one customizable profile per Categories state. The Global
        // profile applies to every state whose own customization isn't
        // enabled. This replaces the v2 movement-based context system
        // (Idle/Walking/Running/etc.) — those buckets were confusing because
        // they didn't line up with the weapon-state buckets used everywhere
        // else in the mod.
        struct NoiseProfile
        {
            bool  enabled = false;  // per-state Customize toggle (ignored on globalNoise — always "on")
            float amp     = 0.0f;   // Intensity (master amount)
            float speed   = 0.0f;   // Speed (oscillation rate)
            float sway    = 0.0f;   // Position Shake (translation weight)
            float tilt    = 0.0f;   // Rotation Shake (rotation weight)
            float wobble  = 0.0f;   // Speed Variation (frequency multiplier; advanced)
            // --- Two-band model (Jun-3 rework, see reference-camera-noise-research) ---
            float driftJitter = 0.35f;  // 0 = slow cinematic drift, 1 = fast handheld jitter
            float roughness   = 0.45f;  // fractal persistence — texture "naturalness"
            // Post-shout-end fade tail (seconds). Only meaningful on the
            // shouts.<state>.base entries the 3p noise resolver reads. 0 =
            // quick default spring-decay; higher rides the shout shake out
            // over this duration so long shouts (e.g. Whirlwind Sprint) keep
            // shaking through the action. Mirrors FirstPersonProfile::
            // shoutFadeDuration's intent on the 3p path.
            float shoutFadeDuration = 0.0f;  // 0..4
            // Per-entry Projectile Repulse strength (0..3, 0 = off) — the 3p
            // release recoil, keyed by WHAT fired: rendered on the magic /
            // staves school cells, the shout entries, the werewolf roar and
            // the archery drawing entries, but stored on every profile so
            // weapon bindings, hand overrides, location copies and melee
            // overrides inherit it through the shared (de)serialization
            // helpers. Mirrors FirstPersonProfile::repulse. NOT part of the
            // ambient crossfade signature — it never changes the texture.
            float repulse = 0.0f;  // 0..3
            // The kick's FEEL, one axis, per entry: 0 = heavy dead-blow
            // (slower, damped, push-forward), 0.5 = the shipped shape
            // exactly, 1 = sharp snap (faster, springy overshoot,
            // rotation-forward). One intuitive control instead of a shape
            // panel.
            float repulseFeel = 0.5f;  // 0..1
            // Attack noise DURATION (seconds). Only meaningful on the attack /
            // power-attack cells. 0 = the shipped behaviour: the attack noise
            // holds for as long as the Attack sub-state does, which is the
            // animation bracket, and on a long or interrupted swing that can
            // outstay the hit by a lot. Above 0 the cell instead runs for
            // exactly this long from the moment the swing starts and then
            // hands back to the parent state (sheathed, drawn, whatever you
            // were in), so a punch can be a punch regardless of how long the
            // animation happens to be.
            float attackDuration = 0.0f;  // 0..5

            HitShake::Tuning hitShake{}; // confirmed outgoing melee impacts; independent of swing noise
            // (A `roarDuration` lived here briefly. Removed: the werewolf roar
            // envelope's timing was already tuned and did not want a knob.)
            bool parentSeeded = false;  // persisted one-time override initialization
            bool operator==(const NoiseProfile&) const = default;
        };

        // Always-on now that the master Enable Camera Noise toggle has
        // been removed; defaults to true, force-true on load. The
        // intended way to mute noise is to leave the per-channel
        // sliders at 0 and per-state Customize toggles off.
        bool         noiseEnabled = true;
        NoiseProfile globalNoise;
        // Indoor variant of globalNoise. Runtime resolver picks this when
        // the active environment is Indoor; UI edits via EditTargetGlobalNoise.
        NoiseProfile globalNoiseIndoor;

        // --- Paragliding (Cinematic Effects; mod support) -------------------
        // Skyrim's Paraglider integration: a full camera profile and a full
        // noise cell of its own, DELIBERATELY kept out of the Categories /
        // Camera Noise trees (those are state trees; a glider is a mod-added
        // condition) and surfaced as one Cinematic Effects entry instead.
        // Detection is the paraglider's own behavior-graph notifications
        // ("StartPara"/"EndPara" — see StateResolver::IsParagliding), so with
        // the mod absent nothing ever fires and all of this stays dormant.
        // While active and paraglideEnabled (declared further up), the profile
        // outranks EVERY other framing (states, target lock, enemy overrides)
        // and the cell owns the noise — the standard springs / crossfades
        // carry both transitions. No env variants: a glide is its own place.
        //
        // The TL variant applies while gliding AND target-locked — the wide
        // scenic framing gives way to a fight framing on the lock, following
        // the TL tree's own convention: it only engages once the user has
        // TUNED it (non-default), so an untouched lock mid-glide keeps the
        // scenic framing instead of snapping to defaults.
        CameraProfile paraglideProfile{};
        CameraProfile paraglideTLProfile{};
        NoiseProfile  paraglideNoise{};
        // Indoor twin of paraglideNoise (2026-09-05, "add an outdoor/indoor
        // toggle to paragliding"). The camera/TL profiles already carry their
        // indoor variants through GetIndoorEligibleProfiles; the noise cell is
        // a standalone field, so its twin is one too. Seeded as a copy of the
        // outdoor cell when the preset has no [cinematic.paraglide.noise_indoor].
        NoiseProfile  paraglideNoiseIndoor{};

        // First-person profile — same shape as the per-state variant but
        // carries Enable-Customization flags for FOV and Camera Noise
        // independently. A user who wants per-state FOV but inherited
        // global noise (or vice versa) flips only the flag they care
        // about. With both off the per-state entry is inert and the
        // resolver falls back to firstPersonGlobal for both subsystems.
        // transitionSpeed: 0 = use the global firstPersonTransitionSpeed.
        // Any positive value overrides on this entry only.
        struct FirstPersonProfile
        {
            // ABSOLUTE per-entry speed, default 1.00 (user ruling
            // 2026-08-31 — the old 0 = inherit-global sentinel is dead;
            // with the Global tab gone, "inherit" pointed at nothing the
            // user could see, and sliders reading 0.00 looked broken).
            float        transitionSpeed = 1.0f;
            float        worldFov        = 80.0f;  // 40..140, vanilla 80
            float        handsFov        = 80.0f;  // 40..140, vanilla 80
            // Per-FoF-cell repulse strength. Multiplies the burst peak
            // on release. Only meaningful on .fire_and_forget /
            // .ritual cells; ignored elsewhere. Default 0 = no burst;
            // user must dial above 0 to opt in. >1 = stronger kick.
            float        repulse         = 0.0f;   // 0..3
            // Repulse Feel — same axis as the 3p per-entry slider. 0 =
            // heavy dead-blow, 0.5 = shipped shape, 1 = sharp snap with
            // a springy overshoot.
            float        repulseFeel     = 0.5f;   // 0..1
            // Per-FoF-cell post-fire fade duration (seconds). Added on
            // top of the 0.6s burst-window hold to form the total
            // post-fire tail. Default 0 = no fade beyond the hold;
            // user must dial above 0 to opt in. Only meaningful on
            // .fire_and_forget / .ritual cells.
            float        fofFadeDuration = 0.0f;   // 0..4
            // Shout post-fire lingering tail. Adds to the Phase 2 fade
            // duration after the voice fires. Default 0 = quick fade
            // (Phase 2 runs ~0.30s). User dials up for a longer linger
            // matching the voice line's tail. Only meaningful on
            // shouts.<name>[.sneak] cells.
            float        shoutFadeDuration = 0.0f; // 0..4
            NoiseProfile noise;                    // enabled/amp/speed/sway/tilt/wobble
        };

        // Sub-gates living in the Global tab. Each independently controls
        // whether its slider set is applied at runtime. Both off = the
        // plugin defers to vanilla 1p entirely; turning one on exposes
        // the matching per-category tabs and applies that subsystem.
        // First Person is always enabled — the FOV and Camera Noise
        // subsystems apply whenever a 1p profile resolves. These flags are
        // kept (the runtime + UI still read them) but are pinned true; the
        // old Global-tab "Enable" toggles were removed.
        bool firstPersonFovEnabled   = true;
        bool firstPersonNoiseEnabled = true;

        // (Removed 2026-08-01: firstPersonSpellLevelScaling — the 1p magic
        // shake used to scale with the spell's Novice..Master tier. The cell's
        // own Intensity is the sole amplitude authority now. The old TOML key
        // first_person.spell_level_scaling is simply ignored on load.)

        // Always-active baseline. Resolution returns this when the per-
        // state entry is absent OR its customize flag is off. customize
        // is meaningless on the global slot (always active).
        FirstPersonProfile firstPersonGlobal;

        // Per-state customizations keyed by Categories tomlKey
        // (mirrors stateNoise). Map only stores entries the user has
        // opened; entries with customize=false are inert.
        std::unordered_map<std::string, FirstPersonProfile> stateFirstPerson;
        // [FP-MERGE] THE only sanctioned way for UI code to create a state
        // entry: presence in the map IS the bind now, so a bare
        // stateFirstPerson[key] from a hover would insert defaults that
        // OVERRIDE Global with fov 80 / silence (the browse-trap class of
        // bug). Seeding from Global makes a freshly created entry
        // behaviourally identical to no entry at all.
        [[nodiscard]] FirstPersonProfile& EnsureStateFp(const std::string& a_key)
        {
            auto it = stateFirstPerson.find(a_key);
            if (it == stateFirstPerson.end()) {
                FirstPersonProfile seeded = firstPersonGlobal;
                // (transitionSpeed inherits Global's value via the copy.)
                it = stateFirstPerson.emplace(a_key, std::move(seeded)).first;
            }
            return it->second;
        }

        // Per-weapon-type First Person overrides, keyed by the same
        // Categories tomlKey as stateFirstPerson (melee states only).
        // FOV and Noise have independent enable arrays so a weapon can
        // override one subsystem without the other. The stored
        // FirstPersonProfile carries both worldFov/handsFov and noise.
        // Resolved by ResolveFirstPersonFovProfile /
        // ResolveFirstPersonNoiseProfile when the active state is a melee
        // key and the player's weapon matches an enabled slot.
        struct FpCustomMeleeSlot
        {
            std::string        keyword;   // WeapType* keyword editor ID
            bool               setFov   = false;
            bool               setNoise = false;
            FirstPersonProfile profile{};
        };
        struct FpMeleeWeaponOverrides
        {
            std::array<FirstPersonProfile, kMeleeWeaponCount> perWeapon{};
            std::array<bool, kMeleeWeaponCount>               perWeaponSetFov{};
            std::array<bool, kMeleeWeaponCount>               perWeaponSetNoise{};
            // Mod-added weapon types (the 3p CustomMeleeSlot's FP twin),
            // keyed by keyword; shares the s.customWeaponTypes catalog.
            std::vector<FpCustomMeleeSlot>                    custom;
        };
        std::unordered_map<std::string, FpMeleeWeaponOverrides> stateFpMeleeOverrides;

        // Global transition rate (FOV + noise crossfade). 1.0 = ~0.5s
        // critical-damped spring; 0.1 ≈ 5s, 10.0 ≈ 50ms.
        float firstPersonTransitionSpeed = 1.0f;  // 0.1..10.0 (default 1.00, user ruling 2026-08-31)

        // Resolve the currently-active 1p profile from live player
        // state. Two flavors so FOV and Noise can come from different
        // sources (e.g. a state with FOV override but inherited noise).
        // Each checks the matching customize flag; never returns nullptr.
        [[nodiscard]] FirstPersonProfile* ResolveFirstPersonFovProfile();
        // [FPFOV] diagnostics: names the storage a resolved FP pointer
        // belongs to ("state-entry", "weapon-type-override", "global"...).
        [[nodiscard]] const char* DescribeFpProfileSource(const FirstPersonProfile* a_p);
        [[nodiscard]] FirstPersonProfile* ResolveFirstPersonNoiseProfile();
        // The exact first-person state key the player is physically in (the
        // primary resolved key), independent of which overrides exist. Used by
        // the Quick Tune header; boxes label from the resolved profile instead.
        [[nodiscard]] std::string ResolveFirstPersonStateKey();
        // Returns the per-weapon-type FP override for the given active
        // state key when it is a melee key and the player's current melee
        // weapon matches an enabled slot, else nullptr. a_fov selects the
        // FOV vs Noise enable array. Takes priority over the per-state
        // profile in both resolvers above.
        [[nodiscard]] FirstPersonProfile* ResolveFpMeleeOverride(const std::string& a_key, bool a_fov);
        // Specific-form binding layer for First Person — most specific, wins
        // over the per-weapon-type and per-state layers. Maps the resolved
        // primary FP state key to a (binding category, sub-state slot), then
        // matches the equipped forms (or, while shouting, the active
        // mod-added shout) against weaponBindings. Returns the binding's
        // FirstPersonProfile when the requested half's values were touched,
        // else nullptr. a_fov selects the FOV vs Noise half.
        [[nodiscard]] FirstPersonProfile* ResolveFpBindingProfile(const std::string& a_primaryKey, bool a_fov);
        // Per-state customizations keyed by tomlKey from
        // GetIndoorEligibleProfiles. CameraNoiseController resolves the
        // current Categories state, looks up its tomlKey here; if missing
        // OR enabled=false, falls back to globalNoise. Map only stores
        // entries the user has touched, keeping save files compact.
        std::unordered_map<std::string, NoiseProfile> stateNoise;
        // Parallel indoor variant. Runtime resolver picks from this map
        // when the active environment is Indoor; UI edits via
        // EditTargetStateNoise.
        std::unordered_map<std::string, NoiseProfile> stateNoiseIndoor;

        // UI helpers — return the variant matching categoriesEditTab
        // (0 Outdoor / 1 Indoor), or the location override currently being
        // edited when the Location popup is open (locationEditActive). The
        // state-key variant default-constructs the entry on first access
        // (same as operator[]) so the UI can write to a fresh slot directly.
        [[nodiscard]] NoiseProfile& EditTargetGlobalNoise();
        [[nodiscard]] NoiseProfile& EditTargetStateNoise(const std::string& key);
        // Env-indexed accessors (0 Outdoor / 1 Indoor). Out-of-range
        // clamps to Outdoor, so a corrupted persisted tab index is harmless.
        [[nodiscard]] NoiseProfile& GlobalNoiseFor(int a_env);
        [[nodiscard]] std::unordered_map<std::string, NoiseProfile>& StateNoiseFor(int a_env);

        // Resolve the per-state NoiseProfile for the given CameraProfile
        // pointer. Walks GetIndoorEligibleProfiles() once (cached) to
        // build a pointer→tomlKey map, then looks up stateNoise. Returns
        // a pointer to the per-state entry if it exists AND is enabled,
        // otherwise nullptr — caller should fall back to globalNoise.
        // a_outKey (optional): receives the resolved noise STATE key; a_outWeapon
        // (optional): the per-weapon-override slot used (-1 if none);
        // a_outBindingLabel (optional): a ready "<Binding> - <SubState>" label
        // when the noise resolved via a per-form weapon binding (empty otherwise).
        // Lets a caller name the exact override the noise tree landed on.
        [[nodiscard]] NoiseProfile* ResolveStateNoise(CameraProfile* a_profile,
                                                      std::string* a_outKey = nullptr,
                                                      int* a_outWeapon = nullptr,
                                                      std::string* a_outBindingLabel = nullptr);

        // Explicit NPC context; never use the player's equipment/camera state.
        // Direction uses the existing power-attack slot order, -1 unknown.
        [[nodiscard]] const NoiseProfile* ResolveNpcMeleeNoise(bool power, bool sneak, bool sprint,
            MeleeWeaponType weapon, int direction = -1, const ItemBindings::EquippedItem& item = {},
            std::string* outKey = nullptr);
        [[nodiscard]] const NoiseProfile* ResolveNpcShoutNoise(std::string_view bucket, std::string_view shoutKey,
            bool sneak, const ItemBindings::EquippedItem& shout = {}, std::string* outKey = nullptr);
        [[nodiscard]] const NoiseProfile* ResolveNpcTransformationNoise(NpcNoise::Form form, NpcNoise::Action action);
        [[nodiscard]] const NoiseProfile* FindNpcStateNoise(std::string_view key, std::string* outKey = nullptr);

        [[nodiscard]] NoiseProfile* ResolveHitShakeProfile(std::string_view key, bool firstPerson,
            MeleeWeaponType weapon, const ItemBindings::EquippedItem& item = {},
            std::string_view customKeyword = {}, std::string* outKey = nullptr, bool mountedArchery = false);
        [[nodiscard]] bool IsHitShakeLocationKey(std::string_view key) const;

        // Maps a resolved Categories/TL camera-profile pointer back to the
        // weapon binding + sub-state slot it belongs to (if any). Used by
        // Quick Tune to edit a bound weapon's OWN noise/TL slots instead of
        // the generic state it would inherit. Returns false for a normal
        // (non-binding) profile. (WeaponBinding is defined further down.)
        struct WeaponBinding;
        [[nodiscard]] const NoiseProfile* ResolveNpcBindingNoise(WeaponBinding* binding, int slot,
                                                                std::string* outKey = nullptr);
        [[nodiscard]] bool FindBindingSlotForProfile(CameraProfile* a_profile,
                                                     WeaponBinding*& a_outBinding,
                                                     int& a_outSlot);

        // Per-weapon-type noise overrides for each Melee state. Parallel
        // to MeleeWeaponOverrides but for NoiseProfile. Resolved by
        // ResolveStateNoise when the parent Melee state's tomlKey matches
        // one of the 5 Melee buckets; the player's current weapon type
        // is checked against the per-weapon slot, returning the override
        // when enabled or falling through to stateNoise[base].
        struct MeleeWeaponNoiseOverrides
        {
            std::array<NoiseProfile, kMeleeWeaponCount> perWeapon{};
            std::array<bool, kMeleeWeaponCount>         perWeaponSet{};
        };
        MeleeWeaponNoiseOverrides weaponsMeleeNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSprintNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSwimNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeAttackNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSneakNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeShoutNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeShoutSneakNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleePowerAttackNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSneakAttackNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSneakPowerAttackNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSprintAttackNoiseOverrides;
        MeleeWeaponNoiseOverrides weaponsMeleeSprintPowerAttackNoiseOverrides;
        // Per-weapon-type noise overrides for each directional power-attack
        // direction. Declared after kPowerAttackDirectionCount (below) via
        // weaponsMeleePowerAttackDirNoiseOverrides near the directional
        // camera overrides.

        // Mod-added weapon type catalog (2026-08-15). Each entry is a
        // WeapType* keyword the user bound from the Weapon Type Overrides
        // popup's Custom tab; every bound type gets its own tab in that
        // popup across all melee entries. Matching is by keyword, so any
        // mod's spears/halberds/katanas work with no support list.
        struct CustomWeaponType
        {
            std::string keyword;      // e.g. "WeapTypeSpear"
            std::string displayName;  // e.g. "Spear"
        };
        std::vector<CustomWeaponType> customWeaponTypes;

        // The bound custom weapon-type keyword the player's CURRENT melee
        // weapon matches (first catalog hit), or nullptr. Resolved fresh
        // per call — cheap (a handful of keyword compares).
        [[nodiscard]] const std::string* CurrentCustomMeleeKeyword() const;




        CameraProfile sheathed       = CameraProfile::Default3p();
        CameraProfile sheathedSprint = CameraProfile::Default3p();
        CameraProfile sheathedSwim   = CameraProfile::Default3p();
        CameraProfile sheathedSneak  = CameraProfile::Default3p();

        CameraProfile weaponsMelee       = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSprint = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSwim   = CameraProfile::Default3p();
        CameraProfile weaponsMeleeAttack = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSneak  = CameraProfile::Default3p();
        // Attack variants split by power-attack flag + sneak/sprint. Resolved
        // in the Attack sub-state branch via StateResolver power/sneak/sprint
        // flags. weaponsMeleeSprintAttack is the MCO/BFCO "normal sprint
        // attack" case (vanilla sprint attacks always set the power flag, so
        // vanilla routes to weaponsMeleeSprintPowerAttack instead).
        CameraProfile weaponsMeleePowerAttack        = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSneakAttack        = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSneakPowerAttack   = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSprintAttack       = CameraProfile::Default3p();
        CameraProfile weaponsMeleeSprintPowerAttack  = CameraProfile::Default3p();

        // Directional power-attack overrides — Base profile above is the
        // fallback. Index order matches StateResolver::PowerAttackDirection:
        // 0=InPlace, 1=Forward, 2=Back, 3=Left, 4=Right.
        static constexpr std::size_t kPowerAttackDirectionCount = 5;
        std::array<CameraProfile, kPowerAttackDirectionCount> weaponsMeleePowerAttackDir{
            CameraProfile::Default3p(), CameraProfile::Default3p(),
            CameraProfile::Default3p(), CameraProfile::Default3p(),
            CameraProfile::Default3p() };
        std::array<bool, kPowerAttackDirectionCount> weaponsMeleePowerAttackDirEnabled{};
        // Per-weapon-type overrides for each directional power-attack profile
        // above. Resolved like the other melee buckets when the directional
        // override is enabled, so a Forward power attack can frame
        // differently per weapon type.
        std::array<MeleeWeaponOverrides, kPowerAttackDirectionCount> weaponsMeleePowerAttackDirOverrides{};
        // Noise twin of the above (shared across Categories/Target Lock,
        // like the directional noise profiles). Resolved by ResolveStateNoise
        // for the directional power-attack keys.
        std::array<MeleeWeaponNoiseOverrides, kPowerAttackDirectionCount>
            weaponsMeleePowerAttackDirNoiseOverrides{};

        // Per-weapon-type and per-weight-class overrides for each base
        // Melee profile above. Two modes per struct, mutually exclusive
        // in the UI. Resolved by ResolveMeleeOverride() given the player's
        // current weapon type. Unset overrides fall back to the base
        // profile above. See Settings/MeleeWeaponOverrides.h.
        MeleeWeaponOverrides weaponsMeleeOverrides;
        MeleeWeaponOverrides weaponsMeleeSprintOverrides;
        MeleeWeaponOverrides weaponsMeleeSwimOverrides;
        MeleeWeaponOverrides weaponsMeleeAttackOverrides;
        MeleeWeaponOverrides weaponsMeleeSneakOverrides;
        MeleeWeaponOverrides weaponsMeleeShoutOverrides;
        MeleeWeaponOverrides weaponsMeleeShoutSneakOverrides;
        MeleeWeaponOverrides weaponsMeleePowerAttackOverrides;
        MeleeWeaponOverrides weaponsMeleeSneakAttackOverrides;
        MeleeWeaponOverrides weaponsMeleeSneakPowerAttackOverrides;
        MeleeWeaponOverrides weaponsMeleeSprintAttackOverrides;
        MeleeWeaponOverrides weaponsMeleeSprintPowerAttackOverrides;

        // Per-specific-form binding. Sits ABOVE the per-weapon-type
        // layer in each category's resolution chain: a binding for
        // "Dawnbreaker" wins over the Sword type override which wins
        // over the base Melee profile. Each binding carries one
        // CameraProfile per sub-state with an independent enabled
        // flag, so a user can override Attacking-only without disturbing
        // the base.
        //
        // The struct holds up to 12 sub-state slots; only the first
        // GetBindingSubStateCount(category) are used per category:
        //   Melee    (5): Melee / Sprinting / Swimming / Attacking / Sneak
        //   Bow      (6): Bow / Bow Sprinting / Bow Drawing / Bow Swimming /
        //                 Bow Sneak / Bow Sneak Drawing
        //   Crossbow (6): same shape as Bow
        //   Spell    (5): Magic / Magic Sprinting / Magic Swimming / Sneak /
        //                 Casting  (cast type stored separately on binding)
        //   Staff    (4): Staves / Staves Sprinting / Staves Swimming / Sneak
        //   Shield   (3): Shield / Shield Sneak / Shield Sprinting
        //   Shout    (2): Shouting / Shouting (Sneaking) — MOD-ADDED shouts
        //                 only (a TESShout that isn't one of the known 27 and
        //                 whose name matches no vanilla shout). Fires while
        //                 the bound shout is being cast, winning over the
        //                 per-state Shouts base in any shoutable state.
        enum class BindingCategory : std::uint8_t {
            Melee    = 0,
            Bow      = 1,
            Crossbow = 2,
            Spell    = 3,
            Staff    = 4,
            Shield   = 5,
            Shout    = 6,
        };
        static constexpr std::size_t kBindingCategoryCount      = 7;
        static constexpr std::size_t kWeaponBindingSubStates    = 15;
        // Per-hand magic slots: 0=Left, 1=Both/Dual-cast, 2=Right (index ==
        // StateResolver::CastingHand). Shared by the school-grid overrides
        // further down and the Spell bindings' per-hand slot arrays below.
        static constexpr std::size_t kMagicHandCount            = 3;

        // SpellItem cast type, copied onto Spell bindings at bind time
        // so the runtime can label/route the spell without re-classifying
        // the form. 0 = unknown (default for non-Spell bindings).
        enum class SpellCastType : std::uint8_t {
            Unknown      = 0,
            FireAndForget = 1,
            Concentration = 2,
        };

        // Per-field enemy override: 6 floats + 6 enable bools spliced onto a
        // picked TL profile when locked onto a tracked enemy type. Defined
        // here (ahead of WeaponBinding) so bindings can carry their own set.
        // See the Enemy Overrides block further down for the slot vocabulary.
        static constexpr std::size_t kEnemyOverrideEnemies = 5;
        struct EnemyFieldOverride {
            // The camera fields apply together (like a weapon-type override's
            // profile). Stored as a CameraProfile so the editor and TOML reuse
            // the exact same machinery / vanilla defaults as the TL tabs.
            CameraProfile profile{};
            // Aim bias used to live here as its own float + flag. It moved to
            // CameraProfile::transitionAimBias on 2026-09-07 so entries and
            // enemies share one storage and one editor (the Transition
            // Override popup) — see the comment on that field. This struct's
            // `profile` carries the enemy's value now.
            bool  fieldsEnabled      = false;
        };

        struct WeaponBinding
        {
            std::uint32_t   formID    = 0;
            std::string     pluginName;
            std::string     displayName;
            ItemBindings::Scope bindingScope = ItemBindings::Scope::ExactForm;
            std::string     enchantmentKey;
            bool            settingsSeeded = false;
            BindingCategory category  = BindingCategory::Melee;
            SpellCastType   castType  = SpellCastType::Unknown;
            // Created from the First Person tab: the loader must NOT
            // normalize the three third-person sections' enables to true for
            // this binding (that normalization is what kept resurrecting the
            // "FP bind changes my 3p framing" leak on every preset load).
            // Cleared by the "Use In Third Person Sections" toggle.
            bool            fpOnly    = false;
            // Categories sliders — fires when the binding is held.
            std::array<CameraProfile, kWeaponBindingSubStates> profiles{};
            std::array<bool, kWeaponBindingSubStates>          enabled{};
            // Target Lock sliders — fires when locked on a target with
            // the binding held. Same shape, independent values.
            std::array<CameraProfile, kWeaponBindingSubStates> tlProfiles{};
            std::array<bool, kWeaponBindingSubStates>          tlEnabled{};
            // Camera Noise per-sub-state. Same shape, independent values.
            std::array<NoiseProfile, kWeaponBindingSubStates>  noiseProfiles{};
            std::array<bool, kWeaponBindingSubStates>          noiseEnabled{};
            // Indoor variants — used when the player is in an interior cell
            // (settings.indoorMode). The slot's enabled flag is shared with
            // the outdoor variant; only the profile values differ. Lets a
            // bound weapon frame differently indoors vs outdoors, matching the
            // base-profile indoor/outdoor system.
            // Separate indoor variants — interiors always use these (like the
            // base profiles). Default to vanilla (NOT a copy of outdoor); kept
            // in sync via the Copy Outdoor/Indoor button.
            std::array<CameraProfile, kWeaponBindingSubStates> profilesIndoor{};
            std::array<CameraProfile, kWeaponBindingSubStates> tlProfilesIndoor{};
            std::array<NoiseProfile, kWeaponBindingSubStates>  noiseProfilesIndoor{};
            // Env selectors — keep the choice in ONE place so a resolution
            // site can't accidentally cover only one environment.
            [[nodiscard]] std::array<CameraProfile, kWeaponBindingSubStates>& ProfilesFor(int e)
            {
                return e == kEnvIndoor ? profilesIndoor : profiles;
            }
            [[nodiscard]] std::array<CameraProfile, kWeaponBindingSubStates>& TlProfilesFor(int e)
            {
                return e == kEnvIndoor ? tlProfilesIndoor : tlProfiles;
            }
            [[nodiscard]] std::array<NoiseProfile, kWeaponBindingSubStates>& NoiseProfilesFor(int e)
            {
                return e == kEnvIndoor ? noiseProfilesIndoor : noiseProfiles;
            }
            // Per-(enemy, sub-state) field overrides — spliced when locked
            // onto a tracked enemy while this binding's TL profile is active.
            // Indoor/outdoor share one set (enemy overrides are not env-split).
            std::array<std::array<EnemyFieldOverride, kWeaponBindingSubStates>, kEnemyOverrideEnemies>
                tlEnemyOverrides{};
            // Indoor twin. Enemy overrides became env-split on 2026-09-07
            // (user: "they should be entry specific and outdoor/indoor
            // specific"), so the comment above this pair no longer holds —
            // interiors read these. Default to vanilla, NOT a copy of outdoor,
            // matching the custom-enemy twins rather than the base camera
            // profiles: an override the user tuned for a dragon outdoors is a
            // statement about that fight, not a value to inherit everywhere.
            std::array<std::array<EnemyFieldOverride, kWeaponBindingSubStates>, kEnemyOverrideEnemies>
                tlEnemyOverridesIndoor{};
            [[nodiscard]] std::array<std::array<EnemyFieldOverride, kWeaponBindingSubStates>, kEnemyOverrideEnemies>&
            TlEnemyOverridesFor(int e)
            {
                return e == kEnvIndoor ? tlEnemyOverridesIndoor : tlEnemyOverrides;
            }
            [[nodiscard]] const std::array<std::array<EnemyFieldOverride, kWeaponBindingSubStates>, kEnemyOverrideEnemies>&
            TlEnemyOverridesFor(int e) const
            {
                return e == kEnvIndoor ? tlEnemyOverridesIndoor : tlEnemyOverrides;
            }

            // Per-CUSTOM-enemy overrides owned by THIS binding (2026-08-15).
            // A freshly bound weapon must NOT inherit the slot-keyed custom
            // enemy overrides tuned for its parent category — it starts with
            // none and stores its own, keyed by the custom enemy's identity
            // (CustomEnemyIdentityKey; index-stable across removals). The
            // binding's sub-state index already encodes the directional
            // power attacks (melee slots 10-14), so no separate paDir arrays.
            struct CustomEnemyGrid
            {
                std::string identityKey;
                std::array<EnemyFieldOverride, kWeaponBindingSubStates> slots{};
                std::array<EnemyFieldOverride, kWeaponBindingSubStates> slotsIndoor{};
                [[nodiscard]] std::array<EnemyFieldOverride, kWeaponBindingSubStates>& SlotsFor(int e)
                {
                    return e == kEnvIndoor ? slotsIndoor : slots;
                }
                [[nodiscard]] const std::array<EnemyFieldOverride, kWeaponBindingSubStates>& SlotsFor(int e) const
                {
                    return e == kEnvIndoor ? slotsIndoor : slots;
                }
            };
            std::vector<CustomEnemyGrid> customEnemyGrids;

            // ── Per-hand overrides (Spell bindings only) ──────────────────
            // [hand 0..2][sub-state]. Hand-specific variants of the slot's
            // Categories / Target Lock / Noise values, env-split like the
            // base slots (enable flags shared between environments). A hand
            // override is its own layer: it fires whenever the resolved hand
            // matches and its flag is on, independent of the slot's base
            // Enable Override toggle — mirroring how per-weapon-type
            // overrides fire independently on melee states. Non-Spell
            // bindings never read these.
            // CASTING-ONLY: although the arrays span every sub-state slot
            // (uniform storage), ResolveSpellBindingHand only ever resolves
            // a hand for the Casting slot (3) — per-hand customization is
            // an active-cast feature, matching the school grids. The other
            // slots' cells are unreachable and the UI doesn't expose them.
            std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount> handProfiles{};
            std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount> handProfilesIndoor{};
            std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount> handTlProfiles{};
            std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount> handTlProfilesIndoor{};
            std::array<std::array<bool, kWeaponBindingSubStates>, kMagicHandCount>          handEnabled{};
            std::array<std::array<bool, kWeaponBindingSubStates>, kMagicHandCount>          handTlEnabled{};
            std::array<std::array<NoiseProfile, kWeaponBindingSubStates>, kMagicHandCount>  handNoise{};
            std::array<std::array<NoiseProfile, kWeaponBindingSubStates>, kMagicHandCount>  handNoiseIndoor{};
            std::array<std::array<bool, kWeaponBindingSubStates>, kMagicHandCount>          handNoiseEnabled{};
            [[nodiscard]] std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount>&
            HandProfilesFor(int e)
            {
                return e == kEnvIndoor ? handProfilesIndoor : handProfiles;
            }
            [[nodiscard]] std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount>&
            HandTlProfilesFor(int e)
            {
                return e == kEnvIndoor ? handTlProfilesIndoor : handTlProfiles;
            }
            [[nodiscard]] std::array<std::array<NoiseProfile, kWeaponBindingSubStates>, kMagicHandCount>&
            HandNoiseFor(int e)
            {
                return e == kEnvIndoor ? handNoiseIndoor : handNoise;
            }

            // ── First Person overrides (all categories) ──────────────────
            // One 1p profile (FOV + noise) per sub-state slot, edited from
            // the First Person section's Specific Weapons tab. Unlike the
            // 3p slots these are OPT-IN: each half applies only once its
            // values are actually touched (binding a
            // weapon must not silently pin the 1p FOV). Resolved by
            // ResolveFpBindingProfile above the per-weapon-type and
            // per-state layers. Not env-split — First Person has no
            // indoor variant anywhere.
            std::array<FirstPersonProfile, kWeaponBindingSubStates> fpProfiles{};
        };

        // Which hand tab governs a Spell binding right now. CASTING-ONLY:
        // returns the live cast-hand for the Casting slot (3) and -1 for
        // every other slot / no active cast / non-Spell binding — holding
        // a bound spell never routes per-hand. Shared by the camera picker
        // (CameraController) and the noise resolver so both route the same
        // hand for the same frame.
        [[nodiscard]] static int ResolveSpellBindingHand(const WeaponBinding& b, int a_slot);
        [[nodiscard]] static bool IsHitShakeBindingSlot(const WeaponBinding& b, int slot);

        // Suffix for display purposes (" (Fire & Forget)" / " (Concentration)").
        // Returns empty string for Unknown / non-spell.
        static const char* GetCastTypeSuffix(SpellCastType c);

        // Holds bindings for ALL categories; filtered by `category` at
        // the consumer. Replaces the earlier melee-only vector.
        std::vector<WeaponBinding> weaponBindings;

        // The fallback framing for a Specific Weapons slot that has no parent
        // to copy: the shipped third-person default, for EVERY category. Bow,
        // crossbow, magic, staves, shield and shout parents all initialize to
        // CameraProfile::Default3p (see the profile declarations above and
        // ResetAllToVanilla), so the Melee-only special case that used to live
        // here left the other six categories seeding and resetting to the
        // engine's VanillaCombat numbers (zoom 0) that the mod never ships.
        [[nodiscard]] static constexpr CameraProfile BindingDefault([[maybe_unused]] BindingCategory category)
        {
            return CameraProfile::Default3p();
        }

        // New equipment bindings copy their parent once, then own their values.
        void SeedWeaponBinding(WeaponBinding& binding, const ItemBindings::EquippedItem& item);

        // Per-category metadata for the binding UI / runtime resolution.
        // constexpr so the preset-compatibility static_asserts at the bottom
        // of this class can pin these counts — they are persisted contracts,
        // not just UI sizes (see the APPEND-ONLY block).
        [[nodiscard]] static constexpr std::size_t GetBindingSubStateCount(BindingCategory c)
        {
            switch (c) {
            case BindingCategory::Melee:    return 15;
            case BindingCategory::Bow:      return 8;   // 6-7 appended 2026-08-23 (Zoom / Zoom Sneaking)
            case BindingCategory::Crossbow: return 8;   // same shape as Bow
            case BindingCategory::Spell:    return 5;
            case BindingCategory::Staff:    return 4;
            case BindingCategory::Shield:   return 3;
            case BindingCategory::Shout:    return 2;
            }
            return 0;
        }
        static const char*       GetBindingSubStateName (BindingCategory c, std::size_t idx);
        static const char*       GetBindingCategoryName (BindingCategory c);

        // Active while the player is blocking (or actively casting a ward).
        // Split into 9 sub-states based on what the player is blocking with —
        // resolved by StateResolver via BlockKind. Sneak variants apply
        // when the player is sneaking; Sprint applies only to Shield (the
        // engine allows shield-bash sprinting but not weapon-block sprint).
        CameraProfile weaponsBlockingOneHanded      = CameraProfile::Default3p();
        CameraProfile weaponsBlockingTwoHanded      = CameraProfile::Default3p();
        CameraProfile weaponsBlockingShield         = CameraProfile::Default3p();
        CameraProfile weaponsBlockingWard           = CameraProfile::Default3p();
        CameraProfile weaponsBlockingOneHandedSneak = CameraProfile::Default3p();
        CameraProfile weaponsBlockingTwoHandedSneak = CameraProfile::Default3p();
        CameraProfile weaponsBlockingShieldSneak    = CameraProfile::Default3p();
        CameraProfile weaponsBlockingWardSneak      = CameraProfile::Default3p();
        CameraProfile weaponsBlockingShieldSprint   = CameraProfile::Default3p();

        // Ward sub-states are opt-in: when disabled (default) the camera
        // resolver falls through to the Restoration Concentration magic
        // profile so the Ward entry doesn't conflict with the spell's own
        // setting. Enable to force the dedicated Ward / Ward Sneak profile.
        bool weaponsBlockingWardEnabled      = false;
        bool weaponsBlockingWardSneakEnabled = false;

        CameraProfile weaponsBow       = CameraProfile::Default3p();
        CameraProfile weaponsBowSprint = CameraProfile::Default3p();
        CameraProfile weaponsBowSwim   = CameraProfile::Default3p();
        CameraProfile weaponsBowDraw   = CameraProfile::Default3p(); // active while drawing/holding the bow (IsAttacking)
        CameraProfile weaponsBowSneak  = CameraProfile::Default3p();
        CameraProfile weaponsBowSneakDraw = CameraProfile::Default3p(); // sneaking + drawing the bow
        // BOW ZOOM — the Eagle Eye state, and it REPLACES the engine's zoom
        // rather than layering on it. DDC already writes worldFOV every frame
        // from the resolved profile, so the vanilla zoom was being flattened
        // anyway; this makes that deliberate and tunable. A profile FOV BELOW
        // the base zooms in, ABOVE it zooms out — the "do the complete
        // opposite" case costs nothing because the profile simply wins.
        //
        // Detected from PlayerCamera::bowZoomedIn, which the engine's own
        // BowZoom anim-event handler sets. That is deliberately NOT a perk
        // check: mods that hand the zoom out at level 1, or move it behind a
        // different perk, all still fire the same event. See
        // [[compatibility-first-design]].
        CameraProfile weaponsBowZoom      = CameraProfile::Default3p();
        CameraProfile weaponsBowSneakZoom = CameraProfile::Default3p();

        CameraProfile weaponsCrossbow       = CameraProfile::Default3p();
        CameraProfile weaponsCrossbowSprint = CameraProfile::Default3p();
        CameraProfile weaponsCrossbowSwim   = CameraProfile::Default3p();
        CameraProfile weaponsCrossbowDraw   = CameraProfile::Default3p(); // active while drawing/holding the crossbow (IsAttacking)
        CameraProfile weaponsCrossbowSneak  = CameraProfile::Default3p();
        CameraProfile weaponsCrossbowSneakDraw = CameraProfile::Default3p(); // sneaking + drawing the crossbow
        // Crossbows get the zoom too — everything else in Archery applies to
        // both, and bowZoomedIn is set by the event, not by the weapon type.
        CameraProfile weaponsCrossbowZoom      = CameraProfile::Default3p();
        CameraProfile weaponsCrossbowSneakZoom = CameraProfile::Default3p();

        CameraProfile weaponsMagic       = CameraProfile::Default3p();
        CameraProfile weaponsMagicSprint = CameraProfile::Default3p();
        CameraProfile weaponsMagicSwim   = CameraProfile::Default3p();
        CameraProfile weaponsMagicSneak  = CameraProfile::Default3p();

        CameraProfile magicAlterationConcentration = CameraProfile::Default3p();
        CameraProfile magicAlterationFireAndForget = CameraProfile::Default3p();
        CameraProfile magicAlterationRitual        = CameraProfile::Default3p();

        CameraProfile magicConjurationConcentration = CameraProfile::Default3p();
        CameraProfile magicConjurationFireAndForget = CameraProfile::Default3p();
        CameraProfile magicConjurationRitual        = CameraProfile::Default3p();

        CameraProfile magicDestructionConcentration = CameraProfile::Default3p();
        CameraProfile magicDestructionFireAndForget = CameraProfile::Default3p();
        CameraProfile magicDestructionRitual        = CameraProfile::Default3p();

        CameraProfile magicIllusionConcentration = CameraProfile::Default3p();
        CameraProfile magicIllusionFireAndForget = CameraProfile::Default3p();
        CameraProfile magicIllusionRitual        = CameraProfile::Default3p();

        CameraProfile magicRestorationConcentration = CameraProfile::Default3p();
        CameraProfile magicRestorationFireAndForget = CameraProfile::Default3p();
        CameraProfile magicRestorationRitual        = CameraProfile::Default3p();

        // Sneak-variant overrides for active spell casts. PickMagicProfile
        // returns these instead when the player is sneaking.
        CameraProfile magicAlterationConcentrationSneak  = CameraProfile::Default3p();
        CameraProfile magicAlterationFireAndForgetSneak  = CameraProfile::Default3p();
        CameraProfile magicAlterationRitualSneak         = CameraProfile::Default3p();

        CameraProfile magicConjurationConcentrationSneak = CameraProfile::Default3p();
        CameraProfile magicConjurationFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile magicConjurationRitualSneak        = CameraProfile::Default3p();

        CameraProfile magicDestructionConcentrationSneak = CameraProfile::Default3p();
        CameraProfile magicDestructionFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile magicDestructionRitualSneak        = CameraProfile::Default3p();

        CameraProfile magicIllusionConcentrationSneak    = CameraProfile::Default3p();
        CameraProfile magicIllusionFireAndForgetSneak    = CameraProfile::Default3p();
        CameraProfile magicIllusionRitualSneak           = CameraProfile::Default3p();

        CameraProfile magicRestorationConcentrationSneak = CameraProfile::Default3p();
        CameraProfile magicRestorationFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile magicRestorationRitualSneak        = CameraProfile::Default3p();

        // ── Per-hand magic overrides ─────────────────────────────────────
        // Each magic school × cast type (+ sneak variant) entry can carry
        // three optional hand-specific profiles: Left, Both/Dual-cast,
        // Right (index == StateResolver::CastingHand). When the resolver
        // reports a casting hand and that hand's slot is enabled, the hand
        // profile replaces the base school profile — Categories and Target
        // Lock keep independent grids (tl*), and the Camera Noise twin
        // lives in stateNoise under "<schoolKey>.hand.<left|both|right>".
        // Profiles default to CameraProfile{} ("untouched"); the UI seeds
        // them to the entry's VANILLA baseline on the enable edge so a
        // fresh hand override starts at default slider values.
        // (kMagicHandCount itself is declared up with the binding constants
        // so WeaponBinding can size its per-hand slot arrays.)
        struct MagicHandOverrideSet
        {
            std::array<CameraProfile, kMagicHandCount> profiles{};
            std::array<bool, kMagicHandCount>          enabled{};
        };
        // Display / TOML vocabulary for the 3 hand slots.
        static const char* GetMagicHandName(std::size_t h);     // "Left Hand" / "Both Hands" / "Right Hand"
        static const char* GetMagicHandTomlKey(std::size_t h);  // "left" / "both" / "right"
        // TOML fragments for the school/cast grid axes (index order matches
        // MagicSchool-1 / CastType-1).
        static const char* GetMagicSchoolTomlKey(std::size_t schoolIdx);
        static const char* GetMagicCastTomlKey(std::size_t castIdx);

        // [school 0..4][castType 0..2][sneak 0..1]
        using MagicHandGrid =
            std::array<std::array<std::array<MagicHandOverrideSet, 2>, 3>, 5>;
        MagicHandGrid magicHandOverrides{};    // Categories tree
        MagicHandGrid tlMagicHandOverrides{};  // Target Lock tree

        // Grid cell for a resolved (school, castType, sneak) tuple, or
        // nullptr when the tuple can't route per-hand (None school/type).
        [[nodiscard]] MagicHandOverrideSet* GetMagicHandSet(MagicSchool school, CastType castType,
                                                            bool isSneaking, bool a_targetLock);

        CameraProfile weaponsStaves       = CameraProfile::Default3p();
        CameraProfile weaponsStavesSprint = CameraProfile::Default3p();
        CameraProfile weaponsStavesSwim   = CameraProfile::Default3p();
        CameraProfile weaponsStavesSneak  = CameraProfile::Default3p();

        CameraProfile stavesAlterationConcentration = CameraProfile::Default3p();
        CameraProfile stavesAlterationFireAndForget = CameraProfile::Default3p();
        CameraProfile stavesAlterationRitual = CameraProfile::Default3p();

        CameraProfile stavesConjurationConcentration = CameraProfile::Default3p();
        CameraProfile stavesConjurationFireAndForget = CameraProfile::Default3p();
        CameraProfile stavesConjurationRitual = CameraProfile::Default3p();

        CameraProfile stavesDestructionConcentration = CameraProfile::Default3p();
        CameraProfile stavesDestructionFireAndForget = CameraProfile::Default3p();
        CameraProfile stavesDestructionRitual = CameraProfile::Default3p();

        CameraProfile stavesIllusionConcentration = CameraProfile::Default3p();
        CameraProfile stavesIllusionFireAndForget = CameraProfile::Default3p();
        CameraProfile stavesIllusionRitual = CameraProfile::Default3p();

        CameraProfile stavesRestorationConcentration = CameraProfile::Default3p();
        CameraProfile stavesRestorationFireAndForget = CameraProfile::Default3p();
        CameraProfile stavesRestorationRitual = CameraProfile::Default3p();

        // Sneak-variant overrides for active staff fires. PickStavesProfile
        // returns these instead when the player is sneaking.
        CameraProfile stavesAlterationConcentrationSneak  = CameraProfile::Default3p();
        CameraProfile stavesAlterationFireAndForgetSneak  = CameraProfile::Default3p();
        CameraProfile stavesAlterationRitualSneak  = CameraProfile::Default3p();

        CameraProfile stavesConjurationConcentrationSneak = CameraProfile::Default3p();
        CameraProfile stavesConjurationFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile stavesConjurationRitualSneak = CameraProfile::Default3p();

        CameraProfile stavesDestructionConcentrationSneak = CameraProfile::Default3p();
        CameraProfile stavesDestructionFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile stavesDestructionRitualSneak = CameraProfile::Default3p();

        CameraProfile stavesIllusionConcentrationSneak    = CameraProfile::Default3p();
        CameraProfile stavesIllusionFireAndForgetSneak    = CameraProfile::Default3p();
        CameraProfile stavesIllusionRitualSneak    = CameraProfile::Default3p();

        CameraProfile stavesRestorationConcentrationSneak = CameraProfile::Default3p();
        CameraProfile stavesRestorationFireAndForgetSneak = CameraProfile::Default3p();
        CameraProfile stavesRestorationRitualSneak = CameraProfile::Default3p();

        // Shouts can only be cast in specific states — blocking, werewolf,
        // vampire-lord and dragon-riding can't shout. We store per-state
        // configuration only for the shoutable ones.
        //
        // Fallback chain when casting a shout:
        //   1. Per-(state, shout) override (if that toggle is on)
        //   2. Per-state base (shoutsBaseByState[i])
        //   3. Outer weapon-state profile (vanilla-ish fallback)
        //
        // Index into the kShoutableStates array (7 entries). Non-shoutable
        // states map to no index — shouts just don't fire there.
        static constexpr std::size_t kShoutableStateCount = 6;

        // Per-state base profiles — apply to any shout cast in that state
        // that doesn't have its own per-(state, shout) override.
        std::array<CameraProfile, kShoutableStateCount> shoutsBaseByState      = {};
        std::array<CameraProfile, kShoutableStateCount> shoutsBaseByStateSneak = {};

        // Per-(state, shout) overrides. Indexed by [shoutable-state-idx][shout-idx].
        // Enabled flag lets users keep tuned values in memory while
        // temporarily falling back to the per-state base.
        std::array<std::array<CameraProfile, kShoutCount>, kShoutableStateCount> shoutOverrideByState           = {};
        std::array<std::array<CameraProfile, kShoutCount>, kShoutableStateCount> shoutOverrideByStateSneak      = {};
        std::array<std::array<bool,          kShoutCount>, kShoutableStateCount> shoutOverrideByStateEnabled    = {};
        std::array<std::array<bool,          kShoutCount>, kShoutableStateCount> shoutOverrideByStateEnabledSneak = {};

        // =====================================================================
        // Target Lock (via TDM integration) — parallel profile tree mirroring
        // the non-locked structure above. Selected whenever IsTargetLocked()
        // is true; the same state+sub-state+school+cast/sneak logic that
        // picks a non-locked profile picks the tl* equivalent here. Vanilla
        // TDM supports lock in any state (including horseback, werewolf,
        // vampire lord). Dragon riding requires the TDM Lock-On Extension
        // Patch, which is slated for inclusion in the next TDM release; we
        // provide UI slots so configs survive that upgrade.
        // =====================================================================
        // Verbose Logging (Diagnostics page). OFF ships. The per-frame
        // diagnostics ([MENUCAM], [SNAPXRAY], [ATKTRACE], [STAIR], [TRANS],
        // [DLGX], [ZOOMDIAG], [FOVPULSE], …) log at DEBUG level, so this
        // single switch is what makes them appear — they stay in the code,
        // costing nothing but a level check, and a bug report can ask for
        // them without a new build.
        bool verboseLogging = false;

        // Push verboseLogging to the logger. Called after every settings load
        // and whenever the toggle changes.
        static void ApplyLogLevel(bool a_verbose);

        float targetLockAimBias = 1.0f;

        // Lock-on yaw TRACKING smoothing (seconds) — the "Target Looseness"
        // slider. How tightly the camera holds a target it is ALREADY locked
        // onto: 0 keeps the tight behaviour (the spring runs at a 0.20s
        // duration), higher values add follow lag as the target moves.
        //
        // Named targetLockAcquireSeconds until 2026-08-17, because it used to
        // pace the acquire swing as well — one number doing two jobs, which is
        // why slowing the swing also made combat tracking mushy. The acquire
        // now has its own dial below. The TOML key keeps the historical name
        // `target_lock_acquire_seconds` so existing presets keep loading their
        // looseness value into the field that still means looseness — never
        // reinterpret a shipped key.
        float targetLockTrackSeconds = 0.0f;

        // Target Acquire Speed — the duration (seconds) of the camera's swing
        // onto a NEWLY locked target. Latched at the acquire edge and held
        // through the swing (HookManager's s_camYawAcquire*), so it sets the
        // visible lock-on speed without touching tracking tightness.
        // 0.20 is the value that was hardcoded before this dial existed, so
        // the default preserves the old feel exactly.
        float targetLockAcquireSwingSeconds = 0.20f;   // seconds, 0.06..1.20

        // Target Acquire Speed <-> its 0.00-1.00 slider face. Presented like
        // Target Switch Speed (higher = faster) rather than as raw seconds,
        // which read backwards next to every other speed dial. Piecewise so
        // the three anchors land exactly: 0.00 -> 1.20s (slowest),
        // 0.50 -> 0.20s (the default / the old hardcoded feel),
        // 1.00 -> 0.06s (near-instant).
        static float AcquireSecondsToDisplay(float a_sec) {
            a_sec = std::clamp(a_sec, 0.06f, 1.20f);
            return a_sec >= 0.20f ? (1.20f - a_sec) / 2.0f
                                  : 0.5f + (0.20f - a_sec) / 0.28f;
        }
        static float AcquireDisplayToSeconds(float a_disp) {
            a_disp = std::clamp(a_disp, 0.0f, 1.0f);
            return a_disp <= 0.5f ? 1.20f - a_disp * 2.0f
                                  : 0.20f - (a_disp - 0.5f) * 0.28f;
        }

        // Angular RATE (degrees/second) of the camera+body swing onto a NEWLY
        // SWITCHED target. The switch turns at this constant speed, so the
        // DURATION is set by how far apart the two targets are: a wide swing
        // takes proportionally longer than a small one. Independent of Target
        // Looseness (which is continuous-tracking tightness). See
        // SwitchDurationForAngle + CameraController / HookManager switch swings.
        float targetLockSwitchSpeed = 300.0f;   // deg/sec, 90..720

        // Convert a switch swing angle (radians) into a duration (seconds) at the
        // current Target Switch Speed. Floored so a near-zero swing still resolves
        // in a stable frame or two, and capped so the slowest speed can't stall a
        // wide swing indefinitely. Used by every switch consumer so they all share
        // one distance-driven window.
        float SwitchDurationForAngle(float a_angleRad) const {
            constexpr float kDegToRad = 0.01745329252f;
            const float spd  = targetLockSwitchSpeed < 1.0f ? 1.0f : targetLockSwitchSpeed;
            const float rate = spd * kDegToRad;                 // rad/sec
            const float dur  = std::fabs(a_angleRad) / rate;
            if (dur < 0.08f) return 0.08f;
            if (dur > 1.5f)  return 1.5f;
            return dur;
        }

        // --- Sheathed ---
        CameraProfile tlSheathed       = {};
        CameraProfile tlSheathedSprint = {};
        CameraProfile tlSheathedSwim   = {};
        CameraProfile tlSheathedSneak  = {};

        // --- Melee ---
        CameraProfile tlWeaponsMelee        = {};
        CameraProfile tlWeaponsMeleeSprint  = {};
        CameraProfile tlWeaponsMeleeSwim    = {};
        CameraProfile tlWeaponsMeleeAttack  = {};
        CameraProfile tlWeaponsMeleeSneak   = {};
        CameraProfile tlWeaponsMeleePowerAttack       = {};
        CameraProfile tlWeaponsMeleeSneakAttack       = {};
        CameraProfile tlWeaponsMeleeSneakPowerAttack  = {};
        CameraProfile tlWeaponsMeleeSprintAttack      = {};
        CameraProfile tlWeaponsMeleeSprintPowerAttack = {};

        // Directional power-attack overrides (Target Lock). Base above is
        // the fallback. Index order matches StateResolver::PowerAttackDirection.
        std::array<CameraProfile, kPowerAttackDirectionCount> tlWeaponsMeleePowerAttackDir{};
        std::array<bool, kPowerAttackDirectionCount> tlWeaponsMeleePowerAttackDirEnabled{};
        // Per-weapon-type overrides for each directional TL power-attack profile.
        std::array<MeleeWeaponOverrides, kPowerAttackDirectionCount> tlWeaponsMeleePowerAttackDirOverrides{};

        // Per-weapon-type overrides for the 5 TL Melee slots (parallel to
        // the Categories side). Resolved by ResolveMeleeOverride on the
        // active player weapon; unset slots fall through to the base
        // tlWeaponsMelee* profile above.
        MeleeWeaponOverrides tlWeaponsMeleeOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSprintOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSwimOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeAttackOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSneakOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeShoutOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeShoutSneakOverrides;
        MeleeWeaponOverrides tlWeaponsMeleePowerAttackOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSneakAttackOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSneakPowerAttackOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSprintAttackOverrides;
        MeleeWeaponOverrides tlWeaponsMeleeSprintPowerAttackOverrides;

        // --- Blocking ---
        // Mirrors the 9 Categories block sub-states. See weaponsBlocking* for
        // detection semantics — picker is parallel.
        CameraProfile tlWeaponsBlockingOneHanded      = {};
        CameraProfile tlWeaponsBlockingTwoHanded      = {};
        CameraProfile tlWeaponsBlockingShield         = {};
        CameraProfile tlWeaponsBlockingWard           = {};
        CameraProfile tlWeaponsBlockingOneHandedSneak = {};
        CameraProfile tlWeaponsBlockingTwoHandedSneak = {};
        CameraProfile tlWeaponsBlockingShieldSneak    = {};
        CameraProfile tlWeaponsBlockingWardSneak      = {};
        CameraProfile tlWeaponsBlockingShieldSprint   = {};

        // --- Bow ---
        CameraProfile tlWeaponsBow           = {};
        CameraProfile tlWeaponsBowSprint     = {};
        CameraProfile tlWeaponsBowSwim       = {};
        CameraProfile tlWeaponsBowDraw       = {};
        CameraProfile tlWeaponsBowSneak      = {};
        CameraProfile tlWeaponsBowSneakDraw  = {};
        CameraProfile tlWeaponsBowZoom       = {};
        CameraProfile tlWeaponsBowSneakZoom  = {};

        // --- Crossbow ---
        CameraProfile tlWeaponsCrossbow          = {};
        CameraProfile tlWeaponsCrossbowSprint    = {};
        CameraProfile tlWeaponsCrossbowSwim      = {};
        CameraProfile tlWeaponsCrossbowDraw      = {};
        CameraProfile tlWeaponsCrossbowSneak     = {};
        CameraProfile tlWeaponsCrossbowSneakDraw = {};
        CameraProfile tlWeaponsCrossbowZoom      = {};
        CameraProfile tlWeaponsCrossbowSneakZoom = {};

        // --- Magic ---
        CameraProfile tlWeaponsMagic       = {};
        CameraProfile tlWeaponsMagicSprint = {};
        CameraProfile tlWeaponsMagicSwim   = {};
        CameraProfile tlWeaponsMagicSneak  = {};

        CameraProfile tlMagicAlterationConcentration  = {};
        CameraProfile tlMagicAlterationFireAndForget  = {};
        CameraProfile tlMagicAlterationRitual         = {};
        CameraProfile tlMagicConjurationConcentration = {};
        CameraProfile tlMagicConjurationFireAndForget = {};
        CameraProfile tlMagicConjurationRitual        = {};
        CameraProfile tlMagicDestructionConcentration = {};
        CameraProfile tlMagicDestructionFireAndForget = {};
        CameraProfile tlMagicDestructionRitual        = {};
        CameraProfile tlMagicIllusionConcentration    = {};
        CameraProfile tlMagicIllusionFireAndForget    = {};
        CameraProfile tlMagicIllusionRitual           = {};
        CameraProfile tlMagicRestorationConcentration = {};
        CameraProfile tlMagicRestorationFireAndForget = {};
        CameraProfile tlMagicRestorationRitual        = {};

        CameraProfile tlMagicAlterationConcentrationSneak  = {};
        CameraProfile tlMagicAlterationFireAndForgetSneak  = {};
        CameraProfile tlMagicAlterationRitualSneak         = {};
        CameraProfile tlMagicConjurationConcentrationSneak = {};
        CameraProfile tlMagicConjurationFireAndForgetSneak = {};
        CameraProfile tlMagicConjurationRitualSneak        = {};
        CameraProfile tlMagicDestructionConcentrationSneak = {};
        CameraProfile tlMagicDestructionFireAndForgetSneak = {};
        CameraProfile tlMagicDestructionRitualSneak        = {};
        CameraProfile tlMagicIllusionConcentrationSneak    = {};
        CameraProfile tlMagicIllusionFireAndForgetSneak    = {};
        CameraProfile tlMagicIllusionRitualSneak           = {};
        CameraProfile tlMagicRestorationConcentrationSneak = {};
        CameraProfile tlMagicRestorationFireAndForgetSneak = {};
        CameraProfile tlMagicRestorationRitualSneak        = {};

        // --- Staves ---
        CameraProfile tlWeaponsStaves       = {};
        CameraProfile tlWeaponsStavesSprint = {};
        CameraProfile tlWeaponsStavesSwim   = {};
        CameraProfile tlWeaponsStavesSneak  = {};

        CameraProfile tlStavesAlterationConcentration  = {};
        CameraProfile tlStavesAlterationFireAndForget  = {};
        CameraProfile tlStavesAlterationRitual  = {};
        CameraProfile tlStavesConjurationConcentration = {};
        CameraProfile tlStavesConjurationFireAndForget = {};
        CameraProfile tlStavesConjurationRitual = {};
        CameraProfile tlStavesDestructionConcentration = {};
        CameraProfile tlStavesDestructionFireAndForget = {};
        CameraProfile tlStavesDestructionRitual = {};
        CameraProfile tlStavesIllusionConcentration    = {};
        CameraProfile tlStavesIllusionFireAndForget    = {};
        CameraProfile tlStavesIllusionRitual    = {};
        CameraProfile tlStavesRestorationConcentration = {};
        CameraProfile tlStavesRestorationFireAndForget = {};
        CameraProfile tlStavesRestorationRitual = {};

        CameraProfile tlStavesAlterationConcentrationSneak  = {};
        CameraProfile tlStavesAlterationFireAndForgetSneak  = {};
        CameraProfile tlStavesAlterationRitualSneak  = {};
        CameraProfile tlStavesConjurationConcentrationSneak = {};
        CameraProfile tlStavesConjurationFireAndForgetSneak = {};
        CameraProfile tlStavesConjurationRitualSneak = {};
        CameraProfile tlStavesDestructionConcentrationSneak = {};
        CameraProfile tlStavesDestructionFireAndForgetSneak = {};
        CameraProfile tlStavesDestructionRitualSneak = {};
        CameraProfile tlStavesIllusionConcentrationSneak    = {};
        CameraProfile tlStavesIllusionFireAndForgetSneak    = {};
        CameraProfile tlStavesIllusionRitualSneak    = {};
        CameraProfile tlStavesRestorationConcentrationSneak = {};
        CameraProfile tlStavesRestorationFireAndForgetSneak = {};
        CameraProfile tlStavesRestorationRitualSneak = {};

        // --- Transformations ---
        CameraProfile tlTransformationsWerewolf       = {};
        CameraProfile tlTransformationsWerewolfSheathed = {};
        CameraProfile tlTransformationsWerewolfSprint = {};
        CameraProfile tlTransformationsWerewolfSwim   = {};
        CameraProfile tlTransformationsWerewolfAttack = {};
        CameraProfile tlTransformationsWerewolfPowerAttack       = {};
        CameraProfile tlTransformationsWerewolfSprintPowerAttack = {};
        CameraProfile tlTransformationsWerewolfRoar   = {};
        CameraProfile tlTransformationsWerewolfFeeding = {};

        CameraProfile tlVampireLordSheathed             = {};
        CameraProfile tlVampireLordSheathedLevitating   = {};
        CameraProfile tlVampireLordMelee                = {};
        CameraProfile tlVampireLordMeleeAttack          = {};
        CameraProfile tlVampireLordMeleePowerAttack     = {};
        CameraProfile tlVampireLordMagic                = {};
        CameraProfile tlVampireLordConcentration        = {};
        CameraProfile tlVampireLordFireAndForget        = {};
        CameraProfile tlVampireLordSprint               = {};
        CameraProfile tlVampireLordSprintLevitating     = {};

        // --- Mounts ---
        CameraProfile tlMountsHorseback         = {};
        CameraProfile tlMountsHorsebackSprint   = {};
        CameraProfile tlMountsHorsebackSwim     = {};
        CameraProfile tlMountsHorsebackMelee    = {};
        CameraProfile tlMountsHorsebackArchery  = {};
        CameraProfile tlMountsHorsebackArcheryDraw = {};
        CameraProfile tlMountsHorsebackArcheryZoom = {};
        CameraProfile tlMountsHorsebackMeleeLeft   = {};
        CameraProfile tlMountsHorsebackMeleeRight  = {};
        // NO dragon TL slot. TDM supports target lock in any on-foot,
        // horseback or transformed state, but dragon riding needs the Lock-On
        // Extension Patch (see the note beside the target_lock reads in
        // Load) — so a dragon TL profile can never resolve on a normal
        // install, which makes it exactly the dead storage
        // mountsHorsebackMelee already is. Removed 2026-08-23; the Categories
        // dragon profiles apply whether or not a lock is somehow held.

        // --- Shouts ---
        // Parallel to shoutsBaseByState / shoutOverrideByState. Same 6 shoutable
        // states, same 27-shout vocabulary, same enable-flag pattern.
        std::array<CameraProfile, kShoutableStateCount> tlShoutsBaseByState      = {};
        std::array<CameraProfile, kShoutableStateCount> tlShoutsBaseByStateSneak = {};
        std::array<std::array<CameraProfile, kShoutCount>, kShoutableStateCount> tlShoutOverrideByState           = {};
        std::array<std::array<CameraProfile, kShoutCount>, kShoutableStateCount> tlShoutOverrideByStateSneak      = {};
        std::array<std::array<bool,          kShoutCount>, kShoutableStateCount> tlShoutOverrideByStateEnabled    = {};
        std::array<std::array<bool,          kShoutCount>, kShoutableStateCount> tlShoutOverrideByStateEnabledSneak = {};

        // =====================================================================
        // Enemy Overrides — per-field splice on top of the picked TL profile
        // when the locked target classifies as one of the five tracked enemy
        // categories (Dragons, Giants, Mammoths, Centurions, Lurkers — see
        // EnemyDetector). Each (enemy, TL slot) holds 6 floats and 6 enable
        // bools; only the toggled-on fields override the base TL value.
        //
        // The single source of truth for the slot vocabulary is the X-macro
        // below. It also generates the TOML key array and the "given a
        // TLSlot, return its CameraProfile" lookup, all in lockstep.
        //
        // Per-enemy aim bias overrides the global `targetLockAimBias` while
        // locked onto that enemy type, when its enable flag is set.
        //
        // EnemyFieldOverride itself is defined above (ahead of WeaponBinding,
        // which embeds a per-binding set).
        // =====================================================================

#define DDC_TL_SLOT_LIST(X)                                                                           \
    /* Sheathed */                                                                                    \
    X(Sheathed,                            "sheathed",                                tlSheathed)                            \
    X(SheathedSprint,                      "sheathed.sprint",                         tlSheathedSprint)                      \
    X(SheathedSwim,                        "sheathed.swim",                           tlSheathedSwim)                        \
    X(SheathedSneak,                       "sheathed.sneak",                          tlSheathedSneak)                       \
    /* Melee */                                                                                       \
    X(WeaponsMelee,                        "weapons.melee",                           tlWeaponsMelee)                        \
    X(WeaponsMeleeSprint,                  "weapons.melee.sprint",                    tlWeaponsMeleeSprint)                  \
    X(WeaponsMeleeSwim,                    "weapons.melee.swim",                      tlWeaponsMeleeSwim)                    \
    X(WeaponsMeleeAttack,                  "weapons.melee.attack",                    tlWeaponsMeleeAttack)                  \
    X(WeaponsMeleeSneak,                   "weapons.melee.sneak",                     tlWeaponsMeleeSneak)                   \
    X(WeaponsMeleePowerAttack,             "weapons.melee.power_attack",              tlWeaponsMeleePowerAttack)             \
    X(WeaponsMeleeSneakAttack,             "weapons.melee.sneak_attack",              tlWeaponsMeleeSneakAttack)             \
    X(WeaponsMeleeSneakPowerAttack,        "weapons.melee.sneak_power_attack",        tlWeaponsMeleeSneakPowerAttack)        \
    X(WeaponsMeleeSprintAttack,            "weapons.melee.sprint_attack",             tlWeaponsMeleeSprintAttack)            \
    X(WeaponsMeleeSprintPowerAttack,       "weapons.melee.sprint_power_attack",       tlWeaponsMeleeSprintPowerAttack)       \
    /* Blocking — 9 sub-states based on equipment and pose. */                                       \
    X(WeaponsBlockingOneHanded,            "weapons.blocking.one_handed",             tlWeaponsBlockingOneHanded)            \
    X(WeaponsBlockingTwoHanded,            "weapons.blocking.two_handed",             tlWeaponsBlockingTwoHanded)            \
    X(WeaponsBlockingShield,               "weapons.blocking.shield",                 tlWeaponsBlockingShield)               \
    X(WeaponsBlockingWard,                 "weapons.blocking.ward",                   tlWeaponsBlockingWard)                 \
    X(WeaponsBlockingOneHandedSneak,       "weapons.blocking.one_handed.sneak",       tlWeaponsBlockingOneHandedSneak)       \
    X(WeaponsBlockingTwoHandedSneak,       "weapons.blocking.two_handed.sneak",       tlWeaponsBlockingTwoHandedSneak)       \
    X(WeaponsBlockingShieldSneak,          "weapons.blocking.shield.sneak",           tlWeaponsBlockingShieldSneak)          \
    X(WeaponsBlockingWardSneak,            "weapons.blocking.ward.sneak",             tlWeaponsBlockingWardSneak)            \
    X(WeaponsBlockingShieldSprint,         "weapons.blocking.shield.sprint",          tlWeaponsBlockingShieldSprint)         \
    /* Bow */                                                                                         \
    X(WeaponsBow,                          "weapons.bow",                             tlWeaponsBow)                          \
    X(WeaponsBowSprint,                    "weapons.bow.sprint",                      tlWeaponsBowSprint)                    \
    X(WeaponsBowSwim,                      "weapons.bow.swim",                        tlWeaponsBowSwim)                      \
    X(WeaponsBowDraw,                      "weapons.bow.draw",                        tlWeaponsBowDraw)                      \
    X(WeaponsBowSneak,                     "weapons.bow.sneak",                       tlWeaponsBowSneak)                     \
    X(WeaponsBowSneakDraw,                 "weapons.bow.sneak.draw",                  tlWeaponsBowSneakDraw)                 \
    X(WeaponsBowZoom,                      "weapons.bow.zoom",                        tlWeaponsBowZoom)                      \
    X(WeaponsBowSneakZoom,                 "weapons.bow.sneak.zoom",                  tlWeaponsBowSneakZoom)                 \
    /* Crossbow */                                                                                    \
    X(WeaponsCrossbow,                     "weapons.crossbow",                        tlWeaponsCrossbow)                     \
    X(WeaponsCrossbowSprint,               "weapons.crossbow.sprint",                 tlWeaponsCrossbowSprint)               \
    X(WeaponsCrossbowSwim,                 "weapons.crossbow.swim",                   tlWeaponsCrossbowSwim)                 \
    X(WeaponsCrossbowDraw,                 "weapons.crossbow.draw",                   tlWeaponsCrossbowDraw)                 \
    X(WeaponsCrossbowSneak,                "weapons.crossbow.sneak",                  tlWeaponsCrossbowSneak)                \
    X(WeaponsCrossbowSneakDraw,            "weapons.crossbow.sneak.draw",             tlWeaponsCrossbowSneakDraw)            \
    X(WeaponsCrossbowZoom,                 "weapons.crossbow.zoom",                   tlWeaponsCrossbowZoom)                 \
    X(WeaponsCrossbowSneakZoom,            "weapons.crossbow.sneak.zoom",             tlWeaponsCrossbowSneakZoom)            \
    /* Magic — base */                                                                                \
    X(WeaponsMagic,                        "weapons.magic",                           tlWeaponsMagic)                        \
    X(WeaponsMagicSprint,                  "weapons.magic.sprint",                    tlWeaponsMagicSprint)                  \
    X(WeaponsMagicSwim,                    "weapons.magic.swim",                      tlWeaponsMagicSwim)                    \
    X(WeaponsMagicSneak,                   "weapons.magic.sneak",                     tlWeaponsMagicSneak)                   \
    /* Magic — schools × cast types */                                                                \
    X(MagicAlterationConcentration,        "magic.alteration.concentration",          tlMagicAlterationConcentration)        \
    X(MagicAlterationFireAndForget,        "magic.alteration.fire_and_forget",        tlMagicAlterationFireAndForget)        \
    X(MagicAlterationRitual,               "magic.alteration.ritual",                 tlMagicAlterationRitual)               \
    X(MagicConjurationConcentration,       "magic.conjuration.concentration",         tlMagicConjurationConcentration)       \
    X(MagicConjurationFireAndForget,       "magic.conjuration.fire_and_forget",       tlMagicConjurationFireAndForget)       \
    X(MagicConjurationRitual,              "magic.conjuration.ritual",                tlMagicConjurationRitual)              \
    X(MagicDestructionConcentration,       "magic.destruction.concentration",         tlMagicDestructionConcentration)       \
    X(MagicDestructionFireAndForget,       "magic.destruction.fire_and_forget",       tlMagicDestructionFireAndForget)       \
    X(MagicDestructionRitual,              "magic.destruction.ritual",                tlMagicDestructionRitual)              \
    X(MagicIllusionConcentration,          "magic.illusion.concentration",            tlMagicIllusionConcentration)          \
    X(MagicIllusionFireAndForget,          "magic.illusion.fire_and_forget",          tlMagicIllusionFireAndForget)          \
    X(MagicIllusionRitual,                 "magic.illusion.ritual",                   tlMagicIllusionRitual)                 \
    X(MagicRestorationConcentration,       "magic.restoration.concentration",         tlMagicRestorationConcentration)       \
    X(MagicRestorationFireAndForget,       "magic.restoration.fire_and_forget",       tlMagicRestorationFireAndForget)       \
    X(MagicRestorationRitual,              "magic.restoration.ritual",                tlMagicRestorationRitual)              \
    /* Magic — sneak variants */                                                                      \
    X(MagicAlterationConcentrationSneak,   "magic.alteration.sneak.concentration",    tlMagicAlterationConcentrationSneak)   \
    X(MagicAlterationFireAndForgetSneak,   "magic.alteration.sneak.fire_and_forget",  tlMagicAlterationFireAndForgetSneak)   \
    X(MagicAlterationRitualSneak,          "magic.alteration.sneak.ritual",           tlMagicAlterationRitualSneak)          \
    X(MagicConjurationConcentrationSneak,  "magic.conjuration.sneak.concentration",   tlMagicConjurationConcentrationSneak)  \
    X(MagicConjurationFireAndForgetSneak,  "magic.conjuration.sneak.fire_and_forget", tlMagicConjurationFireAndForgetSneak)  \
    X(MagicConjurationRitualSneak,         "magic.conjuration.sneak.ritual",          tlMagicConjurationRitualSneak)         \
    X(MagicDestructionConcentrationSneak,  "magic.destruction.sneak.concentration",   tlMagicDestructionConcentrationSneak)  \
    X(MagicDestructionFireAndForgetSneak,  "magic.destruction.sneak.fire_and_forget", tlMagicDestructionFireAndForgetSneak)  \
    X(MagicDestructionRitualSneak,         "magic.destruction.sneak.ritual",          tlMagicDestructionRitualSneak)         \
    X(MagicIllusionConcentrationSneak,     "magic.illusion.sneak.concentration",      tlMagicIllusionConcentrationSneak)     \
    X(MagicIllusionFireAndForgetSneak,     "magic.illusion.sneak.fire_and_forget",    tlMagicIllusionFireAndForgetSneak)     \
    X(MagicIllusionRitualSneak,            "magic.illusion.sneak.ritual",             tlMagicIllusionRitualSneak)            \
    X(MagicRestorationConcentrationSneak,  "magic.restoration.sneak.concentration",   tlMagicRestorationConcentrationSneak)  \
    X(MagicRestorationFireAndForgetSneak,  "magic.restoration.sneak.fire_and_forget", tlMagicRestorationFireAndForgetSneak)  \
    X(MagicRestorationRitualSneak,         "magic.restoration.sneak.ritual",          tlMagicRestorationRitualSneak)         \
    /* Staves — base */                                                                               \
    X(WeaponsStaves,                       "weapons.staves",                          tlWeaponsStaves)                       \
    X(WeaponsStavesSprint,                 "weapons.staves.sprint",                   tlWeaponsStavesSprint)                 \
    X(WeaponsStavesSwim,                   "weapons.staves.swim",                     tlWeaponsStavesSwim)                   \
    X(WeaponsStavesSneak,                  "weapons.staves.sneak",                    tlWeaponsStavesSneak)                  \
    /* Staves — schools × cast types */                                                               \
    X(StavesAlterationConcentration,       "staves.alteration.concentration",         tlStavesAlterationConcentration)       \
    X(StavesAlterationFireAndForget,       "staves.alteration.fire_and_forget",       tlStavesAlterationFireAndForget)       \
    X(StavesConjurationConcentration,      "staves.conjuration.concentration",        tlStavesConjurationConcentration)      \
    X(StavesConjurationFireAndForget,      "staves.conjuration.fire_and_forget",      tlStavesConjurationFireAndForget)      \
    X(StavesDestructionConcentration,      "staves.destruction.concentration",        tlStavesDestructionConcentration)      \
    X(StavesDestructionFireAndForget,      "staves.destruction.fire_and_forget",      tlStavesDestructionFireAndForget)      \
    X(StavesIllusionConcentration,         "staves.illusion.concentration",           tlStavesIllusionConcentration)         \
    X(StavesIllusionFireAndForget,         "staves.illusion.fire_and_forget",         tlStavesIllusionFireAndForget)         \
    X(StavesRestorationConcentration,      "staves.restoration.concentration",        tlStavesRestorationConcentration)      \
    X(StavesRestorationFireAndForget,      "staves.restoration.fire_and_forget",      tlStavesRestorationFireAndForget)      \
    /* Staves — sneak variants */                                                                     \
    X(StavesAlterationConcentrationSneak,  "staves.alteration.sneak.concentration",   tlStavesAlterationConcentrationSneak)  \
    X(StavesAlterationFireAndForgetSneak,  "staves.alteration.sneak.fire_and_forget", tlStavesAlterationFireAndForgetSneak)  \
    X(StavesConjurationConcentrationSneak, "staves.conjuration.sneak.concentration",  tlStavesConjurationConcentrationSneak) \
    X(StavesConjurationFireAndForgetSneak, "staves.conjuration.sneak.fire_and_forget",tlStavesConjurationFireAndForgetSneak) \
    X(StavesDestructionConcentrationSneak, "staves.destruction.sneak.concentration",  tlStavesDestructionConcentrationSneak) \
    X(StavesDestructionFireAndForgetSneak, "staves.destruction.sneak.fire_and_forget",tlStavesDestructionFireAndForgetSneak) \
    X(StavesIllusionConcentrationSneak,    "staves.illusion.sneak.concentration",     tlStavesIllusionConcentrationSneak)    \
    X(StavesIllusionFireAndForgetSneak,    "staves.illusion.sneak.fire_and_forget",   tlStavesIllusionFireAndForgetSneak)    \
    X(StavesRestorationConcentrationSneak, "staves.restoration.sneak.concentration",  tlStavesRestorationConcentrationSneak) \
    X(StavesRestorationFireAndForgetSneak, "staves.restoration.sneak.fire_and_forget",tlStavesRestorationFireAndForgetSneak) \
    /* Transformations */                                                                             \
    X(TransformationsWerewolf,             "transformations.werewolf",                tlTransformationsWerewolf)             \
    X(TransformationsWerewolfSheathed,     "transformations.werewolf.sheathed",       tlTransformationsWerewolfSheathed)     \
    X(TransformationsWerewolfSprint,       "transformations.werewolf.sprint",         tlTransformationsWerewolfSprint)       \
    X(TransformationsWerewolfSwim,         "transformations.werewolf.swim",           tlTransformationsWerewolfSwim)         \
    X(TransformationsWerewolfAttack,       "transformations.werewolf.attack",         tlTransformationsWerewolfAttack)       \
    X(TransformationsWerewolfPowerAttack,  "transformations.werewolf.power_attack",   tlTransformationsWerewolfPowerAttack)  \
    X(TransformationsWerewolfSprintPowerAttack,"transformations.werewolf.sprint_power_attack",tlTransformationsWerewolfSprintPowerAttack)\
    X(TransformationsWerewolfRoar,         "transformations.werewolf.roar",           tlTransformationsWerewolfRoar)         \
    X(TransformationsWerewolfFeeding,      "transformations.werewolf.feeding",        tlTransformationsWerewolfFeeding)      \
    X(VampireLordSheathed,                 "transformations.vampire_lord.sheathed",   tlVampireLordSheathed)                 \
    X(VampireLordSheathedLevitating,       "transformations.vampire_lord.sheathed.levitating", tlVampireLordSheathedLevitating) \
    X(VampireLordSprint,                   "transformations.vampire_lord.sprint",     tlVampireLordSprint)                   \
    X(VampireLordSprintLevitating,         "transformations.vampire_lord.sprint.levitating", tlVampireLordSprintLevitating)  \
    X(VampireLordMagic,                    "transformations.vampire_lord.magic",      tlVampireLordMagic)                    \
    X(VampireLordConcentration,            "transformations.vampire_lord.concentration", tlVampireLordConcentration)         \
    X(VampireLordFireAndForget,            "transformations.vampire_lord.fire_and_forget", tlVampireLordFireAndForget)       \
    X(VampireLordMelee,                    "transformations.vampire_lord.melee",      tlVampireLordMelee)                    \
    X(VampireLordMeleeAttack,              "transformations.vampire_lord.melee.attack", tlVampireLordMeleeAttack)            \
    X(VampireLordMeleePowerAttack,         "transformations.vampire_lord.melee.power_attack", tlVampireLordMeleePowerAttack) \
    /* Mounts */                                                                                      \
    X(MountsHorseback,                     "mounts.horseback",                        tlMountsHorseback)                     \
    X(MountsHorsebackSprint,               "mounts.horseback.sprint",                 tlMountsHorsebackSprint)               \
    X(MountsHorsebackSwim,                 "mounts.horseback.swim",                   tlMountsHorsebackSwim)                 \
    X(MountsHorsebackMelee,                "mounts.horseback.melee",                  tlMountsHorsebackMelee)                \
    X(MountsHorsebackArchery,              "mounts.horseback.archery",                tlMountsHorsebackArchery)              \
    X(MountsHorsebackArcheryZoom,          "mounts.horseback.archery.zoom",           tlMountsHorsebackArcheryZoom)          \
    X(MountsHorsebackMeleeLeft,            "mounts.horseback.melee.attack_left",      tlMountsHorsebackMeleeLeft)            \
    X(MountsHorsebackMeleeRight,           "mounts.horseback.melee.attack_right",     tlMountsHorsebackMeleeRight)           \
    /* Dragon riding has NO TL slot — TDM cannot lock while riding one. */ \
    /* Shouts — per-state base only (per the design doc, full per-shout scope was scoped out). */    \
    X(ShoutsBaseSheathed,                  "shouts.base.sheathed",                    tlShoutsBaseByState[0])                \
    X(ShoutsBaseMelee,                     "shouts.base.melee",                       tlShoutsBaseByState[1])                \
    X(ShoutsBaseBow,                       "shouts.base.bow",                         tlShoutsBaseByState[2])                \
    X(ShoutsBaseCrossbow,                  "shouts.base.crossbow",                    tlShoutsBaseByState[3])                \
    X(ShoutsBaseMagic,                     "shouts.base.magic",                       tlShoutsBaseByState[4])                \
    X(ShoutsBaseStaves,                    "shouts.base.staves",                      tlShoutsBaseByState[5])                \
    X(ShoutsBaseSheathedSneak,             "shouts.base.sheathed.sneak",              tlShoutsBaseByStateSneak[0])           \
    X(ShoutsBaseMeleeSneak,                "shouts.base.melee.sneak",                 tlShoutsBaseByStateSneak[1])           \
    X(ShoutsBaseBowSneak,                  "shouts.base.bow.sneak",                   tlShoutsBaseByStateSneak[2])           \
    X(ShoutsBaseCrossbowSneak,             "shouts.base.crossbow.sneak",              tlShoutsBaseByStateSneak[3])           \
    X(ShoutsBaseMagicSneak,                "shouts.base.magic.sneak",                 tlShoutsBaseByStateSneak[4])           \
    X(ShoutsBaseStavesSneak,               "shouts.base.staves.sneak",                tlShoutsBaseByStateSneak[5])           \
    /* Append new slots so existing slot identities remain stable. */                                     \
    X(MountsHorsebackArcheryDraw,          "mounts.horseback.archery.draw",           tlMountsHorsebackArcheryDraw) \
    X(StavesAlterationRitual,       "staves.alteration.ritual",       tlStavesAlterationRitual)       \
    X(StavesConjurationRitual,      "staves.conjuration.ritual",      tlStavesConjurationRitual)      \
    X(StavesDestructionRitual,      "staves.destruction.ritual",      tlStavesDestructionRitual)      \
    X(StavesIllusionRitual,         "staves.illusion.ritual",         tlStavesIllusionRitual)         \
    X(StavesRestorationRitual,      "staves.restoration.ritual",      tlStavesRestorationRitual)      \
    X(StavesAlterationRitualSneak,  "staves.alteration.sneak.ritual", tlStavesAlterationRitualSneak)  \
    X(StavesConjurationRitualSneak, "staves.conjuration.sneak.ritual",tlStavesConjurationRitualSneak) \
    X(StavesDestructionRitualSneak, "staves.destruction.sneak.ritual",tlStavesDestructionRitualSneak) \
    X(StavesIllusionRitualSneak,    "staves.illusion.sneak.ritual",   tlStavesIllusionRitualSneak)    \
    X(StavesRestorationRitualSneak, "staves.restoration.sneak.ritual",tlStavesRestorationRitualSneak)

        enum class TLSlot : std::uint16_t {
#define DDC_X(name, key, field) name,
            DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
            Count
        };
        static constexpr std::size_t kTLSlotCount = static_cast<std::size_t>(TLSlot::Count);

        // 5 enemy types × kTLSlotCount (129) TL slots. ~20KB total — fine flat.
        // (kEnemyOverrideEnemies is defined above, ahead of WeaponBinding.)
        std::array<std::array<EnemyFieldOverride, kTLSlotCount>, kEnemyOverrideEnemies> enemyOverrides = {};
        // Indoor twin (2026-09-07). Enemy overrides are entry- AND env-specific
        // now: the Enemy Override popup edits whichever variant the Target Lock
        // page's Outdoor/Indoor toggle has selected, and shows nothing from the
        // other one — the same contract Location Overrides have always had.
        // Defaults to vanilla rather than a copy of outdoor, matching the
        // custom-enemy twins.
        std::array<std::array<EnemyFieldOverride, kTLSlotCount>, kEnemyOverrideEnemies>
            enemyOverridesIndoor = {};
        [[nodiscard]] std::array<std::array<EnemyFieldOverride, kTLSlotCount>, kEnemyOverrideEnemies>&
        EnemyOverridesFor(int e)
        {
            return e == kEnvIndoor ? enemyOverridesIndoor : enemyOverrides;
        }
        [[nodiscard]] const std::array<std::array<EnemyFieldOverride, kTLSlotCount>, kEnemyOverrideEnemies>&
        EnemyOverridesFor(int e) const
        {
            return e == kEnvIndoor ? enemyOverridesIndoor : enemyOverrides;
        }

        // Per-(enemy, direction) field overrides for directional power
        // attacks (target lock only). Directional PA profiles live outside
        // the TLSlot table, so they carry their own enemy-override storage.
        std::array<std::array<EnemyFieldOverride, kPowerAttackDirectionCount>, kEnemyOverrideEnemies>
            tlEnemyOverridesPowerAttackDir = {};
        std::array<std::array<EnemyFieldOverride, kPowerAttackDirectionCount>, kEnemyOverrideEnemies>
            tlEnemyOverridesPowerAttackDirIndoor = {};
        [[nodiscard]] std::array<std::array<EnemyFieldOverride, kPowerAttackDirectionCount>, kEnemyOverrideEnemies>&
        TlEnemyOverridesPaDirFor(int e)
        {
            return e == kEnvIndoor ? tlEnemyOverridesPowerAttackDirIndoor
                                   : tlEnemyOverridesPowerAttackDir;
        }
        [[nodiscard]] const std::array<std::array<EnemyFieldOverride, kPowerAttackDirectionCount>, kEnemyOverrideEnemies>&
        TlEnemyOverridesPaDirFor(int e) const
        {
            return e == kEnvIndoor ? tlEnemyOverridesPowerAttackDirIndoor
                                   : tlEnemyOverridesPowerAttackDir;
        }

        // (Aim bias is per-ENTRY and per-enemy both, and lives on
        // CameraProfile::transitionAimBias since 2026-09-07 — edited from the
        // Transition Override popup. The old per-enemy global arrays, and then
        // EnemyFieldOverride's own float, were removed in turn.)

        // ── Custom (player-bound) enemy overrides ────────────────────────
        // Beyond the 5 built-in creature categories, the player can bind the
        // currently locked-on target so ANY enemy gets its own profile. Each
        // bind matches by RACE or by specific NPC base (chosen per bind),
        // identified load-order-safely by local form id + plugin filename
        // (same scheme as WeaponBinding). Each carries the full per-slot +
        // per-PA-direction override grid, mirroring one built-in enemy column,
        // so a custom enemy is editable exactly like the fixed ones. Sparse
        // TOML writes keep storage compact. Custom binds take priority over
        // the 5 categories (NPC match beats Race match).
        struct CustomEnemyOverride {
            // Match granularity, broad -> specific. Name matches every actor with
            // the exact display name (a generic "type" like Draugr Scourge or
            // Bandit Outlaw); NPC matches one unique base (Alduin, Mercer Frey).
            // Keyword/RaceFamily/Faction/Race are legacy (kept for save-compat and
            // resolution; the UI only creates Name / NPC binds now). At resolve
            // time the most specific match wins
            // (NPC > Name > Race > Faction > RaceFamily > Keyword).
            enum class MatchType : std::uint8_t { Keyword = 0, RaceFamily = 1, Race = 2, NPC = 3, Faction = 4, Name = 5 };
            MatchType     matchType = MatchType::Race;
            std::uint32_t formID    = 0;   // LOCAL form id (Race / NPC / Faction)
            std::string   pluginName;      // source plugin filename (Race / NPC / Faction)
            // Keyword: the creature-type keyword editor id (e.g. "ActorTypeUndead").
            // RaceFamily: a case-insensitive substring of the race editor id
            // (e.g. "Draugr", "DwarvenSphere"). Empty for Race / NPC / Faction.
            std::string   matchKey;
            std::string   displayName;     // shown in the Custom-tab dropdown
            // Outdoor field grid (the default, used outdoors).
            std::array<EnemyFieldOverride, kTLSlotCount>               slots{};
            std::array<EnemyFieldOverride, kPowerAttackDirectionCount> paDir{};
            // Indoor twins — used when the player is in an interior cell
            // (settings.indoorMode). Custom enemy overrides are the ONE env-split
            // Target-Lock storage (the base TL slots are env-agnostic); a custom
            // enemy can therefore be framed differently indoors vs outdoors, like
            // the base camera profiles. Default to vanilla (NOT a copy of outdoor).
            std::array<EnemyFieldOverride, kTLSlotCount>               slotsIndoor{};
            std::array<EnemyFieldOverride, kPowerAttackDirectionCount> paDirIndoor{};
            [[nodiscard]] std::array<EnemyFieldOverride, kTLSlotCount>& SlotsFor(int e)
            {
                return e == kEnvIndoor ? slotsIndoor : slots;
            }
            [[nodiscard]] const std::array<EnemyFieldOverride, kTLSlotCount>& SlotsFor(int e) const
            {
                return e == kEnvIndoor ? slotsIndoor : slots;
            }
            [[nodiscard]] std::array<EnemyFieldOverride, kPowerAttackDirectionCount>& PaDirFor(int e)
            {
                return e == kEnvIndoor ? paDirIndoor : paDir;
            }
            [[nodiscard]] const std::array<EnemyFieldOverride, kPowerAttackDirectionCount>& PaDirFor(int e) const
            {
                return e == kEnvIndoor ? paDirIndoor : paDir;
            }
            // Lock-on tracking smoothing (0 = off .. 3 = max, LP tau up to
            // ~1.24 s). When > 0, DDC drives a low-passed bearing while locked
            // onto this enemy so its jerky animation (ice wraiths, horkers)
            // doesn't whip the camera. Not env-split — it characterises the
            // enemy's motion, not the framing.
            float trackingSmoothing = 0.0f;

        };
        std::vector<CustomEnemyOverride> customEnemyOverrides;

        // Returns the index into customEnemyOverrides whose bind matches the
        // locked actor (NPC match preferred over Race), or -1 if none. Pure
        // local-formID + plugin comparison against the actor's race / base.
        [[nodiscard]] int FindCustomEnemyForActor(RE::Actor* actor) const;

        // Stable identity key for a custom enemy bind — used to key the
        // per-weapon-binding custom-enemy grids (indices into
        // customEnemyOverrides shift on Remove, so they can't be stored).
        [[nodiscard]] static std::string CustomEnemyIdentityKey(const CustomEnemyOverride& c);

        // Resolve the EnemyFieldOverride for a custom enemy at the picked TL
        // slot / directional-PA profile (mirrors ResolveEnemyOverride).
        // When an active weapon binding is passed, the override comes ONLY
        // from that binding's own per-custom-enemy grid (a fresh bind starts
        // with none — it does not inherit the category's slot-keyed cells).
        // Returns nullptr when the slot doesn't resolve.
        [[nodiscard]] const EnemyFieldOverride* ResolveCustomEnemyOverride(
            int customIdx, const CameraProfile* resolved, std::optional<TLSlot> slot,
            const WeaponBinding* binding = nullptr, int bindingSlot = -1) const;

        // Splice a custom enemy's enabled camera fields onto dst (parallels
        // ApplyEnemyOverrideResolved). No-op if the override is absent/off.
        void ApplyCustomEnemyOverrideResolved(
            CameraProfile& dst, int customIdx, const CameraProfile* resolved,
            std::optional<TLSlot> slot,
            const WeaponBinding* binding = nullptr, int bindingSlot = -1) const;

        // Maps a TLSlot back to its CameraProfile field. Defined out-of-class
        // because the X-list expansion needs the full SettingsManager body.
        [[nodiscard]] CameraProfile& GetTLProfileBySlot(TLSlot s);

        // Reverse lookup: given a CameraProfile pointer that the TL picker
        // produced, return its TLSlot. Used to thread "which slot was
        // picked" from the picker into the enemy-override splice without
        // having to track a parallel slot variable in every switch arm.
        [[nodiscard]] std::optional<TLSlot> SlotFromTLProfile(CameraProfile* p);
        // Inverse of the above.
        [[nodiscard]] CameraProfile* TLProfileFromSlot(TLSlot a_slot);

        // The TL slot a bound weapon's sub-state STANDS IN FOR — i.e. the slot
        // the picker would have landed on had the binding not won. A binding's
        // own profiles live outside the TLSlot table, so without this mapping
        // anything keyed by slot (the player-bound custom enemy overrides) is
        // simply unreachable whenever a bound weapon is equipped.
        //
        // Takes the whole binding, not just the category, because a bound
        // SPELL's Casting sub-state resolves to a school x cast-type slot and
        // the spell — hence the school — is known from the bind itself.
        //
        // Returns nullopt for sub-states with no fixed slot: the melee
        // directional power attacks (they have dedicated storage, addressed by
        // direction index instead) and shout bindings (whose slot depends on
        // the state the player shouts FROM, which isn't knowable here).
        [[nodiscard]] static std::optional<TLSlot> TLSlotForBinding(const WeaponBinding& a_binding,
                                                                    int a_subStateIdx);

        // Location-override key for a bound weapon's FIRST PERSON sub-state —
        // "binding.<plugin>|<formid>.s<slot>", namespaced so it can never
        // collide with the per-state FP keys. Both the Specific Weapons FP
        // editor's Location button and the FP binding resolver use it, so
        // what is edited there is what the camera reads (2026-08-15).
        [[nodiscard]] static std::string FpBindingLocationKey(const WeaponBinding& a_binding,
                                                              int a_subStateIdx);

        // Location-override key for a bound weapon's THIRD-PERSON camera
        // sub-state — FpBindingLocationKey plus a "|cat" / "|tl" section
        // suffix. Editor (Specific Weapons Location button) and runtime
        // (pickBinding) share it (2026-08-15).
        [[nodiscard]] static std::string BindingCamLocationKey(const WeaponBinding& a_binding,
                                                               int a_subStateIdx, bool a_targetLock);

        // Location-override key for a bound weapon's CAMERA NOISE sub-state —
        // FpBindingLocationKey plus "|noise". Stored in the place's ordinary
        // stateNoise map, so it rides the existing noise persistence,
        // section-binding helpers and ActiveLocationStateNoise resolution.
        // Editor (noise Specific Weapons toolbar Location button) and the
        // binding blocks in ResolveStateNoise share it (2026-08-15).
        [[nodiscard]] static std::string BindingNoiseLocationKey(const WeaponBinding& a_binding,
                                                                 int a_subStateIdx);

        // The active location chain's camera profile for a binding key, or
        // nullptr. Presence in a place's bindingCam map IS the bind.
        [[nodiscard]] CameraProfile* ActiveLocationBindingCam(const std::string& a_key);

        // Resolve which EnemyFieldOverride governs the locked frame, in
        // priority order: an active specific-weapon binding (wins over
        // everything when it carries content) > directional power attack >
        // TL slot. Returns nullptr when none applies. `binding`/`bindingSlot`
        // come from the picker (the matched bound weapon for this state,
        // regardless of whether its base profile is enabled).
        [[nodiscard]] const EnemyFieldOverride* ResolveEnemyOverride(
            std::size_t enemyIdx, CameraProfile* resolved, std::optional<TLSlot> slot,
            const WeaponBinding* binding, int bindingSlot);

        // Apply enemy overrides for the resolved TL profile (splices the 6
        // camera fields of ResolveEnemyOverride's result; aim bias is read
        // separately by the lock-aim solve).
        void ApplyEnemyOverrideResolved(CameraProfile& dst, std::size_t enemyIdx,
                                        CameraProfile* resolved, std::optional<TLSlot> slot,
                                        const WeaponBinding* binding, int bindingSlot);

        CameraProfile transformationsWerewolf        = CameraProfile::WerewolfDefault(); // "Unsheathed" (claws out)
        CameraProfile transformationsWerewolfSheathed = CameraProfile::WerewolfDefault(); // claws in
        CameraProfile transformationsWerewolfSprint = CameraProfile::WerewolfDefault();
        CameraProfile transformationsWerewolfSwim   = CameraProfile::WerewolfDefault();
        CameraProfile transformationsWerewolfAttack = CameraProfile::WerewolfDefault();
        CameraProfile transformationsWerewolfPowerAttack       = CameraProfile::WerewolfDefault();
        CameraProfile transformationsWerewolfSprintPowerAttack = CameraProfile::WerewolfDefault();
        // (The six [WWPA] power-attack variants lived here 2026-08-30 for
        // one day — {standing, moving} x {left, right, dual}, enable-gated.
        // REMOVED at the user's request the same day; a preset that saved
        // transformations.werewolf.power_attack.<variant> keys just has them
        // ignored. Design + the werewolf attack event names it discovered
        // are preserved in memory (werewolf-pa-variants).)
        CameraProfile transformationsWerewolfRoar   = CameraProfile::WerewolfDefault();
        // Feeding on a corpse (2026-08-15). Latched from the vanilla feed
        // magic effect (archetype kWerewolfFeed) in StateResolver; the state
        // ends by stillness-net/timeout since the feed idle emits no clean
        // end signal. Its own row in Third Person and Target Lock.
        CameraProfile transformationsWerewolfFeeding = CameraProfile::WerewolfDefault();
        // Vampire Lord: 5 sub-state profiles. Sheathed is the idle/hovering
        // baseline (replaces the legacy single transformationsVampireLord
        // slot). Magic is the base for any spell-equipped state, overridden
        // by Concentration / Fire & Forget while actively casting that type.
        // Vampire Lord defaults. Engine-natural VL framing is very
        // tight (the slider value `zoom` is multiplied by 0.01, so
        // small values do almost nothing — `zoom=50` translates to
        // +0.5 added to the engine's -0.2 baseline, giving a meaningful
        // pull-back). FOV must be >= ~40 (a non-zero FOV) or worldFOV
        // ends up at 0 and the camera renders zoomed to a single point.
        static constexpr CameraProfile kVampireLordVanilla{
            .sideOffset = 0.0f, .height = 0.0f, .zoom = 50.0f, .fov = 80.0f,
        };
        CameraProfile vampireLordSheathed             = kVampireLordVanilla;
        CameraProfile vampireLordSheathedLevitating   = kVampireLordVanilla;
        CameraProfile vampireLordMelee                = kVampireLordVanilla;
        CameraProfile vampireLordMeleeAttack          = kVampireLordVanilla;
        CameraProfile vampireLordMeleePowerAttack     = kVampireLordVanilla;
        CameraProfile vampireLordMagic                = kVampireLordVanilla;
        CameraProfile vampireLordConcentration        = kVampireLordVanilla;
        CameraProfile vampireLordFireAndForget        = kVampireLordVanilla;
        CameraProfile vampireLordSprint               = kVampireLordVanilla; // "Sprinting (Ground)"
        CameraProfile vampireLordSprintLevitating     = kVampireLordVanilla;

        CameraProfile mountsHorseback        = CameraProfile::VanillaHorseback();
        CameraProfile mountsHorsebackSprint  = CameraProfile::VanillaHorseback();
        CameraProfile mountsHorsebackSwim    = CameraProfile::VanillaHorseback();
        CameraProfile mountsHorsebackMelee   = CameraProfile::VanillaHorseback();
        CameraProfile mountsHorsebackArchery = CameraProfile::VanillaHorseback();
        // Drawing/holding the mounted bow, using the same Attack signal as
        // on-foot Drawing. Zoomed takes precedence; release returns to Archery.
        CameraProfile mountsHorsebackArcheryDraw = CameraProfile::VanillaHorseback();
        // Mounted archery + the bow zoom held. Same signal as the on-foot
        // Zoomed rows (PlayerCamera::bowZoomedIn) and the same contract: this
        // profile REPLACES the vanilla zoom.
        CameraProfile mountsHorsebackArcheryZoom = CameraProfile::VanillaHorseback();
        // Mounted melee mid-swing, split by which side of the horse the swing
        // goes to. Resolved from StateResolver::GetMountAttackSide (latched
        // off freeRotation.x on the swing's first frame); both fall back to
        // mountsHorsebackMelee between attacks.
        CameraProfile mountsHorsebackMeleeLeft  = CameraProfile::VanillaHorseback();
        CameraProfile mountsHorsebackMeleeRight = CameraProfile::VanillaHorseback();
        CameraProfile mountsDragonRiding     = CameraProfile::VanillaDragonRiding();
        // Dragon-riding sub-states (2026-08-23). The base above is CRUISING —
        // powered flight, and the fallback for anything unresolved — so its
        // TOML key is untouched and a preset written before these existed
        // loads with identical framing. Each of the six is seeded FROM the
        // base at load when its key is absent, so an old preset gains six
        // copies of what it already had rather than six vanilla profiles.
        // Driven by StateResolver::DragonAction; see DragonRidingProfileFor.
        CameraProfile mountsDragonRidingPerched  = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingHovering = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingTakeoff  = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingLanding  = CameraProfile::VanillaDragonRiding();
        // Attack and Breath are a MATRIX against posture — a breath on the
        // ground, from a hover and on a strafing pass are three different
        // shots and each gets its own angle (user ruling 2026-08-23).
        CameraProfile mountsDragonRidingAttackGrounded = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingAttackHovering = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingAttackFlying   = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingBreathGrounded = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingBreathHovering = CameraProfile::VanillaDragonRiding();
        CameraProfile mountsDragonRidingBreathFlying   = CameraProfile::VanillaDragonRiding();

        std::vector<CameraProfile*> GetAllProfiles();

        // The two halves GetAllProfiles is built from. Split out 2026-08-17
        // because SwapCategoriesShoulders kept its OWN hand-copied duplicate of
        // the Categories list — and duplicating a 100-entry list is how the
        // Target Lock tree ended up silently missing from the shoulder swap.
        // One list, two readers: a profile added to either half is picked up by
        // enumeration AND by the swap, automatically.
        //
        // Categories = everything the Third Person tabs edit (states, shouts,
        // directional power attacks, per-hand magic). NO vanity, NO dialogue,
        // NO target lock.
        std::vector<CameraProfile*> GetCategoryProfiles();
        // The parallel Target Lock tree, same shape.
        std::vector<CameraProfile*> GetTargetLockProfiles();

        // Negate sideOffset on every profile that frames the camera over the
        // player's shoulder: Categories, TARGET LOCK, indoor variants, location
        // overrides, weapon bindings (both halves) and enemy overrides.
        // Excludes dialogue, vanity and first-person FOV, which are not
        // shoulder-framed. Used by the in-game shoulder-swap hotkey.
        // (Name predates the Target Lock coverage; the hotkey setting
        // categoriesShoulderSwapKey is a shipped TOML key, so neither was
        // renamed.)
        void SwapCategoriesShoulders();

        // ====================================================================
        // Outdoor / Indoor profile variants  +  per-place Location Overrides
        //
        // TWO environments (Outdoor / Indoor), plus an orthogonal Location
        // Override layer that sits ABOVE both.
        //
        // History worth keeping: "City" used to be a third environment in this
        // enum. It was removed 2026-08-02. An environment is a single global
        // set, so every city on the map had to share one tuning, and the only
        // way to bound it was a radius from the town's map marker - which is
        // exactly what failed in Windhelm, where parts of the city sit further
        // from the marker than the radius could reach without also swallowing
        // the road outside. Location overrides replace it: any number of named
        // places, each carrying its OWN sparse set of per-entry overrides, and
        // membership answered by the location record itself rather than by
        // distance. No radius, no false negatives inside a city, and Whiterun's
        // streets can differ from the Whiterun tundra because they are
        // different locations.
        //
        // Storage uses a pre-reserved std::vector so addresses stay stable
        // across the lifetime of the singleton (no rehash, no resize after
        // Init). outdoorIdxMap maps each eligible outdoor profile pointer to
        // its index in indoorProfilesStorage AND in every location override's
        // parallel vector - they are all built from the same eligible list in
        // the same order, so one index map addresses all of them.
        // ====================================================================

        // Environment indices. Used for categoriesEditTab, the *For(env)
        // accessors, and VariantOf().
        enum : int { kEnvOutdoor = 0, kEnvIndoor = 1, kEnvCount = 2 };

        // Live state - true while player's parent cell is interior. Updated
        // each frame by HookManager's per-frame hook. Not persisted.
        bool indoorMode = false;

        // Runtime environment: which of the two variant sets is live.
        [[nodiscard]] int RuntimeEnv() const
        {
            return indoorMode ? kEnvIndoor : kEnvOutdoor;
        }

        // UI state - which variant the Categories/Target Lock/Camera Noise
        // panels edit. 0 = Outdoor, 1 = Indoor. Persisted so the user keeps
        // the tab they were on across launches.
        int  categoriesEditTab = 0;

        // Indoor-variant storage. Reserved at Init time, never resized
        // afterwards - that's what keeps the &element addresses stable.
        std::vector<CameraProfile> indoorProfilesStorage;

        // Outdoor-pointer -> index into indoorProfilesStorage and into every
        // LocationOverride::profiles. Built in InitIndoorOverrides().
        std::unordered_map<CameraProfile*, std::size_t> outdoorIdxMap;
        // The inverse, so OutdoorOf can answer in O(1) instead of scanning the
        // whole map. Matters because a variant pointer now reaches OutdoorOf on
        // every frame a location override is active (the noise resolver maps
        // its pointer back through it).
        std::vector<CameraProfile*> idxToOutdoor;

        // -- Location Overrides ---------------------------------------------
        //
        // A named PLACE plus a sparse set of overrides that apply while the
        // player is in it. Available to every entry in Categories, Target Lock
        // and Camera Noise via the per-entry "Location" button.
        //
        // Membership is decided by walking the player's location PARENT CHAIN
        // (Breezehome -> Whiterun -> Whiterun Hold -> Tamriel) and taking the
        // FIRST ancestor any place matches. That single rule gives both of the
        // things asked for:
        //   * generalisation - capture "Whiterun" and every interior, street
        //     and district inside it is covered without listing them;
        //   * precision - a place captured on Whiterun beats one captured on
        //     Whiterun Hold, because the walk reaches the child first. So the
        //     city and the tundra around it can carry different settings.
        //
        // A place is exactly ONE rule, and the rule is always something the
        // game itself already knows about the spot you are standing in. There
        // are three, and they are deliberately the only three:
        //   * Location   - this specific place (Bleak Falls Barrow), or the
        //                  HOLD it sits in (Whiterun Hold) - a hold is just a
        //                  location record with LocTypeHold;
        //   * Keyword    - one vanilla LocType keyword carried by the place,
        //                  which is the game's own name for a CATEGORY of
        //                  place (all Nordic dungeons, all inns, all mines);
        //   * Region     - one CLIMATE region: the Reach, the Rift's fall
        //                  forest, the Whiterun tundra, Eastmarch's volcanic
        //                  tundra. This is how open country is answered, since
        //                  94% of Skyrim's exterior cells carry no location
        //                  record and no hold can be derived from them;
        //   * Cell       - one interior room, for the rare modded interior
        //                  that carries no location record at all;
        //   * Worldspace - the map you are on. A last-resort floor only, never
        //                  offered alongside the three above.
        // The last two exist so the answer to "where am I" is NEVER nothing:
        // open wilderness between locations has no location record, and a
        // screen that says "no location data" is useless to the player.
        //
        // One rule per place, rather than a bag of rules, because that is how
        // the choice presents itself in front of the player: "just here",
        // "everywhere like here", "everywhere round here". It also makes
        // precedence obvious - Location beats Keyword beats Worldspace, from
        // most specific statement to broadest.
        //
        // Keyword rules are what make mod-added content work with no patch: a
        // new dungeon carries the same LocType keywords its author already has
        // to apply for the map, fast travel and radiant quests.
        struct LocationOverride
        {
            enum class Kind : std::uint8_t { Location = 0, Keyword = 1, Worldspace = 2, Cell = 3,
                                             Region = 4,
                                             // A single ROOM BOUND inside an interior cell —
                                             // the portal-system boxes level designers author.
                                             // plugin+formID name the room-marker REFR. The
                                             // NARROWEST tier of all: the chain walk places
                                             // rooms ahead of everything else, so "this room
                                             // special, the rest of the dungeon from the
                                             // location override" needs no extra machinery.
                                             Room = 5 };

            std::string name = "New Location";
            Kind        kind = Kind::Location;
            bool        enabled        = true;
            // Shipped/migrated entries. Can be disabled and re-tuned but not
            // deleted, so a re-seed on the next load can't resurrect them.
            bool        builtIn        = false;

            // (Weather-conditioned places were removed 2026-08-13 at user
            // request; a legacy weather_mask key in a preset is ignored on
            // load, so an old "(Rain)" variant loads as a plain duplicate
            // rule the user can Remove.)

            // NOTE: there is deliberately no "applies indoors" flag.
            // Interiors are CHILD locations of the place they sit in, so the
            // parent-chain walk already answers them, and a more specific
            // place always wins: a place on Dead Man's Drink beats one on
            // Falkreath while you are inside the inn, with no second axis to
            // contradict it. The flag existed briefly and could only create
            // that contradiction.

            // Kind::Location / Kind::Worldspace - plugin + LOCAL form ID, so
            // the rule survives load-order changes; a missing plugin simply
            // never resolves, which is how a Dawnguard place no-ops without
            // Dawnguard. This is also the only way to reach a record carrying
            // NO LocType keyword at all (Fort Dawnguard and Castle Volkihar
            // genuinely have none).
            std::string   plugin;
            std::uint32_t formID = 0;

            // Kind::Keyword - the keyword's editor ID, e.g. "LocTypeDraugrCrypt".
            std::string keyword;

            // Kind::Room only - the display name of the location (or cell)
            // the room belongs to, captured at bind time. Pure GROUPING
            // metadata: the left list shows every room of one location under
            // a dropdown headed by this name. Never used for matching.
            std::string parentName;

            // Per-eligible-profile overrides, indexed by outdoorIdxMap.
            // profileSet[i] is the per-entry Enable: false means "this entry
            // has no opinion here", and resolution falls through to the plain
            // Outdoor/Indoor variant. Sparse by construction - a place the
            // user hasn't tuned costs nothing at runtime.
            std::vector<CameraProfile> profiles;
            std::vector<bool>          profileSet;

            // Indoor-variant slots, parallel to profiles/profileSet. An
            // entry's Outdoor and Indoor variants are INDIVIDUAL entries
            // (user ruling 2026-08-31): each binds places on its own, so
            // "Indoor Sprinting" bound to Dead Man's Drink says nothing
            // about plain "Sprinting" and vice versa. Same env-selector
            // shape as WeaponBinding::ProfilesFor.
            std::vector<CameraProfile> profilesIndoor;
            std::vector<bool>          profileSetIndoor;
            [[nodiscard]] std::vector<CameraProfile>& ProfilesFor(int e)
            {
                return e == kEnvIndoor ? profilesIndoor : profiles;
            }
            [[nodiscard]] std::vector<bool>& ProfileSetFor(int e)
            {
                return e == kEnvIndoor ? profileSetIndoor : profileSet;
            }
            [[nodiscard]] const std::vector<bool>& ProfileSetFor(int e) const
            {
                return e == kEnvIndoor ? profileSetIndoor : profileSet;
            }

            // Camera Noise. Presence in the map IS the per-entry enable (the
            // NoiseProfile's own `enabled` flag keeps its usual meaning).
            std::unordered_map<std::string, NoiseProfile> stateNoise;
            NoiseProfile globalNoise;
            bool         globalNoiseSet = false;

            // First person. Keyed by the same dotted FP state key
            // stateFirstPerson uses; presence IS the per-entry bind for
            // both channels.
            std::unordered_map<std::string, FirstPersonProfile> fpState;

            // Specific-weapon binding CAMERA slots (2026-08-15). Keyed by
            // BindingCamLocationKey ("binding.<plugin>|<fid>.s<slot>|cat" or
            // "...|tl"); presence IS the per-entry bind, mirroring fpState.
            // Bindings live outside the outdoorIdxMap registry, so they
            // can't use profiles/profileSet.
            std::unordered_map<std::string, CameraProfile> bindingCam;

            // Camera Noise transformation BEATS (2026-08-15) — the six
            // Transformations-tab beat rows (kFxBeatLocKeys). Presence IS
            // the per-entry bind, mirroring fpState/bindingCam; the value
            // fully replaces the global tuning while the place is active.
            // BeatTuning is reused as the value shape — direction/range
            // only mean anything for the Feeding row, same as globally.
            std::unordered_map<std::string, BeatTuning> fxBeats;

            // DIALOGUE looks (2026-08-20) — per-place framing for one dialogue
            // preset. Keyed by DialogueLook::uid rendered as decimal, because
            // a look has no profile pointer in outdoorIdxMap and neither its
            // list index nor its (editable, duplicated) name is stable. Presence
            // IS the bind, same as fpState / bindingCam / fxBeats.
            std::unordered_map<std::string, CameraProfile> dlgLooks;
        };
        std::vector<LocationOverride> locationOverrides;

        // Master switch for the whole layer. On by default - an override the
        // user never configures is a no-op, so there is nothing to protect an
        // existing setup from, and an opt-in nobody finds is a dead feature.
        bool locationOverridesEnabled = true;

        // ----- [CLIPCAM] per-animation camera overrides --------------------
        //
        // One entry per INDIVIDUAL animation file (the OAR replacement path
        // when OAR resolved one, else the clip's own animationName), matched
        // live by AnimationCameraController's hkbClipGenerator hook and
        // applied as the top framing override while the clip plays.
        //
        // Deliberately OFF the GetIndoorEligibleProfiles registry (that list
        // is index-mapped into every saved preset's location storage and is
        // fixed-cardinality); the indoor split is the WeaponBinding-style
        // explicit twin instead, selected in ONE place via ProfileFor(env).
        // Entries carry a uid from day one — the DialogueLook lesson: the
        // path is user-visible and the index moves, so anything that ever
        // needs to reference an entry (a place's override, a log line)
        // needs an identity that is neither.
        struct AnimationCameraEntry
        {
            std::uint32_t uid = 0;
            std::string   name;           // display name, user-editable
            std::string   animationPath;  // normalized (lowercase) match key
            std::string   subModName;     // OAR metadata, display only
            std::string   modName;        // OAR metadata, display only
            // (a per-entry `enabled` flag shipped for two days and was
            // removed 2026-08-30 — a bound animation is simply active, and
            // Remove is the off switch. The old `enabled` key is ignored.)
            CameraProfile profile{};        // outdoor
            CameraProfile profileIndoor{};  // indoor twin, seeded from outdoor at creation
            // Target-lock variants (2026-08-30, the Specific Animations tab
            // on the Target Lock page). Default-constructed = untuned; the
            // pickers use them only when locked AND a framing field moved —
            // the paraglide tlTuned precedent, so an untouched TL variant
            // never demotes the entry to defaults mid-lock.
            CameraProfile tlProfile{};
            CameraProfile tlProfileIndoor{};
            // Per-entry noise cell (the Specific Animations tab on the
            // Camera Noise page). noise.enabled is the Customize gate —
            // explicit, so browsing an entry can never create a live amp-0
            // cell ([[noise-entries-have-no-toggle-already]] is about the
            // PLAIN state cells; a keyed cell keeps the flag). Disabled =
            // the pre-existing behaviour (global noise while active).
            NoiseProfile  noise{};
            NoiseProfile  noiseIndoor{};   // mirrors outdoor unless diverged
            [[nodiscard]] NoiseProfile& NoiseFor(int a_env)
            {
                return a_env == kEnvIndoor ? noiseIndoor : noise;
            }
            [[nodiscard]] CameraProfile&       ProfileFor(int a_env)
            {
                return a_env == kEnvIndoor ? profileIndoor : profile;
            }
            [[nodiscard]] const CameraProfile& ProfileFor(int a_env) const
            {
                return a_env == kEnvIndoor ? profileIndoor : profile;
            }
            [[nodiscard]] CameraProfile&       TlProfileFor(int a_env)
            {
                return a_env == kEnvIndoor ? tlProfileIndoor : tlProfile;
            }
            [[nodiscard]] bool TlTuned(int a_env) const
            {
                const CameraProfile& t = a_env == kEnvIndoor ? tlProfileIndoor : tlProfile;
                const CameraProfile  d{};
                return t.sideOffset != d.sideOffset || t.height != d.height ||
                       t.zoom != d.zoom || t.fov != d.fov ||
                       t.rotation != d.rotation || t.pitchOffset != d.pitchOffset ||
                       t.ProximityBiasAnySet();
            }
        };
        std::vector<AnimationCameraEntry> animationCameras;
        // Master switch, same on-by-default reasoning as the location layer:
        // with no entries the feature is a no-op, so an old preset (which has
        // no [animation_camera.*] sections at all) loads to exactly 1.0
        // behaviour.
        bool animationCamerasEnabled = true;
        // Fresh-uid assignment, the AssignDialogueLookUids idiom: max-scan,
        // never a counter, so preset switches can't reissue a live id.
        void AssignAnimationCameraUids();
        // Entry lookup by uid; nullptr when gone. Callers re-resolve every
        // frame — the vector reallocates on menu edits.
        [[nodiscard]] AnimationCameraEntry* FindAnimationCamera(std::uint32_t a_uid);
        // Customized animation noise (location first); null leaves the
        // surrounding state/cinematic source in charge.
        [[nodiscard]] NoiseProfile* ResolveAnimationNoise(std::uint32_t a_uid);

        // Every place matching where the player is, NARROWEST FIRST.
        // Maintained by LocationDetector from the same per-frame hook that sets
        // indoorMode. Not persisted.
        //
        // A chain rather than a single winner because precedence is decided PER
        // ENTRY, not per place. Binding "All Cities" for Sheathed and then
        // "Whiterun" for Melee must not cost Sheathed its city framing just
        // because a narrower place now matches and happens to say nothing about
        // it. Each entry takes the first place in this list that actually binds
        // it, so a broad place is a base layer and a narrow one overrides only
        // what it defines — the same "more specific wins FOR THE FIELDS IT
        // DEFINES" rule the rest of the mod layers by (Outdoor→Indoor, state→
        // per-weapon override, TL slot→enemy override).
        std::vector<int> activeLocationChain;
        // The narrowest match, for logging and the menu's "active here" badge.
        int activeLocationIdx = -1;

        // UI state. locationEditIdx = which place the per-entry Location popup
        // is showing. locationEditActive is set to that index ONLY while the
        // popup body renders, which is what makes EditTarget route slider
        // writes into the place instead of the Outdoor/Indoor variant; -1 the
        // rest of the time. Neither is persisted as camera data.
        int locationEditIdx    = 0;
        int locationEditActive = -1;

        // Size every place's profiles/profileSet to the eligible list. Safe to
        // call repeatedly; only grows.
        void SizeLocationOverride(LocationOverride& a_lo);

        // Index of the place matching a rule, or -1. Used by the Location popup
        // to turn an offered option into a place without ever creating a
        // duplicate rule.
        [[nodiscard]] int FindLocationOverride(LocationOverride::Kind a_kind,
                                               const std::string& a_plugin,
                                               std::uint32_t a_formID,
                                               const std::string& a_keyword) const;
        // Create (or find) the place for a rule and return its index. Sized and
        // ready to take per-entry overrides.
        int EnsureLocationOverride(LocationOverride::Kind a_kind, const std::string& a_name,
                                   const std::string& a_plugin, std::uint32_t a_formID,
                                   const std::string& a_keyword);
        // Drop a place that no longer overrides anything, so backing out of a
        // choice does not leave dead rows behind. No-op while it still carries
        // per-entry overrides. Returns true when it was removed.
        bool PruneLocationOverrideIfEmpty(int a_idx);

        // The narrowest place the player is currently in, or nullptr. For
        // display and for the noise/global fallbacks; per-entry resolution goes
        // through the CHAIN (see activeLocationChain).
        [[nodiscard]] LocationOverride* ActiveLocation()
        {
            if (!locationOverridesEnabled) return nullptr;
            if (activeLocationIdx < 0 ||
                activeLocationIdx >= static_cast<int>(locationOverrides.size())) return nullptr;
            auto& lo = locationOverrides[static_cast<std::size_t>(activeLocationIdx)];
            if (!lo.enabled) return nullptr;
            return &lo;
        }

        // Index of the place that actually supplies this profile right now, or
        // -1. Walks the chain narrowest-first and returns the first that binds
        // it — the menu uses this to say which row is winning.
        // a_env picks which variant's slots (kEnvOutdoor / kEnvIndoor) —
        // runtime callers pass RuntimeEnv(), the menu passes
        // categoriesEditTab so the popup edits the variant on screen.
        [[nodiscard]] int ActiveLocationOwnerOf(CameraProfile* outdoor, int a_env);
        [[nodiscard]] int LocationOwnerOfResolvedProfile(const CameraProfile* a_profile) const;
        // Same for a noise key.
        [[nodiscard]] int ActiveLocationNoiseOwnerOf(const std::string& a_key);

        // Storage slot for `outdoor` inside place `a_idx`, or nullptr when the
        // profile isn't eligible / the index is out of range. Ignores
        // profileSet - this is the EDIT target, so the UI can write to a slot
        // before enabling it.
        [[nodiscard]] CameraProfile* LocationSlotOf(CameraProfile* outdoor, int a_idx, int a_env);
        // Whether place a_idx overrides `outdoor`'s a_env variant.
        [[nodiscard]] bool LocationSlotEnabled(CameraProfile* outdoor, int a_idx, int a_env);
        // Set/clear that flag (std::vector<bool> has no addressable elements).
        void SetLocationSlotEnabled(CameraProfile* outdoor, int a_idx, bool a_on, int a_env);
        // Sections a place can bind things in. A place is one WORLD rule
        // shared by all three, but what it binds is tracked per section: a
        // Location set up in Categories must not show up in Camera Noise as
        // though it were configured there.
        enum : int { kLocSecCategories = 0, kLocSecTargetLock = 1, kLocSecNoise = 2,
                     kLocSecFirstPerson = 3, kLocSecDialogue = 4 };
        // Does this place bind anything in that section?
        [[nodiscard]] bool LocationHasSectionBindings(const LocationOverride& a_lo, int a_section);

        // Runtime lookup against the ACTIVE place only. nullptr when there is
        // no active place or it doesn't override this profile.
        [[nodiscard]] CameraProfile* ActiveLocationVariantOf(CameraProfile* outdoor, int a_env);
        // Noise equivalents against the active place. State lookup is
        // enabled-gated like the env maps; global is set-gated.
        [[nodiscard]] NoiseProfile* ActiveLocationStateNoise(const std::string& key);
        [[nodiscard]] NoiseProfile* ActiveLocationGlobalNoise();

        // Transformation-beat equivalents. Stable per-row keys (UI fxBeat
        // index order: WW Transform, WW Revert, VL Transform, VL Revert,
        // Feeding, Bats). Walks the active chain narrowest-first; nullptr
        // when no active place binds the key. Owner variant returns the
        // place index for the UI badge, -1 when none.
        static constexpr const char* kFxBeatLocKeys[6] = {
            "ww_transform", "ww_revert", "vl_transform", "vl_revert",
            "feeding", "bats"
        };
        [[nodiscard]] BeatTuning* ActiveLocationFxBeat(const std::string& a_key);
        [[nodiscard]] int ActiveLocationFxBeatOwnerOf(const std::string& a_key);

        // First-person equivalents. Walk the active chain narrowest-first and
        // return the first place binding this FP key. Owner variant
        // returns the place index for UI labels, -1 when none.
        [[nodiscard]] FirstPersonProfile* ActiveLocationFpProfile(const std::string& key,
                                                                  bool a_fov);
        [[nodiscard]] int ActiveLocationFpOwnerOf(const std::string& key, bool a_fov);

        // Dialogue proximity pace (runtime only, never persisted). 1 = full
        // speed. HookManager's dialogue block writes it each frame from the
        // camera's distance to the nearest conversation subject (NPC face /
        // player head); every dialogue transition clock advances by dt×this,
        // so the move eases off as the camera nears a face and runs at the
        // slider's full speed out wide. Relaxes back to 1 after dialogue.
        float dlgPaceMul = 1.0f;

        // List of profiles eligible for the indoor split - the X-macro-style
        // single source of truth. Returns (toml_key, outdoor_ptr) pairs so
        // Save/Load can iterate them generically. Categories core + Shouts.
        // No target-lock, no dialogue, no vanity. tomlKey is owned by the
        // returned vector - recomputed each call so multiple invocations
        // don't accumulate state.
        struct IndoorEligibleProfile { std::string tomlKey; CameraProfile* outdoor; };
        std::vector<IndoorEligibleProfile> GetIndoorEligibleProfiles();

        // Build indoorProfilesStorage + outdoorIdxMap. Called from Load().
        // Each variant entry starts as a copy of its outdoor counterpart so a
        // fresh install behaves identically in every environment until the
        // user diverges them.
        void InitIndoorOverrides();

        // Returns the requested environment's variant of `outdoor` if
        // `outdoor` is eligible, otherwise returns `outdoor` itself. Env
        // kEnvOutdoor (or an unknown env) is always identity. Stable address.
        [[nodiscard]] CameraProfile* VariantOf(CameraProfile* outdoor, int a_env);
        [[nodiscard]] CameraProfile* IndoorVariantOf(CameraProfile* outdoor)
        {
            return VariantOf(outdoor, kEnvIndoor);
        }
        // Inverse of VariantOf: if `p` points into either variant storage,
        // returns the outdoor backing profile it was generated from. If `p`
        // is already an outdoor pointer (or unknown), returns `p`. Used by
        // Quick Tune to look the resolved profile back up in BuildCategoryTabs
        // (whose entries hold outdoor pointers only).
        [[nodiscard]] CameraProfile* OutdoorOf(CameraProfile* p);

        // Runtime resolver: returns the RuntimeEnv() variant of `outdoor` when
        // eligible; otherwise returns `outdoor` unchanged. The picker calls
        // this at the boundary just before dereferencing the resolved profile.
        [[nodiscard]] CameraProfile* ResolveByEnv(CameraProfile* outdoor);

        // UI helper: returns the categoriesEditTab variant of `outdoor` when
        // eligible; otherwise returns `outdoor`. Used by RenderProfileBlock
        // and the surrounding Reset/Apply buttons to keep the UI editing the
        // set the env toggle is pointing at.
        [[nodiscard]] CameraProfile* EditTarget(CameraProfile* outdoor);

        // Which Zoom slider range this profile should be EDITED with.
        //
        // Registered ONCE, here, keyed on the profile pointer — deliberately
        // not chosen at each render site. There are well over a hundred
        // RenderProfileBlock calls plus Quick Tune, the location-override
        // compact rows and the indoor variants, and the dragon's wide ranges
        // already proved what happens when a range is picked per site: Quick
        // Tune kept the human-scale constants and SILENTLY CUT any dragon zoom
        // above 200 the moment a slider was touched. Anything that routes
        // through OutdoorOf (indoor variants, location overrides, TL twins)
        // inherits the right range for free.
        [[nodiscard]] const Defaults::SliderRange& ZoomRangeFor(CameraProfile* a_p);

        // Pick the magic profile for the (school, castType) tuple resolved by
        // StateResolver. Returns weaponsMagic as the base when no active cast
        // (castType == None) or no detected school. During an active cast,
        // returns the matching magic{School}{CastType} slot — Concentration
        // and FireAndForget override the base, with Ritual as its own
        // category. Used by both CameraController and HookManager profile
        // selection so they pick the same slot for the same inputs.
        [[nodiscard]] CameraProfile* PickMagicProfile(MagicSchool school, CastType castType, bool isSneaking = false);
        [[nodiscard]] CameraProfile* PickStavesProfile(MagicSchool school, CastType castType, bool isSneaking = false);

        // The dragon-riding slot for one DragonAction. Cruising is the base
        // (mountsDragonRiding), which is also the fallback for an action that
        // somehow has no slot — never returns null, so the caller can assign
        // it straight into `selected`.
        //
        // Categories only: there is no target-lock dragon tree (see
        // tlMountsDragonRiding's removal note). ONE function for both call
        // sites (CameraController and HookManager each pick profiles
        // independently); a hand-copied switch in four places is how
        // mountsHorsebackMelee ended up configurable and never applied.
        [[nodiscard]] CameraProfile* DragonRidingProfileFor(DragonAction a_action);

        // The ten sub-states in menu order (Perched, Hovering, Takeoff,
        // Landing, then Attack x3 and Breath x3 by posture) — WITHOUT the
        // base, which is the thing they are usually being copied from or
        // reset against. One list so the reset, the load seed and the Apply
        // To Sub-states button cannot disagree about what "the sub-states"
        // are.
        [[nodiscard]] std::array<CameraProfile*, 10> DragonRidingSubStateProfiles();

        // Target-lock equivalents — identical semantics against the tl* tree.
        // a_ignoreHandOverride skips the per-hand splice and returns the plain
        // TLSlot-backed profile. Per-hand profiles live OUTSIDE the TLSlot table,
        // so anything slot-keyed (enemy overrides, the Quick Tune apply scopes)
        // must ask for the underlying slot instead of the spliced pointer —
        // otherwise SlotFromTLProfile misses and the whole layer goes dark.
        [[nodiscard]] CameraProfile* PickTLMagicProfile(MagicSchool school, CastType castType, bool isSneaking = false,
                                                        bool a_ignoreHandOverride = false);
        [[nodiscard]] CameraProfile* PickTLStavesProfile(MagicSchool school, CastType castType, bool isSneaking = false);

    private:
        void ApplyEngineSettings();
        SettingsManager() = default;
        SettingsManager(const SettingsManager&) = delete;
        SettingsManager(SettingsManager&&) = delete;
        SettingsManager& operator=(const SettingsManager&) = delete;
        SettingsManager& operator=(SettingsManager&&) = delete;
    };

    // ══ PRESET COMPATIBILITY — APPEND-ONLY CONTRACTS ═══════════════════════
    //
    // Almost everything DDC persists is keyed by NAME, which makes it safe to
    // insert into. These are the exceptions: values whose NUMERIC POSITION is
    // written into preset files, where inserting in the middle silently
    // re-points a user's tuning onto the wrong thing rather than losing it.
    // Losing a setting is visible. Re-pointing one is not.
    //
    // If a line below fails to compile, you have inserted rather than
    // appended. DO NOT "fix" it by editing the number — that just moves the
    // corruption into every preset in the wild. Append instead, then update
    // the expected value here in the same commit. Read PRESET-COMPAT.md.

    // (1) BindingCategory is written raw as `category` on every bound form
    //     (BuildSaveTable, weapons.bound_forms). New categories append.
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Melee)    == 0);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Bow)      == 1);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Crossbow) == 2);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Spell)    == 3);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Staff)    == 4);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Shield)   == 5);
    static_assert(static_cast<int>(SettingsManager::BindingCategory::Shout)    == 6);
    static_assert(SettingsManager::kBindingCategoryCount == 7,
                  "Appending a BindingCategory: bump this and add its static_assert above.");

    // (2) SpellCastType is written raw as `cast_type` on Spell bindings.
    static_assert(static_cast<int>(SettingsManager::SpellCastType::Unknown)       == 0);
    static_assert(static_cast<int>(SettingsManager::SpellCastType::FireAndForget) == 1);
    static_assert(static_cast<int>(SettingsManager::SpellCastType::Concentration) == 2);

    // (3) Binding sub-state indices are persisted as the TOML keys "s0".."sN"
    //     (profiles / tl_profiles / noise / enemy grids) AND baked into the
    //     location-override map keys by FpBindingLocationKey ("|%08X.s%d").
    //     A category's list may only ever GROW AT THE END — never insert a
    //     sub-state next to the one it reads best beside. The labels live in
    //     GetBindingSubStateName; keep the two in step.
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Melee)    == 15);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Bow)      == 8);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Crossbow) == 8);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Spell)    == 5);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Staff)    == 4);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Shield)   == 3);
    static_assert(SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Shout)    == 2);
    // The per-binding slot arrays are sized by the largest category.
    static_assert(SettingsManager::kWeaponBindingSubStates >=
                      SettingsManager::GetBindingSubStateCount(SettingsManager::BindingCategory::Melee),
                  "Binding slot arrays are too small for the largest category.");

    // (4) Magic hand slots are written as left/both/right by name, but the
    //     index IS StateResolver::CastingHand — keep the mapping fixed.
    static_assert(SettingsManager::kMagicHandCount == 3);
}
