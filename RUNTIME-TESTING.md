# Runtime testing and release evidence

## Version 1.2.0 release preparation (September 19, 2026)

The versioned Release DLL and all **41 standalone checks pass**. These include
startup logging without Verbose Logging, fatal output after abrupt process exit,
three retained prior sessions, missing/malformed Address Library reporting,
loaded-module capture, bounded hook diagnostics and preset/effect regressions.
The existing logging implementation is included in the release build.

A fresh offline matrix passes **210 scenarios on all 21 available Steam
executables**, with **47 hooked vtable targets per executable**. Both GOG images
remain missing; the matrix exits 2 for incomplete inputs, with zero test failures.
No game code is executed by these checks. Earlier 1.5.97/1.7.104 main-menu results
below refer to their named logging-build hashes, not this final 1.2.0 DLL.

DLL SHA-256: `1A41D065F534E3243325E439B46E0502CCEF3C0C5FB569DE9F3D27543213B170`.
PDB SHA-256: `96E4B5879E5D7BF49BDB9122A51057191B1ADC1554A2F733F43C01AC751C65CE`.
Evidence: `build/diagnostics/release-1.2.0-20260919/`.
The timestamped release candidate separately records packaging validation,
archive/source hashes and the extracted-source rebuild result. Matching symbols
are retained privately. No public upload or GitHub tag is created by preparation.

The release preserves the requested removals of Cinematic Views and the Apply
button. Paraglider timing experiments remain reverted; the separate Crowd
Modifier exclusion remains. The automated workflow does not change personal
settings, presets or the active profile's mod list. A live-game smoke test of
the final packaged DLL has not been performed during this preparation.

## Paraglider timing fixes reverted (September 19, 2026)

At the user's request, both experimental paraglider timing changes are reverted:
the isolated-stall guard and the subsequent continuous median filter. The
original wall-clock speed estimate, follow integration and inline leash math
are restored byte-for-byte from the pre-fix source snapshot. The temporary
`PlayerFollow` helper and its check target are removed. Buffered glide logging
remains, with its original schema; the experimental `followMs` field is removed.
Crowd Modifier's paraglider exclusion is retained, as separately requested.

Evidence is in `build/diagnostics/paraglide-stutter-revert-20260919/`, including
the archived experimental source and verification of the restored integration
files against `paraglide-crowd-20260919/before/`. The two sections immediately
below are historical records of the reverted experiments, not current behavior.

The Release build and all 41 checks pass. The reverted DLL/PDB are installed
in Finale with matching build hashes and the previous pair backed up. The five
protected settings/preset/mod-list files are unchanged. Live validation remains
pending.

## Reverted experiment: paraglider follow refinement (September 19, 2026)

The user confirmed that stutter was reduced but still visible. The 19:41-19:42
captures verify that the first timing guard works on large stalls: a 62.38 ms
interval uses 19.52 ms for follow and changes its correction by only 0.4 units;
a 133.13 ms interval uses 16.09 ms and stays near the previous correction size.
However, capture 2 still contains a 29.72 ms interval with a 143.2-unit follow
change and a 26.98 ms interval with a 102.5-unit change. Those intervals pass
the old 2x-median cutoff. Flee Framing is zero, the engine-relative framing is
nearly unchanged and no follow reset/wind guard fires on those frames.

Glide follow now uses the recent five-update median continuously after its
short warmup, covering both longer and shorter timing fluctuations without a
hard cutoff. Speed estimation and integration still share that interval.
Normal non-glide timing, steady-rate follow tuning and teleport handling are
preserved. The median still adopts sustained FPS changes within a few updates.
This replaces the threshold described in the earlier follow-up below.

`PlayerFollowChecks` adds two approximate fast-launch replays below the old
cutoff and a repeated long/short interval sequence at walking, glide and shout
speeds. Both replays now match an uninterrupted frame with identical movement;
the modeled corrections change from -129.08 to +9.90 units and -129.36 to
-34.88 units, respectively. The latter retains the expected smooth catch-up
as the player slows. The tests also check that repeated timing variation does
not alternately expand and contract the camera trail. These are numerical
replays, not measurements of the updated build in game.

Evidence and the retained session are in
`build/diagnostics/paraglide-follow-refinement-20260919/`. The large 326.5-unit
summary peak in capture 3 occurs after glide closes, with Flee Framing rising;
it must not be mistaken for an in-flight regression. Live acceptance should
distinguish steady flight from opening/closing and landing. Actual frame stalls
remain visible even when the camera no longer amplifies them.

The Release build and all 42 checks pass. The DLL/PDB are installed in Finale
with matching build hashes and the previous pair backed up. The five protected
settings/preset/mod-list files are unchanged. Live validation remains pending.

## Reverted timing experiment and retained Crowd Modifier exclusion (September 19, 2026)

The new 19:21 session produced two `[PARAGLIDE-TRACE]` captures. Capture 2
shows 72.02 ms and 80.06 ms update intervals, with player movement of only
19.6 and 15.0 units. DDC's player-relative follow correction changes by 323.8
and 235.4 units on those frames. The pre-follow camera changes by only 0.4
and 0.0 units. Chained-camera/noise time totals 0.71 and 0.64 ms respectively;
Flee Framing is zero, with no follow reset or wind-distance guard on either
frame. The capture's single wind guard occurs on deployment, separately.
This establishes a camera catch-up response to long update intervals, not
the cause of those intervals. It does not identify a GPU/streaming/mod culprit.

Follow speed previously divided movement by the long wall interval, pulling
the estimated speed and permitted follow distance down. Integration then
consumed that same long interval against the tightened distance window.
Paragliding now substitutes recent cadence for isolated intervals over twice
the median of five updates, using the same interval for speed and integration.
Sustained FPS changes enter the history normally. The fixed history runs before
deployment too, resets with follow initialization, and excludes long loading
gaps. Other states retain their existing wall-clock follow timing, and the
rate curve, speed-scaled leash and teleport limits are unchanged. The existing
math is shared in `Core/PlayerFollow.h` for production and regression checks.
The trace adds `followMs` so the effective interval can be compared with the
unclamped `frameMs` on the next live run.

Crowd Modifier's controller now checks the actual paraglide latch, returns no
adjustment and clears its envelope and cached encounter. This applies even if
the dedicated cinematic profile is disabled and to both normal and target-lock
glides. Landing starts a fresh encounter scan instead of restoring stale crowd
pressure. Saved Crowd Modifier tuning is preserved.

`PlayerFollowChecks` covers 20/30/60/144/240 Hz flight, injected stalls, continued
flight without delayed catch-up, sustained FPS changes, pause/invalid intervals,
unchanged non-glide timing, rigid/max looseness and teleport protection. A
one-axis replay of the second hitch's approximate preconditions changes the
old -230.42-unit correction to +6.60 units, identical to uninterrupted movement
in the same fixture. This is a numerical regression, not a live game capture.
The Release build and all 42 checks pass. Evidence, the original session logs
and selected trace rows are in `build/diagnostics/paraglide-crowd-20260919/`.
The DLL/PDB are installed in Finale with matching build hashes and the previous
pair backed up. All five protected settings/preset/mod-list files are unchanged.

Live acceptance: repeat the high-speed cliff/glide sequence, inspect entry
with an active crowd and a normal landing, then land for two seconds or pause
before quitting. Verify both Crowd Modifier channels stay absent in flight,
and compare `followMs`/`correctionStep` during any remaining update gaps.
Underlying frame stalls are not fixed by this camera-side change.

## High-speed paraglider stutter investigation (September 19, 2026)

The user reported stutter when using Whirlwind Sprint off a cliff at extreme
speed and deploying the paraglider. The retained `DietDrCamera.2.log` from
18:31 on September 19 contains a glide at 18:35-18:36. Its old `[PARAALT]`
samples include a 0.706-second interval, but those messages accumulate clamped
noise-envelope time and only run while gliding. They cannot establish frame
timing or identify the source of the stutter. The active preset uses maximum
looseness for Whirlwind Sprint and paragliding and full Flee Framing strength.
Framing uses the exact spring solver; no solver instability was established.

`[PARAGLIDE-TRACE]` now buffers the first four glide captures per launch. Each
retains up to 0.75 seconds before deployment and 12 seconds afterward, bounded
to 4096 updates. Collection stops earlier after two seconds out of glide or on
pause/loss of camera. A full/expired buffer waits until flight ends or pause
before formatting and logging. Short glide-latch drops remain in one capture.
The capture records unclamped camera-update intervals, time in the chained
camera call and following noise pass, player movement, positions before/after
DDC follow, correction changes, follow resets, looseness, Flee Framing, current
and target profile values, cell/state, and the wind altitude/speed multipliers
and distance-guard activation. Existing wind behavior is unchanged. The old
periodic altitude info logs are replaced by this deferred report.

Reports include 100 ms context rows and neighbouring samples around the three
largest frame gaps, follow-correction changes and camera-call durations. These
are CPU update measurements, not GPU present times. The chained-call duration
includes engine work and hooked code; it is not isolated vanilla engine cost.
Pre/post positions bracket DDC follow, before later scene-graph propagation,
and cannot prove that the renderer or another mod leaves the pose unchanged.
The follow/frame/noise tuning and all user settings remain unchanged.

`ParaglideDiagnosticsChecks` verifies 20/60/144/240 Hz captures with 12000-unit
per-second travel, a 250 ms stall, a camera-only 400-unit correction, pre-roll,
latch flutter, pauses, missing camera, full buffer and per-launch limits.
The Release build and all 41 checks pass. Evidence and copies of the retained
logs are in `build/diagnostics/paraglide-stutter-20260919/`.
The matching DLL/PDB are installed in Finale with the previous pair backed up;
the five protected settings/preset/mod-list files retain their original hashes.

Live acceptance remains pending. Leave Verbose Logging off, repeat the
Whirlwind Sprint/cliff/deploy sequence, then land and wait at least two seconds
or open a pause menu before exiting. Compare with a normal-speed glide in the
same area if possible. Read the matching trace before changing spring tuning.
A large frame interval alone does not identify streaming, GPU load, another
mod, or DDC as the cause. If normal frame intervals accompany abrupt follow
corrections, inspect resets/Flee/profile hand-offs; if wind guards or noise
cost spike, inspect that path. This initial build only added measurement;
the follow-up above documents the result and subsequent camera-side change.

## Extras Reset button sizing (September 19, 2026)

Projectile Tracing, Death Camera and Ragdoll Camera render their sliders in the
Extras host window. Cinematic Effects renders them in a child pane. ImGui 1.90.7
multiplies the child's font scale by its parent's scale, so the same 1.1 Reset
scale produced a larger button in Cinematic Effects (1.1 x 1.5 = 1.65).
The source behavior is documented by
[ImGuiWindow::CalcFontSize](https://github.com/ocornut/imgui/blob/v1.90.7/imgui_internal.h).

The three direct tabs now use that same effective Reset scale through a scoped
override. The shared Extras host scale constant also controls the tab host, so
the reference cannot drift independently. The normal inline layout, controller
registration, Reset defaults and slider sizing are preserved; the scope restores
the normal Reset scale when leaving each tab.

Evidence is in `build/diagnostics/extras-reset-buttons-20260919/`. In-game visual
acceptance remains unperformed: compare Reset size/alignment with Cinematic
Effects for both tracing views and every death/ragdoll slider, then confirm
mouse/controller Reset still restores each value's existing default.
The Release build and all 40 existing checks pass. The DLL/PDB are installed
in the active Finale profile with matching hashes and the previous pair backed
up. All protected settings/preset files and the mod list remain unchanged.

## Wider, stronger magic flybys (September 19, 2026)

Magic projectile flybys now use a 600-unit radius and 1.5 base intensity.
Arrows/bolts retain their 300-unit radius and 1.0 base intensity. The same smooth
distance falloff, cinematic texture and 0.22-second decay apply. Existing NPC
Magic/Transformations amounts still scale the effect independently in each view.

The tracker keeps its source's radius across resets, and the cinematic renderer
uses the separate magic gain. Activation diagnostics report the actual radius
and base gain. Ownership, projectile eligibility, player-hit cancellation and
the dedicated dragon/centurion/Forgemaster cinematic exclusions are unchanged.
No settings or preset fields are added.

`ProjectileFlybyChecks` exercises eligible and excluded spell records at 75,
450 and 600 units in both views at 20/30/60/120/240 FPS, including slow/fast mod
missiles, staves, scrolls and Ice Storm. An identical 450-unit pass reaches magic
but not arrows/bolts after resets. The actual first-person cinematic sampler
checks magic's increased base output, wider falloff, quiet boundary, mute and
user intensity scaling alongside the existing archery calibration.

Evidence is in `build/diagnostics/magic-flyby-strength-20260919/`. In-game feel
still needs checking with close and wider spell misses in both views, alongside
arrows at the same NPC Noise amounts.
The Release build and all 40 checks pass. The DLL/PDB are installed in the
active Finale profile with verified hashes and the previous pair backed up.
All protected settings/preset files and the mod list are unchanged.

## Per-slider bulk apply removed (September 19, 2026)

Removed the experimental per-slider Apply button and its popup from Third
Person, Target Lock, First Person and Camera Noise at the user's request.
The slider/reset controls, build integration and preset checks are restored to
their verified pre-feature contents. The standalone bulk-apply implementation
and its dedicated test include are removed. Existing copy/paste controls and
preset values are retained.

Removal evidence, build/test logs and deployment hashes are in
`build/diagnostics/slider-apply-removal-20260919/`. Earlier implementation and
popup revisions remain archived in their original diagnostic directories.
The Release build and all 40 checks pass. The installed DLL contains none of
the removed popup's identifiers. The DLL/PDB are deployed to the active Finale
profile with verified hashes and the previous pair backed up. Protected
settings, presets and the mod list are unchanged. In-game validation was not run.

## Magic projectile flybys (September 19, 2026)

The user confirmed the preceding arrow/bolt correction works in game. The
captured `working-archery-session.log` includes detected passes and cinematic
contributions. That confirmation applies to archery, not this magic extension.

The initial extension used the same 300-unit falloff and cinematic texture for
traveling spells, staves and scrolls; the later tuning revision above increases
magic's radius and intensity. Transformed casters retain NPC Transformations
ownership. Separate trackers/event storage keep Magic and Archery independent.
No new preset fields or player Repulse/profile dependencies are introduced.

The missile update/contact observers now report flybys. Unconditional cone
update/contact observers also cover Ice Storm with player tracing disabled.
They chain the previous handlers. Cone contacts use the projectile center,
not the wave's surface; hitting another actor does not end its tracking. Known
player contacts cancel pending flybys. Camera-thread shooter-handle resolution
uses the existing dragon/centurion/Forgemaster cinematic ownership classifier,
even when those dedicated sources are off.

Game-record fixtures cover Firebolt, Fireball, Ice Spike, Icy Spear, Ice Storm,
Magelight, spider spit, seeker missiles, lurker spit and vampire-lord bolts.
Compatible mod spells use the same metadata path without names or FormID lists.
Instant beams, concentration streams, flamethrowers, runes and voice powers
retain their existing effects. Native 1.6.1170 inspection confirms missile and
cone velocity getters use CommonLib's relocated `linearVelocity` fields.

`ProjectileFlybyChecks` adds those records plus staff/scroll/mod metadata, both
views at 20/30/60/120/240 FPS, slow/fast projectiles, independent sources, piercing
waves and player-hit cancellation. Existing checks cover geometry, wall stops,
recycling, reset behavior, cinematic output and creature cinematic ownership.
Evidence is in `build/diagnostics/magic-flyby-20260919/`. Bounded ordinary-log
`[NPC-MAGIC-FLYBY]` samples/counters report eligibility, ownership skips and
pass strength; `[NPCNOISE] contributing` identifies rendered magic flybys.

The Release build and all 40 CTests pass. `deployment.json` records the installed
DLL/PDB hashes under `C:/SkyrimMo2/mods/Diet Dr Camera/SKSE/Plugins` in Finale;
the previous working pair is backed up. All four protected settings/preset files
and the mod list remain byte-identical to this task's intake.

Live acceptance still needed for this extension:

- Compare near/distant mage and spider misses in both views, including Firebolt,
  Ice Spike and Ice Storm with projectile tracing off.
- Check direct hits, wall stops and Ice Storm passing another actor first.
- Mute Magic/Archery independently; check vampire-lord bolts with Transformations
  and confirm player casts do not create NPC flybys.
- Check dragon breath and centurion steam with their dedicated amounts on/off;
  neither should acquire an extra generic flyby layer.

## Flyby detection and output correction (September 19, 2026)

The initial flyby build was reported as not working. The captured session at
16:11 loaded the candidate, and the active preset had Archery 1.0 in both views.
It recorded an incoming arrow hit but no flyby diagnostics because those were
debug-only. This log alone does not prove whether any close misses were detected.

Source/native inspection found that the new flight gate read `velocity`, while
arrow flight uses the separate `linearVelocity` member. On the available
1.6.1170 image, ArrowProjectile vtable slot 0x86 resolves to ID 44183 and reads
offsets 0x104/108/10C; UpdateFlightPath ID 44197 calls that getter and applies
gravity to 0x10C. These are CommonLib's relocated linear-velocity fields. The
fix reads that field through a shared adapter, and moves observation into the
unconditional arrow update observer rather than the optional aim-correction
hook. Existing update/contact hooks are retained; no offsets are embedded in
production code and no relocation IDs are added.

The initial 0.16 amplitude and 0.65 rotation weight also produced extremely
small output. The flyby now uses amplitude 1.0, rotation 2.2 and a 0.22-second
decay before the renderer's normal POV gains. Distance weighting and the user's
NPC Archery amounts still apply; player weapon profiles and Repulse do not.
Checks now exercise zero auxiliary/nonzero linear velocity and the actual
first-person cinematic sampler, including output strength, falloff and mute.

Default-level, bounded `[NPC-FLYBY]` diagnostics report observer installation,
active listener/amount, both velocity magnitudes and filter results, counters,
detected passes and their distance/strength. `[NPCNOISE] contributing` confirms
when a flyby reaches the cinematic event output. These distinguish detection
failures from output/tuning failures without changing user logging preferences.

Evidence and the reported log are in
`build/diagnostics/archery-flyby-fix-20260919/`. The Release build and all 40 CTests
pass, including the added native-velocity and rendered-output regressions.
`deployment.json` records the installed DLL/PDB hashes in MO2's Finale profile;
the replaced pair is retained under `previous-binaries/`. The four protected
settings/preset files and mod list remain byte-identical to this fix's intake.
The native evidence is read-only
inspection of the existing private 1.6.1170 fixture, not gameplay verification
or certification of other executable versions. The live acceptance list below
still applies to the corrected build.

## Arrow and bolt flyby noise (September 19, 2026)

NPC Noise > Archery now controls passing projectiles in each view. The old
release beat and player bow/crossbow noise resolver are removed. Repulse was
not read by that old beat; the replacement also has no dependency on player
weapon noise, item bindings or Repulse. Existing preset amounts are retained.

The existing arrow update/contact hooks feed actual positions into a bounded,
thread-safe tracker. Closest approach is measured against the player's upper
body, including player motion between samples. Squared smoothstep falloff is
continuous from full strength at zero distance to silence at 300 game units.
Only a segment that reaches closest approach can arm the brief cinematic
texture; no velocity prediction extends a shot through a wall. Known player
contacts cancel pending flybys so Damage Reaction owns the hit. A volley uses
the existing loudest-wins event pool, with one pass per projectile identity.

The Release build and all 40 CTests pass. Evidence is kept in
`build/diagnostics/archery-flyby-20260919/`, including source snapshots,
build/check logs and protected settings hashes. `deployment.json` identifies
the installed DLL/PDB in MO2's Finale profile and the previous binary backup.
All four settings/preset files and the mod list retain their original hashes.
No new hooks, relocation IDs or preset fields are introduced.

`ProjectileFlybyChecks` covers arrows/bolts at 20, 30, 60, 120 and 240 FPS;
speeds of 1,500/3,000/12,000 units per second; full-segment close passes; moving
players; vertical/world-offset geometry; distance monotonicity; no release or
approach burst; wall stops; player-hit cancellation; pooled/recycled identities;
64 simultaneous tracks; and stale/disabled/POV/cell/teleport resets.

Remaining live acceptance checks:

- Compare near, medium and distant misses from both bows and crossbows. A
  release aimed away from the player should produce no archery noise.
- Change player bow/crossbow noise, item bindings and Repulse from zero to high
  values: flyby character should stay the same. NPC Archery amounts still scale
  it independently in first and third person, including when base noise is off.
- Stand close to the line of fire and behind cover. Confirm a wall-stopped
  projectile produces no imagined pass beyond the collision; take a direct hit
  and check Damage Reaction without a second flyby burst.
- Test moving, sneaking, mounted play, rapid volleys, slow time, menu/POV changes
  and save/load. Check that effects settle and no queued flyby replays.
- Keep wolf/bear melee noise, dragon breath and centurion steam behavior from
  the preceding creature update. Flyby detection requires an arrow/bolt source
  and cannot consume a spell, cone or flame projectile.

Automated flight checks do not establish the final in-game feel or certify
additional Skyrim runtime versions.

## Creature magic contacts and nearby noise (September 19, 2026)

Follow-up to the spider-spit report. Reviewed UESP and all 152 final non-NPC
races across the five game/expansion masters and 74 installed Bethesda Creation
plugins. The audit now follows race/NPC attacks, correlated template choices,
spells, enchantments, effect archetypes, projectiles, hazards, explosions and
script properties. The detailed matrix and sources are in
[DAMAGE-REACTION-COVERAGE.md](DAMAGE-REACTION-COVERAGE.md).

Ordinary spider spit has a discrete venom contact followed by its poison
stream. Hostile web/paralysis, resource-drain and force/stagger contacts no
longer depend solely on a health-value callback. Utility/self effects remain
excluded. A spell's secondary slow does not duplicate its damaging melee hit.
Carrier and explosion-enchantment notifications for an area force impulse are
coalesced across camera frames. Different attackers and projectile identities
remain independent.

School-less creature spells can use existing NPC magic tuning. Cast events and
projectile releases share per-caster receipts; contact payloads do not create
fake ranged-cast beats. Wolf/bear and other ordinary creature melee noise is
retained. One race/rig ownership helper serves both the cinematic scanner and
all generic NPC-noise exclusions. Dragon breath and centurion/Forgemaster
breath retain their dedicated noise controls even when their intensity is zero.
Incoming breath damage remains a continuous reaction, with no new discrete
breath-impact path.

Evidence, exact installed DLL/PDB hashes and protected-file verification are in
`build/diagnostics/creature-attacks-20260919/deployment.json`. The previous pair
is retained in that directory's `previous-binaries/`. Installation copies only
the DLL/PDB into the enabled Diet Dr Camera mod in MO2's Finale profile; the
four settings/preset files and mod list are checked against saved hashes.
No new engine hooks, relocation IDs, preset fields or UI controls are added.

Automated validation: Release build and 39 CTests, including 144 physical race
fixtures, 30 full spell fixtures plus the colossus explosion's stagger effect,
duplicate notifications, separate attackers/projectiles, utility filtering,
cinematic ownership and the existing motion/direction/recovery checks.
These are source/record and headless checks, not a new live encounter or runtime
version-matrix certification.

Remaining in-game acceptance pass:

| Encounter / settings | Expected behavior |
| --- | --- |
| Small/large/giant spider; dodge then take spit | Nearby release noise may occur on a miss; the venom impact requires contact, followed by poison motion only while damaging ticks arrive. |
| Spider web/paralysis or hostile slowing replacement | One soft contact response without needing health damage; no camera motion from self-applied utility slowdown. |
| Wolves and bears, melee noise enabled | Existing nearby swing noise remains, with the physical incoming reaction on a hit. |
| Seeker mouth projectile, chaurus spit, lurker spit/spray | One release/contact per attack; drain/poison streams remain smooth. |
| Lurker/colossus/giant area attack | A force contact can occur outside the physical strike; carrier/explosion reports do not repeat the same force impulse. |
| Dragon breath; Damage Reaction off, NPC noise on, dedicated breath off/on | No generic breath layer; enabling the dedicated source restores only its own noise. Check both views. |
| Centurion steam and Forgemaster breath; same settings | Same exclusive ownership, including when the first-person cinematic intensity is zero. |
| Either breath; NPC/cinematic noise off, Damage Reaction on | A smooth reaction only while damaging the player; no repeated discrete projectile jolts. |
| Menus, POV changes, disabled noise, unload/reload | No stored projectile release or stale cast beat replays. |

## Creature hit-reaction coverage (September 19, 2026)

Updated the Flee Framing description to the requested sentence and reduced
Damage Reaction to one short description. Added physical contact profiles and
classification for the UESP creature roster, checked against 79 installed
Bethesda plugins. A frozen fixture covers all 144 final non-NPC race records
with attack events. Named-attack checks cover frost atronach arms, centurion
hands, lurker/colossus stomps, creature bites/claws, tusks, hooves/rams, mechanical
weapons, ballista bolts, spirits, weapon proxies and bow/crossbow bashes.

The Release build and all 39 CTests pass. Existing contact shape values and the
complete motion mixer are unchanged; new profiles run through the same timing,
direction, overlap and lifecycle checks, including first-person recovery at
30/60/144/240 FPS. No new engine hooks or relocation IDs were introduced.
In-game encounter feel and mod-specific animation replacements are not newly
validated. [DAMAGE-REACTION-COVERAGE.md](DAMAGE-REACTION-COVERAGE.md) records the
UESP sources, contact choices, coverage and fallback limits.

DLL SHA-256: `935EA6B1A4498D1577581C38CD59DE653F85FC19C36C92A5C2826D945A6DF428`.
The tested DLL and matching PDB are installed in the active `Finale` MO2 profile.
Skyrim was closed, and all five protected settings/preset/modlist files are
unchanged. Previous binaries, settings backups, research and test/deployment
records are under `build/diagnostics/damage-enemies-20260919/`.
Cinematic Views remains excluded from the build.

## Cinematic Views removed from the current build (September 19, 2026)

The DLL excludes the Cinematic Views controller and picker. Its Extras tab,
Quick Tune panel, camera selection/transition logic, input and hook calls, and
SKSE serialization registration are removed. Source remains available for future
design work. Preset format 13 still reads and writes existing view definitions,
but they are excluded from active profile enumeration.

The Release build and all 39 CTests pass, including preset round trips and the
updated inactive-profile check. Generated project inspection confirms neither
implementation is compiled. Binary checks confirm the menu and runtime markers
present in the previous DLL are absent from this one, while Cinematic Effects,
Crowd Modifier and the stored-view codec remain present. Live gameplay has not
been tested for this build.

DLL SHA-256: `B35E68E1F0266B389F9866B2AD48490A44723564904DB74EE26057A9FAB8D9A6`.
Installed DLL/PDB match the tested build in the active `Finale` MO2 profile.
Skyrim was closed; all five protected settings/preset/modlist files are unchanged.
Source snapshots, previous binaries, settings backups, test logs, removal checks
and the deployment record are under
`build/diagnostics/cinematic-views-removal-20260919/`.
The feature descriptions and verification records below are historical.

## Cinematic Views trigger positions and distant scenery (September 19, 2026)

New bindings store a player-centered trigger point separately from the subject.
Set Here relocates that trigger in the same worldspace/cell. Aimed Scenery adds
bounded picking of rendered terrain, distant water and scene geometry; playback
uses the saved world point without requiring a loaded reference. Preset format 13
preserves missing trigger points as legacy subject-centered activation.

The Release build and all 39 CTests pass. Added cases cover separate areas and
encounter histories for one distant target, height/radius/worldspace boundaries,
trigger relocation, far-ray limits, format-13 round trips, format-12 migration
and rejection of malformed trigger data. Headless panel checks pass at 1080p,
1440p, 2160p and narrow width, including Set Here dispatch and scale restoration.
Live terrain/water picking and gameplay remain unverified for this build.

DLL SHA-256: `D1E7B9A2A69C02F4FA10D02F61E64ADB373387A863A5A26B67C32CE5B82CA9EE`.
Installed DLL/PDB match the build outputs in the active `Finale` MO2 profile.
Skyrim was closed; settings, presets and the modlist remain unchanged and are
backed up with the previous binaries under
`build/diagnostics/cinematic-triggers-deployment-20260919-132119/`.
Test and layout evidence: `build/diagnostics/cinematic-triggers-ctest-final-20260919.log`
and `build/diagnostics/cinematic-triggers-preview-20260919/`.

## Cinematic Views readability adjustment (September 19, 2026)

Increased slider labels and track size, enlarged both lists and widened the view
list. Child panes now size their text independently. The Release build and all
39 CTests passed. Headless layout and interaction checks passed at 1080p, 1440p,
2160p and a narrow panel width; in-game visual verification remains pending.

Installed the DLL and matching PDB into the active `Finale` profile's normal MO2
mod with Skyrim closed. Settings and the modlist are unchanged. Installed DLL
SHA-256: `1F80011538DFA8AD9E4D3BD94F1E44407DA9C6E26B28691E01860674278BAD25`.
Previous binaries and the deployment record are in
`build/diagnostics/cinematic-readable-deployment-20260919-123733/`;
layout evidence is in `build/diagnostics/cinematic-readable-preview-20260919/`.

## Cinematic Views UI deployment (September 19, 2026)

Installed the compact Cinematic Views build in the active `Finale` profile's
`C:/SkyrimMo2/mods/Diet Dr Camera/SKSE/Plugins/` directory. Subject rows bind on
activation; search, Bind, Enabled and Preview controls are removed. The pane uses
Cinematic Effects' compact sliders and text scaling. The Release build and all
39 CTests passed, as did headless layout and interaction checks. In-game visual
verification remains pending. Skyrim was closed during installation.

Installed DLL SHA-256:
`D49D6269F81C66895F80D0D44237FDE48D166DE65B5EBBB9603CEE4B540B6C5A`.
Both DLL and PDB match the build outputs; settings and the profile modlist are
unchanged. Previous binaries and the deployment record are preserved under
`build/diagnostics/cinematic-simple-deployment-20260919-122553/`.
Earlier entries below describe their own build hashes and deployment status.

## Startup logging verification (September 19, 2026)

The next-update logging build passes all **39 CTests**, including new checks
that abruptly exit child processes and verify fatal-log persistence and three
retained sessions. It passes **210 offline hook scenarios on 21 Steam images**
and fresh isolated main-menu startup on **1.5.97/SKSE 2.0.20** and
**1.7.104/SKSE 2.3.1**. Both real startup logs show the correct runtime/SKSE,
the same DLL PDB identity, the matching Address Library header, loaded modules
and completion checkpoints. Private-profile binaries and the shared catalog
were restored. Gameplay, normal exit and the missing GOG images are not newly
validated. The build has not been deployed to the normal installation or published.

DLL SHA-256: `195E14871B7745009A6B29F098EF6FFB2AA5E8CE0CAEEC32605010AE52A70A32`.
PDB identity: `1FCAC9AA-AB94-4336-8F24-4A933B3371F6`, age `201`.
Evidence: `build/diagnostics/logging-verification-20260919.json`,
`logging-ctest-final-20260919.log`, `logging-matrix-final-20260919/`, and
`runtime-matrix-20260919/startup-1.5.97-20260919-110205/` plus
`runtime-matrix-20260919/startup-1.7.104-20260919-110242/` under the same
diagnostics directory. [SUPPORT-LOGGING.md](SUPPORT-LOGGING.md) describes the
logs to request. The earlier stages below retain their original build hashes.

## Expanded executable matrix (September 19, 2026)

The candidate at this earlier stage passed **210 offline scenarios across all 21 Steam
runtime targets**, with all **47 hooked vtable targets** checked on each image.
All original executables match Steam's manifest sizes and SHA-1 hashes. The
released databases contain all 59 explicit DDC IDs for all 23 loader targets;
GOG 1.6.659/1179 executable fixtures remain missing. The Release build and
38 CTests pass. See [the complete matrix and commands](RUNTIME-MATRIX.md).

The expanded tests reproduced the same existing-callback rejection on 1.7.99
that the earlier audit fixed only on 1.7.104. Both camera and furniture cases
now pass on both independently reviewed executables. Unsafe call targets,
competing UI drivers and changes after preflight remain rejected.

This DLL also passes fresh isolated main-menu startup on 1.5.97/SKSE 2.0.20
and 1.7.104/SKSE 2.3.1 with Menu Framework 3.14.1. Hooks install, game data loads,
the DDC menu registers, and the process stays alive ten seconds after main-menu
opening. Controlled process termination is recorded; normal exit and gameplay
are not counted as tested. Previous private-profile binaries and the shared
Creations catalog were restored. No normal-installation deployment or publishing
occurred. One initial harness cleanup race was repaired; that run is not counted.

Evidence: `build/diagnostics/runtime-matrix-20260919/`. DLL SHA-256:
`531678CCFDC744C6995EAB8504F703A4D46DA1DC9BC8D76C0854DFA2E759F830`.
The older audit below is retained as the preceding stage's evidence.

## Unreleased runtime compatibility audit (September 19, 2026)

The Release build and 38 checks pass, including a new regression that removes
each SKSE export from a private copy of the compiled DLL. The binary verifier
now requires the legacy SE Query export alongside Load and the AE Version
declaration. A current-source inventory finds all 59 explicit IDs in mappings
covering the 23 loader-listed versions, including the macro/template vtables
omitted by the earlier inventory. This is address coverage, not ABI proof.

Seven offline scenarios pass on each of 1.5.97, 1.6.1170 and 1.7.104: clean
preflight, existing camera/furniture callbacks, a wrong engine call target,
an already-disabled UI job, a replaced main-thread call and a replaced dialogue
decrement. These execute production preflight against private mapped images;
they never execute game instructions. Original hook selection rejects the
two existing-callback scenarios on 1.7.104. The revised code accepts those
verified sites and continues to reject unsafe conflicts. Failure cases leave
the inspected patch sites unchanged. The modified UI-job check identifies
an existing UI driver rather than treating its unconditional branch as an
unrecognized native layout; it still refuses a second driver.

The report's RuntimeHooks.cpp(24) location is the shared failure handler, not
a unique cause. The user's exact older runtime/mod combination is still
unknown, and this work must not be presented as its confirmed fix. No new
game launches, gameplay tests, installation or publishing were performed.
See [the full research and evidence](RUNTIME-COMPATIBILITY-AUDIT.md).

Source snapshots, original-selection failures, current scenario reports and
hashes are under `build/diagnostics/runtime-audit-20260919/`. DLL SHA-256:
`868EDB8D941D16898F8EFF21633A259F2D3F39B193A7463E413D302BD006E1F5`.

## Unreleased false damage reactions from block modifiers (September 18, 2026)

The user confirmed the native R3 reset is fixed, but still reported occasional
jerks when blocking during noise. The 16:43-16:44 session records five discrete
magic reactions from source `3C12B39B`, with no incoming direction, interleaved
with genuine blocked weapon contacts. The active load order and installed plugin
resolve that source to Adamant.esp's `MAG_BlockSlowdownSpell` (`0012B39B`). Its
`MAG_BlockSlowdownEffect01` (`0012B39A`) is a non-hostile detrimental speed
modifier, magnitude 40, rather than an incoming attack. The second effect is a
recovering weight modifier and is not marked detrimental.

The ValueModifierEffect observers previously accepted any negative detrimental
or hostile actor-value change, without checking which value was changed. They
therefore injected an extra magic impulse when the block slowdown applied.
Observers now require a damaging health-value application. Negative speed,
armor, stamina, magicka and other utility modifiers no longer create reactions;
the original effect handler still runs normally. This is an event filter, not
health scaling: player health totals and damage magnitude never control motion.
Weapon and blocked contacts still use TESHitEvent independently of health loss.
The accepted damage curves, directions, travel and stream behavior are unchanged,
as are the first-person noise mixer and the now-accepted R3 hooks.

The new regression uses the recorded block-spell flags and magnitude while
mixing continuous fire and blocked greatsword contacts in both views at
30/60/144/240 FPS. Applying/releasing the utility effect must add no motion;
the legacy predicate is compared to demonstrate the false impulse. Healing,
invalid ticks, restoration of beneficial effects, tiny/large damaging ticks and
non-hostile detrimental damage retain coverage. Evidence and local source-record
metadata are under `build/diagnostics/block-jerk-20260918/`.

The Release build and all ten relevant regression checks passed. The recorded
fixture adds up to 0.050983 radians of extra motion with the legacy intake and
zero with the corrected filter. The reaction implementation outside that intake
predicate, CameraNoiseController.cpp and HookManager.cpp match their pre-change
copies. No new runtime hook or offset is introduced.

The DLL and matching PDB were installed in the usual MO2 mod at 16:52; installed
hashes match the build. DLL SHA-256:
`619EAD0A5BEB9200AE035CB05B562DEC7CAC5C2FE891EF8A89EC72EE33E125C9`.
Previous binaries, unchanged camera preferences/presets and TDM settings, test
logs and a deployment manifest are backed up in
`build/block-jerk-backup-20260918-165253/`.

In-game acceptance of this additional block fix is pending.

## Unreleased first-person source continuity and native R3 reset (September 18, 2026)

The user accepted the increased first-person damage travel, but reported a snap
when entering block during noise from other sources. First person was combining
cinematic/NPC amplitude and character with the player's current noise profile,
then capturing that combined sample in the profile's texture crossfade. Changing
the player profile could move ongoing external noise to a different phase and
retain an expired source in the outgoing texture. Dragon, jump, draw, attack,
event and NPC sources now keep independent clocks and character followers. Their
existing source envelopes feed separate samples outside the player-profile fade.
The sampler keeps the established two-band waveform and per-view gains. Sources
use their own rotation settings instead of borrowing the player's rotation floor.
Damage Reaction's accepted impulse, recovery and continuous-magic tuning is
unchanged. This addresses a demonstrated mixer dependency; the reported in-game
block transition still needs retesting.

The earlier TDM-only R3 change was incomplete. Inspection of Skyrim 1.6.1170's
native third-person and horse camera input handlers confirms a separate release
path that resets freeRotation. Their secondary PlayerInputHandler base is at
0x20; native writes at base-relative 0xB4/0xB8 address the full state's
freeRotation at 0xD4/0xD8. The new hooks preserve those two values across an R3
Toggle POV release while chaining the original handler. They do not consume or
rewrite the input event. TDM target-lock handling, held POV behavior and native
input bookkeeping still run; keyboard POV and menu inspect paths are unchanged.
The previously disabled TDM reset preference remains disabled.

The Release build and ten relevant regression checks passed: first-person source
continuity, noise transitions, runtime layout, camera quality, presets, damage
reactions, melee/archery/magic hit shake and spell trajectories. Source checks
use the production waveform at 30/60/144/240 FPS, including overlapping sources,
rapid profile fades, source expiry, live character changes and paused Quick Tune.
Layout checks verify the input base adjustment and orbit offsets across eight
runtime layouts. Full executable-image preflight, including both new input
vtable targets, passed for 1.5.97, 1.6.1170 and 1.7.104 without executing game code.
Evidence is under `build/diagnostics/block-noise-r3-20260918/`.

The DLL and matching PDB were installed in the usual MO2 mod at 16:12. Installed
hashes match the final Release build. DLL SHA-256:
`CCD753888FDC93B9DD8F23E49D5611473491556666DB67789B0BBF4419D6DA69`.
Previous binaries, camera preferences/presets, the unchanged TDM INI and test
evidence are backed up in `build/block-noise-r3-backup-20260918-161230/`.
Deployment verified all preferences unchanged. The accepted DamageReaction.h
also matches its pre-change SHA-256. The final build and whitespace check passed.

The user subsequently confirmed the R3 reset is fixed. Occasional block jerks
remained and led to the non-damaging-effect investigation above.

## Unreleased first-person damage travel and R3 preference (September 18, 2026)

The 15:06 first-person body-response experiment below was rejected in gameplay:
the requested change was greater travel, and stretching out the motion made it
feel worse. Discrete first-person hits now use the earlier quick impact and
recovery curve. Pitch/yaw amplitude and their overlap ceilings are doubled;
roll retains its earlier amplitude and limit. Fine texture returns at its
previous level. Third-person tuning, creature/centurion classification, the
continuous-magic envelope and its gain, health independence and the single
Intensity slider per view remain intact. Delayed contacts still start at rest.

At Intensity 3 and 240 FPS, the model gives a frontal greatsword peak of
11.241 degrees and side yaw of 10.830 degrees, compared with about 5.62 and
5.42 degrees before. First-person timing checks require the greatsword peak
within 50 ms and all impact families to settle within 500 ms; the 30/60/144/240
FPS tests require the increased travel rather than a prolonged deflection.
Direction through the rendered-camera axis mapping, creature attacks, blocked
and power attacks, overlapping impacts, delayed dispatch and smooth streams
are also covered. These are offline motion checks, not gameplay recordings.

The initial R3 investigation identified TDM's failed-target-lock fallback, but
incorrectly treated it as the whole cause. The subsequent native-handler fix
above addresses Skyrim's own reset path. The installed default INI enables
`bResetCameraWithTargetLock`; the user's MCM override previously omitted it.
This installation set `[TargetLock] bResetCameraWithTargetLock = 0` in
`C:/SkyrimMo2/overwrite/MCM/Settings/TrueDirectionalMovement.ini`. TDM reads this
override after its default INI. No DDC input hook or key assignment changes:
target lock/unlock, R3 hold-to-change-POV, keyboard POV and inventory inspect
retain their handlers. This is a local preference change, not a shipped DDC
feature. The original INI is included in the installation backup.

The local TDM `DirectionalMovementHandler.cpp` identifies the failed-lock reset
branch. Upstream [TogglePOVHook](https://github.com/ersh1/TrueDirectionalMovement/blob/master/src/Hooks.cpp)
and [settings loading](https://github.com/ersh1/TrueDirectionalMovement/blob/master/src/Settings.cpp)
confirm short taps stay in the target-lock path and the MCM override is read
after defaults. Evidence is under `build/diagnostics/damage-travel-r3-20260918/`.
The Release build and all eight relevant checks passed. The DLL/PDB were
installed at 15:31; their hashes match the build. DLL SHA-256:
`1A6BA349871F57A3F0FC53AD4689D8084EE9426338E3F10AF467E08E110AF14E`.
Previous binaries, unchanged camera preferences/presets and the original TDM
INI are backed up in `build/damage-travel-r3-backup-20260918-153143/`.
The TDM verification confirms the effective camera-reset value is false and
the rest of the user INI is unchanged. Installation hashes and check results
are recorded with the evidence.
The user subsequently accepted the first-person damage feel. R3 acceptance is
superseded by the native-handler work above.

## Unreleased Damage Reaction body response and creature attacks (September 18, 2026)

In-game feedback on the 14:25 build was that first-person impacts still felt
like jerks. That version reached its greatsword peak in about 38 ms. First-person
discrete impacts now use a quintic deflection and longer recovery, with zero
velocity and acceleration at the start, peak and finish. Roll follows slightly
later to form a recovering lean. The sharp counter-kick and fine impact noise
are removed from this path. Third-person impacts retain their faster envelope;
continuous magic retains its existing smooth pressure and elemental texture.
Fresh, delayed contacts start at zero displacement instead of appearing already
partway through the motion. Intensity remains the only exposed setting.

The compiled before/after comparison at Intensity 3 and 240 FPS gives:

| First-person frontal impact | Peak (degrees) | Time to peak (ms) | Maximum speed (degrees/second) |
| --- | --- | --- | --- |
| Previous greatsword | 5.619 | 38 | 282.8 |
| New greatsword | 5.622 | 138 | 94.8 |
| New one-handed blade | 4.723 | 108 | 92.3 |
| Giant club | 6.133 | 200 | 82.4 |
| Centurion axe | 5.885 | 138 | 105.1 |
| Centurion hammer | 6.073 | 192 | 82.9 |

These are motion-model measurements, not recordings of the game renderer.
Creature classification now distinguishes giant clubs, hand swipes and stomps;
centurion axe and hammer hands; bites, claws and heavy creature attacks; and
dragon bite, wing and tail attacks. Ordinary weapons retain their weapon family;
heavy creatures' natural-weapon proxies retain their creature response. The
installed Dragonborn.esm authors DLC2CrBenthicLurkerWeapon as a two-handed sword,
which is covered explicitly by the classification regression.
Race keywords, editor IDs and skeleton paths cover standard and recognizable
mod variants. Unknown variants receive a generic reaction. Standard melee,
projectile and harmful-magic events are accepted across enemy types; attacks
that bypass those engine notifications cannot be guaranteed. Shield bashes can
name an armor form and are now accepted. Duplicate discrete notifications from
one source within 8 ms coalesce; a new hit or another attacker remains separate.

Classification was checked against the installed Skyrim.esm race ATKE records
and vanilla SteamCenturion.nif / steambehavior.hkx assets. The centurion model's
left forearm carries the axe; its right forearm carries the hammer. Shared
Chop/Slash/Stab attack events use mirrored left/right clips, so the active clip
node name identifies the hand. Known chops and sweeps also supply a strike
direction in the attacker's frame. Unidentified motions use source direction.
No native hook is added: the existing clip Activate/Update/Deactivate observers
copy bounded attack-name snapshots for nearby centurion graphs. Worker callbacks
never inspect a cached actor or graph. Scopes are rebuilt on the main thread;
stale clips, scope changes, pauses, loads and camera suppression clear or expire
the context. An unknown hand gets a generic heavy mechanical response.

DamageReactionChecks covers the exact vanilla event/node names, race/skeleton
variants, unrelated centurions and giant spiders, mirrored sweeps, downward
hammer chops, clip refresh/expiry/reuse/reset and duplicate impact coalescing.
First-person timing checks require a sustained displacement, bound peak speed
at 30/60/144/240 FPS, reject an opposite recovery kick and test delayed dispatch.
Existing stream cadence/direction, weapon weight, blocking, intensity, reset
and camera-axis checks also pass. No health value or damage amount scales motion.

Evidence, private asset extracts and the compiled comparison are under
`build/diagnostics/damage-reaction-feel-20260918/`. The first full build encountered
MSVC C1090 while writing compiler debug symbols; the serial retry and final
incremental build succeeded. All eight relevant checks and the whitespace check
passed. The DLL and matching PDB were installed in the usual MO2 mod at 15:06.
DLL SHA-256:
`1CFB536D8B62B1266B95DA01B334D3366B7EB7751C6B59E1B3E668782902AAE5`.
Installed hashes match the Release build. The previous binaries, unchanged
preferences/presets, test logs and deployment manifest are backed up in
`build/damage-reaction-feel-backup-20260918-150608/`. Private game asset extracts
and the comparison build remain in the diagnostic directory.
In-game feel and creature-hand acceptance remain pending. Retest first-person
normal/power/blocked weapon impacts, giant club/swipe/stomp and centurion
left/right chop/slash/stab/power attacks, then repeat while moving the view,
under ongoing magic, after POV changes and after loading a preset.

## Unreleased Damage Reaction direction revision (September 18, 2026)

The previous first-person response limited pitch to 1.72 degrees and yaw to
1.32 degrees, making even maximum intensity feel small. Directional impulse
travel now uses 0.050 radians in first person and 0.045 in third person before
attack weighting, intensity and soft limiting. The new overlap ceilings allow
heavy and power attacks to remain distinct. First-person roll remains smaller
than third-person roll. Fine impact texture retains its prior amplitude.

The camera-ray regression also exposed a reversed lateral cue: negative yaw
turns the view toward a source on the right. Side hits now deflect the rendered
view away from the source. Tests follow both the root's third-person rotation
composition and the first-person NiCamera child mapping, across headings,
pitch and bank, with front/rear/left/right, diagonal and elevated sources.
The new regression fails against the previous implementation and passes with
the corrected direction and travel.

Measured greatsword peaks at 240 FPS, in degrees:

| View | Intensity | Front pitch before / after | Side yaw magnitude before / after |
| --- | --- | --- | --- |
| First person | 1 | 0.78 / 2.80 | 0.82 / 3.05 |
| First person | 3 | 1.54 / 5.62 | 1.28 / 5.41 |
| Third person | 1 | 1.35 / 2.57 | 1.41 / 2.84 |
| Third person | 3 | 2.77 / 5.69 | 2.23 / 5.65 |

Continuous magic has stronger directional pressure with proportionally slower
tracking to retain a smooth change of direction. Its established elemental
sway/ripple amplitude and continuous phase remain intact. Duplicate and uneven
ticks still do not restart motion. DamageReactionChecks passes the existing
30/60/144/240 FPS stream checks, along with new checks for both views' direction
reversal at intensities 1 and 3 and a discrete blade hit during ongoing poison.
The reported session contained both poison ticks and a weapon impact. Diagnostic
quotas now separate continuous/discrete contacts and views, so magic ticks cannot
hide later weapon contacts; logs include the resolved recoil axes.

Health independence, Intensity-only settings and preset values are unchanged.
Evidence is in `build/diagnostics/damage-reaction-direction-20260918/`.
The Release build, whitespace check and all eight relevant checks passed:
damage reactions, melee/archery/magic hit shake, camera quality, runtime layout,
preset compatibility and spell trajectories. The DLL and matching PDB were
installed in the usual MO2 mod at 14:25. DLL SHA-256:
`3DC9DF122B8CE8F90101228B74B4FC7D7B8F6ACEA223748B9CBC868E7193C11E`.
Installed hashes match the build. Previous binaries, unchanged preferences and
presets, logs and the deployment manifest are preserved in
`build/damage-reaction-direction-backup-20260918-142529/`.
In-game assessment of the stronger recoil remains pending; compare frontal and
side hits in both views, greatswords versus lighter weapons, power attacks,
blocking and smooth streams while turning or resuming contact.

## Unreleased Projectile Tracing view and impact revision (September 18, 2026)

Projectile Tracing now uses the same Third Person / First Person section-header
helper as Cinematic Effects, with the medium slider scale, 1.4 body text and
2.1 checkboxes used by Death and Ragdoll Camera. Each view has its own enable
toggles, reticle size/thickness and sneak-eye X/Y controls. Either tracing
toggle exposes the sneak-eye sliders. Runtime HUD positioning, reticle drawing,
spell capture and projectile aim gates use the active view's settings. Trace
buffers and pending release state clear on view/toggle changes, preset loads
and game loads; an inactive view cannot render the previous view's traces.

Preset format 11 retains the existing third-person keys and adds independent
first-person values. Format 10 and earlier seed both views from their old
shared tuning once. The new preset checks cover independent amounts/toggles,
legacy migration, disabled appearance values, sparse first-person off settings,
repeat-save stability, invalid input and resets.

Previously a fired spell kept a static predicted endpoint and expired on its
estimated arrival clock. An enemy moving away could leave a false impact
marker behind while the actual missile kept flying. Zero-gravity spells also
inherited a charge-preview endpoint, bypassing the native first-person launch
direction. All ordinary fired missiles now retain a weak native projectile
handle, record actual flight and refresh the remaining trajectory every 50 ms
of game time. Only the existing native missile contact observer can confirm a
final impact point and start the settling timer. Unresolved or removed missiles
leave no invented impact. No new native hook is installed, and first-person
projectile direction remains engine-controlled. Observed history and render
samples are bounded; the displayed path retains the travelled segment and its
fading trail.

SpellTrajectoryChecks reproduces an enemy initially intersecting a shot at
0.4 seconds, moving aside, and the projectile continuing to a wall at 1.2
seconds. It verifies that predicted arrival never confirms or expires the
shot, the revised endpoint reaches the wall, and only a real contact settles.
It also covers same-frame impacts, loss without impact and long flight history,
alongside the existing native gravity, first-person direction and collision
filter regression cases. Native contact positions are logged for the first
16 observed impacts to support the in-game retest.

Gameplay and visual acceptance remain pending. Test bow and spell/staff toggles
independently in both views, sneak-eye placement with only spells enabled,
stationary and strafing enemies, walls behind missed enemies, both casting
hands, zero-gravity and gravity-affected spells, rapid casts, POV changes and
preset reloads. Previewed future collisions remain estimates of the current
scene; actual flight and confirmed contacts determine the fired-shot result.

The Release build and all eight relevant checks passed: preset compatibility,
spell trajectories, runtime layout, camera quality, incoming damage reaction,
and melee/archery/magic hit shake. The DLL and matching PDB were installed in
the usual MO2 mod at 13:53. DLL SHA-256:
`AE7F1C708E94D7CC26F314F4DE2F16D25274020FD6E953EA43C9B5A3EC71DE34`.
The replaced binaries, current preferences/presets, build/test logs and manifest
are in `build/projectile-tracing-views-backup-20260918-135327/`. Installed hashes
match the build; existing preference and preset files are unchanged. The saved
format-10 files remain available in that backup for the previous build.

The subsequent scaling correction uses the Death/Ragdoll medium slider scale,
1.4 body text and 2.1 checkboxes, retaining the Third Person / First Person
separators. The Release build and whitespace check passed. This UI-only DLL
and matching PDB were installed at 14:02. DLL SHA-256:
`A5E811E0B89346D999D01967915DC7BB356B9C6351C90FC478B82EC51996CABE`.
The previous binaries and unchanged preferences/presets are backed up in
`build/projectile-tracing-scale-backup-20260918-140232/`; installed hashes match
the build. In-game visual acceptance remains pending.

## Unreleased Damage Reaction build (September 18, 2026)

Extras > Cinematic Effects > Damage Reaction adds separate third-person and
first-person Intensity sliders. Motion and recovery are tuned internally. Both views
start at Intensity 0; use 1 to try the baseline response. Preset
format 10 saves each view's intensity. Experimental Recovery, Direction and
Texture keys are ignored on load and omitted on save. Earlier presets keep
the effect disabled.

The current revision uses incoming weapon hit events directly, including blocked
contacts. Total/current health and damage amount are not inputs to the motion
model. Harmful magic applications supply spell type and streaming cadence;
their signed amount only filters out healing and beneficial-effect removal.
Arrow and missile contacts supply incoming velocity, with the source's position
as the fallback direction. One-handed blades/blunt weapons, two-handed blades/
blunt weapons, unarmed, arrows and elemental magic have separate motion shapes.
Unattributed directions use a neutral pitch cue. Scripted effects that emit no
weapon-hit or harmful-effect callback cannot generate a reaction.

Responses use a smooth onset, controlled rebound and settling tail, weighted by
weapon/spell type, power attacks and blocking. Continuous magic maintains one
coherent envelope and texture phase per source: fire sways, frost applies slower
pressure and shock has a fine vibration. Ticks refresh contact and direction
without restarting the motion. Critically damped onset, direction tracking and
release preserve camera position and velocity through contact changes. A short
grace interval absorbs uneven callback timing. Overlapping effects have bounded rotation, with
tighter first-person yaw and roll limits. Pauses, dialogue, death, killmoves,
camera handoffs, long frame gaps and settings reloads clear pending reactions.
The effect uses the existing camera compositor and does not change damage.

Native hook interfaces were checked against the bundled CommonLib headers and
[FloatingDamageNG's hook documentation](https://github.com/alandtse/FloatingDamageNG/blob/main/src/Hooks.cpp).
Offline checks against unpacked Skyrim 1.5.97, 1.6.1170 and 1.7.104 executables
resolve the seven magic-effect vtable entries to executable image sections. Reports are
in `build/diagnostics/damage-reaction-strength-20260918/runtime/`. These checks neither execute game
code nor establish in-game compatibility with other installed hook providers.

The Release plugin builds. DamageReactionChecks covers mirrored directions,
camera-relative axes, weapon weight, every impact type in both views at
30/60/144/240 FPS, smooth onset, settling, time-step invariance, overlapping
impacts, stale callbacks, projectile attribution and reset generations. Checks
also require visible greatsword recoil at Intensity 1, useful range at 3,
stronger power attacks, lighter blocked hits and tick-count-independent magic.
Stream checks simulate three seconds of exposure followed by recovery at
30/60/144/240 FPS in both views, including duplicate and irregular callbacks,
continuous onset/phase/release, direction reversal, interrupted contact,
simultaneous sources and camera resets. They reject the previous repeated-jolt
pattern by bounding motion speed and requiring sustained pressure between ticks.
PresetCompatibilityChecks covers both intensity sliders, sparse defaults,
retired experimental keys, bounds, invalid input and older presets. Existing melee, archery, magic
hit-shake and camera-quality checks also pass.

The initial DLL and matching PDB were installed in the normal MO2 mod at 12:04. DLL
SHA-256: `4AF2F78CFDEA44F0B3869191FCB7DD55A09813B9AF2873FBF5696469C8A71BEE`.
The previous binaries, current preferences and presets, test logs and deployment
manifest are preserved in `build/damage-reaction-backup-20260918-120448/`.
Installed hashes match the build, and preferences/preset hashes remain unchanged.
The preset backup also preserves files readable by the previous format-9 build.

In-game acceptance is pending. Check front/rear/left/right melee and arrows;
compare swords, maces, fire, frost, shock and poison; test continuous damage,
rapid mixed hits, high-health characters and fully blocked weapon hits. Repeat in
both views and verify pause, POV switching and preset loading leave no old
reaction. Judge direction, attack distinction, aiming comfort and recovery
with Intensity 1 before trying the upper intensity range.

The initial build crashed when the author enabled Intensity 1. The September 18
12:08:51 report on Skyrim 1.6.1170 identifies the health read in `Snapshot`:
the implicit C++ ActorValueOwner cast used offset 0x98, while the native player
stores that interface at 0xB8. An ordinary magic-effect update reached the bad
virtual call as soon as capture became active. This was a DDC health-access bug;
the report does not establish a conflict with the other mods on the stack.

The 12:15 fix routes all five incoming-damage health reads through a shared
runtime-aware accessor. A regression fixture plants an engine-style vtable at
the native subobject offset and calls that production reader for eight runtime
versions. It fails with the original reader and passes with the corrected one.
The offline executable check now also compares the accessor with the player's
native RTTI complete-object locator: 0xB0 on 1.5.97, 0xB8 on 1.6.1170 and 1.7.104.
The health getter resolves to executable code in each image. The Release build,
runtime layout, damage motion and preset checks pass. Crash logs, before/after
evidence and image reports are in
`build/diagnostics/damage-reaction-crash-20260918/`. These checks do not replace
an in-game retest of enabling the effect and taking damage.

The corrected DLL and matching PDB were installed in MO2 at 12:15. DLL SHA-256:
`B0A5EE8510DDDE42AE0766A5645242B774F62DCD062361CC5531E3E82DCEB4FA`.
The replaced binaries, preferences, presets, crash/test evidence and deployment
manifest are backed up in
`build/damage-reaction-health-fix-backup-20260918-121528/`. Installed hashes match
the build; existing preferences and presets are unchanged. In-game acceptance
of this correction is pending.

The author then reported negligible greatsword feedback and clarified that
health must not influence the effect (their character has approximately 100,000
health). The current revision removes the health reader, health-notification
hook, health/damage fields and health-fraction severity curve entirely. A
TESHitEvent now supplies a complete weapon contact without waiting for health
to change. Greatswords and battleaxes use a heavier blade shape; warhammers use
a heavier blunt shape. Power attacks receive more recoil, and blocked contacts
retain a lighter response even with no health loss. No preset values are changed.

The 12:58 revision used a fixed type-dependent pulse rather than accumulated
damage. The first 48 accepted contacts and first 16 activation transitions are
logged at Info level so the next in-game check can confirm routing without
enabling every verbose camera diagnostic. The removed health-reader regression
fixture is superseded by direct weapon-contact and tick-independent motion
checks; its historical before/after evidence remains in the crash backup.

The Release build and seven relevant checks pass, along with offline preflight
against 1.5.97, 1.6.1170 and 1.7.104. The DLL and matching PDB were installed at
12:58. DLL SHA-256:
`D88B239AA06A7A2A358ACA17F3620F94DF251EB023F74845AAF8B5DE1801A3B4`.
The previous binaries, unchanged preferences/presets and verification evidence
are preserved in `build/damage-reaction-impact-backup-20260918-125839/`.
In-game acceptance of the health-independent revision is pending.

The author subsequently reported repeated jolts from magic streams. The saved
game log confirms sustained shock contacts arriving roughly every frame. The
old mixer restarted a discrete reaction every 0.18 seconds; the 13:21 revision
replaces that path with continuous directional pressure and coherent elemental
motion. Refreshes preserve phase, displacement and velocity, including when
contact resumes during release. Coalescing retains the freshest source direction.
Recovery, Direction and Texture controls were removed at the author's request;
each view now exposes and stores only Intensity, with the motion tuned internally.

The Release build and all seven relevant checks pass for this revision. Native
hooks are unchanged from the previously checked health-independent build.
The updated DLL and matching PDB were installed in MO2 at 13:21. DLL SHA-256:
`A896027C44B4C462D45A122E3B546728491BD20CC99576F1D8355EFA6E125A8A`.
The replaced binaries, current preferences/presets, build/test logs and deployment
manifest are in `build/damage-reaction-stream-backup-20260918-132112/`.
Installed hashes match the build, and all preference/preset files remain unchanged.
Stream feel and the simplified menu still need in-game acceptance.

The author subsequently reported that the revised effect seems good, before
moving on to the Projectile Tracing changes documented above.

## Unreleased controller hint build (September 16, 2026)

DDC sections replace the existing framework footer with hints for DDC's current
controller mode: sidebar, page, popup, slider adjustment, binding capture or text
entry. Copy, Paste and Quick Tune labels read the current settings each frame.
Keyboard keys are identified separately from controller buttons, and PlayStation
button names follow the engine's controller type. The footer uses the framework's
existing space and adds no widgets, layout height or scrolling. Other mods and
framework settings retain the framework's hints and input behavior.

The Release plugin builds and all 33 main checks pass. All four controller
compatibility/layout checks pass with the production hint renderer against the
framework's 3.15 ImGui implementation, with and without legacy key arrays. They
cover binding changes, cleared bindings, context changes, narrow layout, unchanged
input/style state and immediate handoff to other windows. The installed framework
provides all twelve API exports used by the footer. An offline preview using its
Skyrim font at 32 pixels was inspected for sidebar, page, slider and narrow layouts;
in-game acceptance of this footer build remains pending.

The DLL and matching PDB were installed in the normal MO2 mod at 21:59. DLL
SHA-256: `C93C069117B9F619D183DB2D86D7ADBF0704E4CDCC1FBDF0A8796E5A9A9D52C9`.
The previous DLL/PDB, local preferences and deployment manifest are preserved in
`build/controller-hints-backup-20260916-215949/`. The framework DLL, framework INI
and local preferences retain their pre-installation hashes.

The 22:20 readability revision keeps each binding and explanation inside one
outlined group, with a divider for inline pairs and a minimum gap between groups.
The renderer chooses inline groups, stacked labels or two rows to maximize text
size within the same footer rectangle. Labels now explain returning to sections,
keeping/canceling slider changes and the bumpers' 10x steps. Copy/paste identify
hovered entries versus entire tabs, and stick-click bindings are distinct from
stick movement used for scrolling. Custom binding labels still update live.

The Release build and all four expanded controller checks pass. Geometry checks
require both labels inside their shared group, no overlap and minimum spacing.
Previews with the installed Skyrim font and Modern theme colors were inspected
at wide and narrow widths. In-game appearance acceptance remains pending.

Revision DLL SHA-256:
`568139CE30D725C3CB93F3658D06706BBBFB2A206D3D5F810F5675B396893FD6`.
It and the matching PDB are installed in MO2; the previous DLL/PDB, preferences
and deployment manifest are in
`build/controller-hints-readability-backup-20260916-222013/`. Framework DLL/INI
and local-preference hashes are unchanged by this installation.

The author found the grouped presentation better, but reported clipboard hints
over Reset/sliders and generic entry labels over override buttons. The 22:40
revision resolves clipboard hints from the current controller item, current
frame and active popup layer. Reset, sliders and other controls without a
clipboard target omit those hints. Supported targets identify transition,
location, weapon and enemy overrides, tabs, indoor/outdoor settings and other
specialized contents. Clipboard payloads and bindings are unchanged.

The Release build and all four controller checks pass, including stale/mouse
hover, group-inherited IDs, hidden/removed targets and popup-layer cases. The
existing grouped layout was inspected with longer transition-override labels
using the installed font; it keeps the same footer size. In-game acceptance of
this revision remains pending.

Revision DLL SHA-256:
`1206B484DCFEDECBCD11A0670CE3FF074E6184F825510B9CEEB01843DA1579E6`.
It and the matching PDB are installed in MO2. The previous DLL/PDB, preferences
and deployment manifest are preserved in
`build/controller-hints-context-backup-20260916-224045/`. Framework DLL/INI
and local-preference hashes are unchanged by this installation.

The 22:49 revision matches clipboard subjects to the controls' capitalization
and wording, including Transition Override, Location Override, Weapon Type
Overrides and Enemy Override. Paste is hidden while the menu clipboard is empty;
Copy remains available. The Release build and all four controller checks pass,
including empty, populated and cleared clipboard states. In-game acceptance is
pending.

Revision DLL SHA-256:
`D4FE4437DB88D8907133FADA7CCAB12665279B8EF33E0A74ADC923F8D5A64EA0`.
The DLL and matching PDB are installed in MO2, with previous files, preferences
and the deployment manifest in
`build/controller-hints-paste-backup-20260916-224938/`. Framework DLL/INI
and local-preference hashes remain unchanged.

The 23:02 revision assigns fixed slots to navigation, confirm, back, secondary
controls, scrolling, Copy, Paste and Quick Tune. All menu modes and clipboard
labels contribute to the reserved widths, including currently hidden actions.
Hover and mode changes preserve group bounds, key/action origins and text size;
unavailable actions leave their slots blank. Window, font or binding changes
can still adapt the layout within the existing footer dimensions.

The Release build and all four controller checks pass. Position checks cover
every clipboard label and menu mode at four widths with Xbox/PlayStation names
and long bindings. The same checks pass with the installed Skyrim font at 32
pixels; previews of entries, absent clipboard hints, overrides, sliders and
narrow layouts were inspected. In-game acceptance remains pending.

Revision DLL SHA-256:
`478CDE15E27FA31C0CF81786D094782050FBA606D856257DE291B1E2391244E4`.
The DLL and matching PDB are installed in MO2. Previous files, preferences and
the deployment manifest are in
`build/controller-hints-stable-backup-20260916-230248/`. Framework DLL/INI
and local-preference hashes remain unchanged.

The author's 23:08 Steam screenshot showed excessive padding inside short
binding/caption pairs. The 23:19 revision keeps fixed slot origins and text size
but draws each box only around its current contents. It also validates clipboard
hint targets against the current widget registry rather than the cursor's old
rectangle. An active row keeps its hints while scrolling brings it into view;
removed/disabled controls and unrelated groups still cannot publish a target.
Target Lock's General tab has no clipboard target or context menu. Controller
clipboard polling rejects a previous target after the cursor moves elsewhere.

The Release build, all four controller checks and EntryClipboardChecks pass.
A native ImGui school-list fixture exercises scrolling in both directions,
offscreen selections and changed rectangles without dropping clipboard hints.
Geometry checks require snug boxes and binding/caption spacing as well as stable
slot origins. Installed-font previews were inspected; in-game acceptance remains
pending.

Revision DLL SHA-256:
`5FC384718E82A5D60175FEC4A57A8045864BBA335D9DA890DD02F59BBEB7EFC6`.
The DLL and matching PDB are installed in MO2. Previous files, preferences and
the deployment manifest are in
`build/controller-hints-scroll-backup-20260916-231928/`. Framework DLL/INI
and local-preference hashes remain unchanged.

## Unreleased Target Lock bias build (September 16, 2026)

Height, Zoom and FOV Bias now accompany Pitch Bias in the same Target Lock
transition popups, Quick Tune and nested profiles. The order is Aim, Height,
Zoom, FOV, Pitch. Each new channel defaults to disabled and zero, uses the
existing proximity curve and smoothing, and releases smoothly with target lock.
Height only translates vertically; Zoom retains the existing distance limits
and collision path. Preset format 9 stores the new channels and retains earlier
formats. The main popup retains its no-scroll flags; the Quick Tune companion
grows for twelve rows and fits the rows to a short viewport without scrolling.

The Release plugin builds, all 33 main checks pass, and the four existing
controller compatibility checks pass. The production bias motion checks cover
channel independence, signed amounts, distance falloff, target changes, release
and invalid values. The production codec checks cover nested profiles, disabled
tuning, explicit neutral enemy overrides, older presets and stable rewrites.
The author subsequently accepted this build before requesting the controller
footer changes above.

The DLL and matching PDB were installed in the normal MO2 mod at 21:26. DLL
SHA-256: `3F84AE66FB06B92BA53CD2D988EC25853F8C454A4B94AD81C81F19157E8CF27A`.
The prior DLL/PDB, local preferences and deployment manifest are preserved under
`build/target-lock-bias-backup-20260916-212602/`. The framework DLL and opening
hotkey configuration were not changed by this feature.

## Unreleased controller compatibility build (September 16, 2026)

The development build adds DDC-scoped controller capture for SKSE Menu
Framework 3.15. It retains DDC's custom page controls and explicit focus transfer
to and from the section tree. Selecting a section stays in the list; D-pad Right
enters its controls and displays the cursor. Quick Tune yields while the main panel is open.
The installed framework DLL is not modified.

The Release plugin builds and all 33 existing checks pass. Four additional
[controller compatibility checks](tests/menu-controller/README.md) pass against
the upstream 3.15 navigation implementation with both ImGui IO layouts. These
are headless checks; the DDC menu, XInput path and installed DLL combination
still need in-game acceptance. This evidence is separate from the released
versions below.

Initial candidate DLL SHA-256:
`D8CC72685E00CD540DBD115A53D293BD3E37904D247157D0069E5958C6AC0064`.
The DLL and matching PDB were installed in the normal MO2 Diet Dr Camera mod.
The previous DLL and deployment manifest are preserved under
`build/controller-compatibility-backup-20260916-201018/`.

The author's 20:16 Steam screenshot showed a blue box over the Third Person
tabs. The headless fixture reproduced it after clicking a tab with the mouse
and returning to controller input: the framework's foreground highlight uses
the lingering native navigation ID even on a NoNav tab. The follow-up clears
that ID only when DDC owns page capture. Sidebar and other-mod highlights remain,
and native text entry retains its real focus. Both ImGui layouts pass the new
draw-list and text-editing checks.

The author also reported that LB still opened the menu despite rebinding it.
The post-update local framework INI contained `ToggleKeyGamePad = LB` and
`ToggleModeGamePad = DOUBLEPRESS` separately from `ToggleKey = KP_3`. Per the
author's instruction, its controller toggle mode is now `OFF`; Keypad 3 remains
enabled. The prior INI is backed up under
`build/controller-fix-backup-20260916-202134/`. This is a local configuration
change, not a binding override shipped by DDC.

The author clarified that LB was not their toggle before the framework update.
The INI backup above was taken after that update and does not establish the
previous binding. Both downloaded 3.14.1 and 3.15 archives ship LB/double-press
as the controller default. An installation reset is possible, but no pre-update
user INI was recovered to confirm it; the cause of the binding change remains
unverified.

The blue-box follow-up DLL and matching PDB were installed in the normal MO2 mod. DLL
SHA-256: `2A684211C49A3149AA8954D13571977E7E336CC5B3FCF4985E64289AB2892366`.
The previous DLL/PDB and the deployment manifest share the INI backup folder
above. The Release build, four compatibility checks and five related menu checks
pass. The author subsequently reported that the cursor seems fixed and requested
removing automatic entry into the right pane. Selection and reselection now keep
controller focus in the section list until D-pad Right, matching the requested
handoff. The tests cover both mouse and controller selection and verify focus
on every frame. Mouse text editing and keyboard clipboard shortcuts remain
available while controller focus is in the sidebar; controller clipboard actions
pause there. The author has not yet confirmed the disabled LB binding in game.

The manual-entry build is installed in the normal MO2 mod with matching symbols.
DLL SHA-256: `FB90FF2F0D7E01D0B1E8F6557A1F5912DB8854E1307CE0B3A33F0467C8DB4B83`.
The previous DLL/PDB and deployment manifest are preserved under
`build/controller-manual-entry-backup-20260916-204609/`. The Release build and
all nine relevant checks pass; manual section entry still needs in-game confirmation.

### Keyboard navigation follow-up (September 17)

The keyboard footer uses the framework's Font Awesome Solid arrow glyphs with
"Move cursor" and shows Enter for selection. Left at the page edge returns to the section list,
including when the page cursor has not been placed, and restores its visible
focus. Right enters the page. Enter and Numpad Enter select sections and activate
page controls without repeating while held; text editing and binding capture
retain their own Enter handling.

Menu hotkey polling and binding capture now preserve extended DirectInput key
identities. Down Arrow (`0xD0`) no longer matches Numpad 2 (`0x50`), preventing an
unrelated arrow press from opening Quick Tune. Existing stored bindings are kept.

`tools/check-menu-controller.ps1` covers sidebar round trips, both Enter keys,
held Enter, text/binding isolation, other-mod focus, both supported ImGui IO
layouts, symbol glyphs, footer fitting, and navigation/numpad key separation.
The footer was also rendered and inspected with the framework's solid icon
font. These checks do not replace in-game confirmation of the new behavior.

All five compatibility checks and the Release build pass. The DLL and matching
PDB are installed in the normal MO2 mod and their hashes match the build outputs.
DLL SHA-256: `E0FE5E0529FC8A62DB071EFBB273C0308B811DD2F7579DEAFFFE25E3CFEE2D0A`.
The previous binaries, edited input sources, and deployment manifest are under
`build/keyboard-input-fix-backup-20260917/`.

### Keyboard footer seam follow-up (September 17)

Steam screenshots `20260917212356_1.jpg` and `20260917212403_1.jpg` showed
the keyboard-only seam after the framework update to 3.18. The fallback footer
replaces ImGui's draw list, but its inherited window border still inset the
draw-command clip rectangle. The Modern theme's 3px border therefore clipped
away the background at the menu edge. The fallback now begins without a native
border, then restores the theme thickness for its own border drawing.

The clip-bound regression failed on the previous build in both ImGui IO layouts.
All five checks pass after the fix, including border widths 0, 1, 3, and 6.
The Modern theme preview now applies actual draw-command clipping and shows no
transparent seam. The installed framework provides all 16 checked footer API
exports. In-game acceptance of this correction remains pending.

The Release DLL and PDB are installed in the normal MO2 mod with matching hashes.
DLL SHA-256: `6D09072E9120296AD2E2F6C66733005B8F74ADA37CD4AE63A57317F17D9FC27C`.
Previous binaries, source snapshots, and the deployment manifest are under
`build/keyboard-footer-clip-backup-20260917/`.

### Fixed outer menu dimensions (September 17)

The external keyboard strip is removed. While a DDC section is selected,
`MenuHintBarLayout` reserves the controller footer's height before ImGui begins
the framework's tree and page panes. Keyboard hints use a child window inside
that same reserved area. ImGui computes the panes' clipping, scrollbars and hit
regions from their actual sizes; the main window's position and size are kept.
Pane size conditions are restored before DDC renders, or at frame end when
another mod was selected.

All five compatibility checks pass. They now compare the complete pane/footer
rectangles on the first frame of keyboard/controller switches, exercise shrinking
and growing the panel, scroll and click each pane's bottom control, check keyboard
popup focus, and verify that other mods regain their full content area. Modern
theme previews with the Futura Condensed font and actual draw-command clipping
show identical outer bounds with both input devices. In-game confirmation is
pending; the tests use the pinned framework source and both supported IO layouts.

The Release DLL and matching PDB are deployed to the normal MO2 mod.
DLL SHA-256: `12A61E4ADED064FEBFBA759ACB8E4BC33BC4F08F58E5E9675118CF9E0B5E488D`.
Backup and deployment record: `build/keyboard-footer-layout-backup-20260917-214604/`.

### Location override hints (September 17)

All six location override popup layouts now keep the contextual hint bar inside
the popup, beneath Reset All and Done. The list and settings panes reserve its
height, so keyboard/controller switches retain the same popup and footer bounds.
The footer uses the main menu's font size and current bindings, selection,
clipboard availability and editing mode. Closing the popup restores the main
menu footer.

All five menu compatibility checks pass, including both ImGui IO layouts. The
new cases cover popup ownership, first-frame device switches, contextual labels,
scrolling to and clicking the last setting, Reset All, Done and main-footer
restoration. ABI checks include the window font-scale field. Modern theme
previews using Futura Condensed and actual draw-command clipping show the hints
below the buttons in both input modes. The installed framework provides all 24
checked footer API exports. In-game confirmation remains pending.

The Release DLL and matching PDB are installed in the normal MO2 mod with verified
hashes. DLL SHA-256:
`2C87C933C8147FEDDB9A4E35107668E99E8F8CAF06D0504D6F9F6D94C587198D`.
Backup and deployment record: `build/location-hints-backup-20260917-220518/`.

### Specific NPC actions and cinematic clipboard labels (September 18)

Reviewed Steam screenshots `20260918111938_1.jpg` and `20260918112038_1.jpg`.
Specific NPCs now hides Remove NPC until an NPC is selected and Delete Preset
until a valid preset belonging to that NPC is selected. Hidden controls reserve
their footer space without appearing or joining keyboard/controller navigation.
Cinematic creature-shake entries now use Copy Entry and Paste Entry in the hint
bar for both keyboard and controller input.

The Release build and all four native menu capture/navigation checks pass across
both ImGui IO layouts. In-game confirmation remains pending.

The Release DLL and PDB are installed in the normal MO2 mod with verified hashes.
DLL SHA-256:
`3A716F141A344694AEDE1582510CACD3C8472C908EB9D62E9ACF9B16095E7B3A`.
Backup and deployment record: `build/npc-actions-copy-entry-backup-20260918-112613/`.

### Specific Animations click-to-bind (September 18)

Activating a library animation with the mouse, keyboard or controller now binds
it immediately and opens its editor. Bound animations disappear from Browse,
Recent and search results; they remain in the Bound Animations column. Removing
a binding returns it to the picker. Browse and Recent share this behavior across
Third Person, Target Lock and Camera Noise.

The available-file and folder counts exclude bound animations, and empty folders
are omitted. The browser refreshes when binding paths change, including preset
switches that replace bindings while keeping the same entry count.

The Bind/Select Bound Animation button and selected-animation caption are
removed. The library fills the freed space. First-person arms animations and replacement
animations without OAR remain unavailable, with the reason shown on hover.

The Release build and both native ImGui navigation checks pass. The updated
layout fixture covers sparse and dense lists, scrolling, return paths and four
UI scales, including moving from the taller library's bottom rows to Remove.
In-game confirmation of direct binding, hiding and restoring animations remains
pending.

The Release DLL and PDB are installed in the normal MO2 mod with verified hashes.
DLL SHA-256:
`8A01E8DF34356DA9DD06BDA632705C759D963EB99119C5B5FC98FBCA5458440D`.
Backup and deployment record: `build/animation-unbound-backup-20260918-111256/`.

### Specific Animations pane sizing (September 17)

The animation library now reserves the measured selected-caption height and one
button row instead of a fixed 185-pixel allowance. The bound list extends to the
same button row, aligning Remove with Bind/Select Bound Animation. Both columns
fill the available pane height, leaving only the normal window/table padding.
This shared layout applies to Third Person, Target Lock and Camera Noise.

The Release build passes. Native ImGui previews using the production sizing
calculations show aligned buttons and matching bottom padding for a selected
animation, empty selection, wrapped filename, first-person warning, Recent view
and half-scale UI. The selected, wrapped and smaller-scale renders were visually
reviewed. In-game confirmation remains pending. The reporting Steam screenshot
is `20260917222041_1.jpg`.

The Release DLL and PDB are installed in the normal MO2 mod with matching hashes.
DLL SHA-256: `A42018AA39F281C02A6016B65A6450BF5CE841367873C5B0CDD88A8AC555ED52`.
Backup and deployment record: `build/animation-pane-layout-backup-20260917-222409/`.

### Shared cursor navigation (September 17)

The reported Up-from-Remove jump is reproduced by a regression fixture. Shared
keyboard/controller navigation now records each control's visible column bounds,
so table columns remain distinct even when they share an ImGui window. Entering
a list uses the pane's boundary before selecting a row, allowing sparse lists to
be reached from their footer or an adjacent pane. Horizontal return paths retain
the originating row, including after menu translation/scaling. Physical window
bounds still determine scroll ownership.

Native ImGui fixtures also exposed partially clipped bottom rows being skipped
when entering a list, and fractional row overlap causing Down to skip the next
entry at some font scales. Crossing panes now uses visible item bounds, while
movement within a list retains offscreen neighbors for scrolling. Directional
edge comparisons allow ImGui's subpixel row overlap.

All eight relevant checks pass: the directional-navigation suite plus seven
framework checks covering both supported ImGui IO layouts. Coverage includes
footer/list round trips, sparse and dense columns, returning to the original row,
empty/disabled controls, long names, partial clipping, scroll position, inline
Reset buttons, popup layers, four native UI scales, 400 varied grids and a
2,000-row clipped list. Native fixtures call the production item-registration
helper. The Release build passes; in-game confirmation remains pending.

The Release DLL and PDB are deployed to the normal MO2 mod with matching hashes.
DLL SHA-256: `4F091A1805DD685485055A2F350A515A34CB2A5CC1A1F6F50E58A71126B5081E`.
Previous binaries, regression evidence and deployment record:
`build/animation-remove-nav-backup-20260917-224028/`.

## Released versions

Version 1.1.1 fixes Sheathed/Unsheathed whole-tab matching. Its regression checks
exercise both directions and environment separation. This patch changes no
runtime-hook or menu-loading code and has not received an additional in-game
tab-paste test. The live results and DLL hashes below describe 1.1.0 and its
earlier candidates.

The author approved version 1.1.0 for release on September 15, 2026 after
testing in the normal 1.6.1170 mod list. The original reporter's exact runtime
and mod combination remain unknown. A message ending in
`Hooks/RuntimeHooks.cpp(24)` identifies the shared failure handler.
Retain the complete message, `DietDrCamera.log`, Skyrim's ProductVersion,
SKSE version, Address Library package version, and the active plugin list.

## Current evidence (September 15, 2026)

| Executable | Released 1.0 hook sources | Candidate hook sources | Candidate gameplay |
| --- | --- | --- | --- |
| Steam 1.5.97.0 | Offline preflight passes | Offline preflight passes | Pending |
| Steam 1.6.1170.0 | Offline preflight passes | Offline preflight passes | Author-approved release; broader combinations remain untested |
| Steam 1.7.104.0 | Offline preflight passes | Offline preflight passes | Pending |
| Reporting user's exact version/mod combination | Not yet identified | Pending | Pending |
| GOG 1.6.659 / 1.6.1179 | Address/layout checks only | Executable unavailable here | Pending |

None of these three clean images reproduces the report with the released 1.0
hook sources. The baseline and candidate use identical images and the matching
released Address Library databases, including the actual format-5 database
from Address Library v13 for 1.7.104 (not a generated CSV fixture).

| Executable | Camera update | Furniture | Main UI drive | Dialogue decrement |
| --- | --- | --- | --- | --- |
| 1.5.97.0 | `+0x1A6` | `+0x295` | `+0x61A` | `+0x4F9` |
| 1.6.1170.0 | `+0x1A6` | `+0x2A2` | `+0xADF` | `+0x6E8` |
| 1.7.104.0 | `+0x1A6` | `+0x2A2` | `+0xAF1` | `+0x6E8` |

All three also pass UI job control-flow validation. The main-thread call moved
on 1.7.104; both the released and candidate scanners resolve its new position.
These checks exercise hook preparation, not hook installation inside a running
game or interactions with other plugins that modify those instructions.

Interactive testing on 1.5.97 with SKSE 2.0.20 and Menu Framework 3.14.1 found
a separate startup crash. The dependencies-only profile loaded game data and
remained open. Released 1.0 and the initial 1.1 candidate installed their hooks,
then crashed in the first UI-scale update: the menu SDK had retained a null
framework DLL handle from DDC's static initialization. Matching PDB crash logs
identify `EnsureMenuUiScale` through `MenuUI::TickUiScale` in the camera hook.
The fix resolves the module at API use and checks context readiness. A Windows
DLL fixture tests dependency loading after the consumer starts, including an
initially absent ImGui context. This does not establish the cause of the original
`RuntimeHooks.cpp(24)` report or complete gameplay acceptance.

The startup-fix candidate (`58868D76520DFDC6C2592C3123C0B3AAD57C53A6F4A6D7DF930627AA1BCE1CE1`)
passes all 33 automated checks. Its isolated 1.5.97 run reached DataLoaded,
registered the DDC menu, entered the main menu and subsequently initialized the
player camera and menu input. The previous startup crash did not recur during
this session, and the author quit normally (Steam process exit code 0).
The same DLL also reached DataLoaded and the main menu on 1.7.104 with SKSE
2.3.1, installed its hooks and registered its menu. These results establish
startup on those two clean runtime/dependency combinations.

The author's current test scope is startup checks on the isolated runtimes,
with new-feature acceptance in the normal 1.6.1170 mod list. A subsequent UI
refinement keeps typed slider values in place with cursor-style highlighting;
its visual acceptance is part of that main-list test. The older AE/GOG versions
listed in RUNTIME-SUPPORT.md have not all been launched here, and the exact
reporting user's runtime/mod combination is still unknown.

After that refinement, candidate
`E004ABAD2E60FA991014BA9ECF707066341488900432A5B97EC63EDBC5A99303`
passed startup on both isolated runtimes and in the normal 1.6.1170 mod list.
All reached DataLoaded and the main menu with DDC's hooks and menu installed;
the normal list also connected to TDM and chained its existing camera callback.
The isolated runs ended by controlled termination after reaching the menu.
The author tested features in 1.6.1170 and closed the game normally.

The author accepted Pitch Bias and requested removal of its explanatory UI text.
The subsequent slider revision fixes immediate first-click focus, uses yellow
selection, and fits the input to its digits plus the one-pixel caret. A private
interaction check runs the production widgets against ImGui 1.90.8 (the installed
Menu Framework version): selection and yellow rendering occur in the mouse-down
frame, typing replaces the selection, Enter/click-away commit, Escape cancels,
and switching between rows preserves both edits. Main and Quick Tune widgets
pass at 100%, 160% and 240% font scale. The author subsequently approved the
release after main-list testing and the final UI refinements. The final 1.1.0 DLL is
`5B3A303C9E3F7E900E2A06265411397131BFA18FCF276473AA4D89F5AC41D697`;
the September 15 23:08-23:10 main-list log records 1.6.1170 loading the plugin,
validating its hooks and entering gameplay. The isolated startup results above
identify the earlier DLL with the same runtime-hook and menu-loading fixes.
Approval does not establish gameplay coverage for every listed runtime.

The first return to the normal list exposed a test-isolation problem: 1.7.104
had rewritten the shared LocalAppData `ContentCatalog.txt` with UUID content
identifiers. The 1.6.1170 engine threw `invalid stoull argument` while parsing it.
Preserving that incompatible cache and removing it from the shared location
allowed the same candidate and mod list to start. Test launchers now snapshot
and restore this file, with separate runtime caches. Recovery checks cover a
pre-existing catalog, an originally absent catalog, and a failed launch.

## Obtain and preserve private fixtures

Use your own Steam/GOG entitlement. Keep game executables and data outside
public source control. Neither release archive includes Bethesda game files.
Do not replace the live installation to obtain a test executable.

Steam's console depot downloads go under
`steamapps/content/app_489830/depot_489833/`. On this machine,
`download_depot 489830 489833 2289561010626853674` returned Skyrim 1.5.97.0.
Its original executable SHA-256 is
`693E5A51EA2680119A68620BCF5080E81745549872B5D06BBD3F51131B67ABAB`.
Preserve that file under a versioned private filename before downloading another
manifest of the same depot, which uses the same content directory.
Verify each file's ProductVersion and hash after download.

On September 15, 2026, `download_depot 489830 489833` returned manifest
`4886117324142477814`, containing Skyrim 1.7.104.0. Its original executable
SHA-256 is `846EFCCF0C1374D71F892907F46549560F2FCB0A75CB87A3EED438BAA0F1402F`.
The matching `versionlib-1-7-104-0.bin` from the official
[Address Library v13 archive](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)
has SHA-256 `8AAB3DD251D135B849BD983F86A4A205C920FA3E81F8E30C0E63CCFEF9423842`.
All three executable copies and the extracted test database remain private.

[DepotDownloader](https://github.com/SteamRE/DepotDownloader#usage) is an
alternative with explicit app, depot, manifest, and output-directory options.
Authenticate directly with Steam; do not put account passwords in repository
files or command histories. Historical manifest availability is controlled by Steam.

An executable alone is sufficient for the instruction check below. It is not a
complete playable version. Gameplay tests need a separate full game directory
with matching data and engine dependencies, matching
[SKSE](https://skse.silverlock.org/), Address Library, and SKSE Menu Framework.
Use separate mod-manager instances/profiles with isolated INIs and test saves.
Never open a valuable existing save under the downgraded test setup.

## Offline production hook check

Build with `DDC_BUILD_CHECKS=ON` as described in [BUILDING.md](BUILDING.md).
Use a private executable copy whose code section is unpacked, plus the exact
version's Address Library database. The checker maps the PE sections and unwind
metadata and runs DDC's production `RuntimeHooks::Prepare()` implementation.
It does not run the executable's entry point or call any Skyrim code.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-runtime-image.ps1 -RuntimeImage C:\private-tests\SkyrimSE-1.5.97.exe.unpacked.exe -AddressLibrary C:\private-tests\version-1-5-97-0.bin
```

The report records file hashes, runtime, checker exit code, and `gameplay: not-run`.
Its JSON and log live under the ignored `build/diagnostics/runtime-tests/` folder.
The SE fixture's actual runtime is read through CommonLib's ProductVersion string
reader; its fixed numeric Windows version fields misleadingly say 1.0.0.0.

For a before/after comparison, extract a previous release's `RuntimeHooks.cpp`
and `RuntimePatchInspection.cpp` into a private directory. Configure with
`-DDDC_HOOK_BASELINE_DIR=C:/private-tests/old-hooks` and build
`RuntimeBaselineChecks`. Pass that executable to the script's `-Checker` option.
The game image and Address Library must be identical for both checks.

## Gameplay acceptance before an update

New cinematic features and Risa integration:

- With Crowd Modifier set to zero, verify the existing camera is unchanged.
  Test Zoom Intensity only, FOV Intensity only and both at 0.25, 1 and 3 against
  one, two, four, eight and twelve engaged enemies. Larger groups must still
  widen the view beyond four, with diminishing returns; higher intensity must
  increase the response and extend the group size over which it builds;
  turn away from the group, step behind a wall, end combat and switch POV.
  Only third-person framing should widen, without cycling as enemies leave the
  screen. Test third-person camera collision in a narrow interior.
- In Cinematic Views, use + Add View to select a word wall and Chillrend (or
  another placed item). Search for word wall and distinguish its stone mesh from
  overlapping word/effect objects. Test with weapons sheathed and drawn, and
  open SMF both normally and through Risa. Verify the list/refresh/status remain
  usable when aiming at sky or no valid geometry. Check the gold focus marker
  against the visible subject by moving the menu aside. Refresh after re-aiming.
  Repeat in first person, third person and at different window scales. Invisible
  trigger volumes should not displace visible scenery in the subject list.
  Bind an Aimed Surface point on a large entrance or terrain as well.
  Preview the native third-person entry at small/large distances; test all
  six standard framing sliders and per-channel Transition Overrides. Lower Zoom
  must move closer, as in Third Person; FOV is an absolute lens value. Check
  sky/clouds/horizon against geometry at Zoom 0, 10, 100 and 200, narrow/wide FOV,
  camera rotation, walls, corners and changing collision. No scene/sky divergence.
  Confirm a picked-up/missing subject cannot pull the camera to its old position.
- Preserve the 21:28-21:29 log in build/diagnostics/cinematic-radius-20260918:
  it confirms the word-wall entry started/rendered, then ended with "Movement
  released the idle view" after 2.11 seconds. The next view ended when opening
  the menu. Both paths must be corrected for walking and Quick Tune respectively.
- Bind/reset a view, set Radius to a small usable value and approach while
  walking. First entry must start a fixed ten-second view without an idle wait
  or prior aim requirement. Keep walking/sprinting/jumping, circle the subject,
  and cross back outside the radius. The subject must remain loosely framed,
  including behind the player's movement direction; the former 90-degree yaw
  limit must not lose it. Brief occlusion must not drop an acquired lock, while
  native camera collision still respects walls. Missing/unloaded subjects end it.
- After first playback, set the view's Idle Timer to 0.5, 5 and 20 seconds.
  Stand still inside its radius and verify that complete wait. Walking after
  activation must retain the lock; manual mouse/right-stick camera input must
  release it and restart idle timing. Test with Vanity disabled, then with
  Vanity enabled and its timer set very short/long: the view's own timer must
  remain unchanged. No first/idle toggle, duration slider or Repeat Delay.
- Open Quick Tune during first and idle visits. The named Cinematic View box
  must edit that bound profile, with live Rotation/Pitch Offset, all six camera
  channels and the standard transition companion. Radius and Idle Timer must
  also edit that view. Spend more than ten seconds tuning a first encounter,
  then close: it must resume its remaining time, not expire or treat the menu
  controls as manual look. Mouse, pad, companion B-close, hotkey-close and
  provisional slider revert should behave like the other Quick Tune layouts.
- Check the cleaned Cinematic Views editor at narrow and wide menu sizes. Camera
  and Activation controls should have no descriptions; Name/Search fields should
  fit their panes, and Preview/Reset Encounter/Remove actions should wrap without
  overlapping. Picker names, identifying details, marker and errors remain usable.
- Tune Lock-On Tightness in the editor and live Quick Tune at 0, 0.5 and 1. Lower
  values allow framing drift while still following the subject as you circle it;
  higher values hold the authored angle more precisely. Default 0.5 should match
  the previous lock. Check both Rotation and Pitch Offset with narrow/wide FOV,
  entry/return smoothing, save/reload of distinct per-view tightness, and loading
  older entries without the new field. Camera collision must remain native.
- Combat, dialogue, normal menus, target lock, mounts, POV change and flight
  still release a view. Ordinary locomotion/animation camera bindings must not
  steal it when movement starts. First-person must not activate or be forced
  into third person. Entry/return transitions must not change gameplay tuning.
- Preview On Close queues while menus disable movement and bypasses radius,
  idle wait and LOS for a loaded subject. Check editor/HUD rejection messages.
  A preview must never consume first-encounter history; an actual rendered first
  visit must, even if dismissed early. Test save/reload, another character,
  loading a save before the encounter and Reset Encounter. Open Quick Tune
  during a preview and verify its live angle edits and paused timer as well.
- Update/save/reload two independent profiles and timers, including transition
  flags. Check Quick Tune Update/Save as New, copying entries, preset rollback,
  stable IDs/history, and old range/idle_delay/native-profile migration. No live
  preset file should change until explicit Update/Save. Existing shot-only
  definitions still begin with normal third-person framing defaults.
- On 1.5.97 with Risa 5.2, open SMF through Risa, select an existing preset and
  quit without saving the game or opening Journal. Restart and verify the
  selection and saved values. Repeat for Update, active rename/delete, and the
  preset-cycle hotkey. Check controller navigation and movement after closing
  the menu with Risa. Offline checks do not replace these gameplay checks.

Run the reporting user's exact executable and relevant mod combination first,
then the older/current baseline runtimes. Record the DDC DLL hash and all dependency
versions. Keep failures and passes rather than replacing a failed log with a pass.

- Reproduce the original failure with 1.0; confirm the candidate starts and the
  expected hooks install without ambiguous or skipped checks.
- Load a disposable test save. Exercise first/third person, target acquisition,
  target switching, mounting, dialogue, unpaused menus, furniture, and collision.
- Test Pitch Bias at negative, zero, and positive values while approaching and
  leaving an enemy. Verify unlock, target death, POV change, preset reset,
  indoor/location/enemy overrides, and a bias-only animation override.
- Type slider values in the full menu and Quick Tune. Check Enter, click-away,
  Escape, negative/decimal values, range limits, and displayed speed sliders.
  Confirm normal mouse dragging and controller adjustment still work.
- Copy/paste complete camera, target-lock, noise, first-person, weapon, and
  animation tabs. Check both environments, disabled tuned entries, per-hand,
  directional, enemy, and location settings. Reorder locations/load a test preset
  between copy and paste. Unrelated settings must remain unchanged.
- Save and reload format-8 presets; load released format-7 presets without
  changed tuning. Use copies so the released build's presets remain available.

Complete the broader [release checklist](RELEASE-CHECKLIST.md) as well. Passing
an offline test is evidence about those instructions, not a substitute for this
gameplay acceptance or a promise of compatibility with every other plugin.
