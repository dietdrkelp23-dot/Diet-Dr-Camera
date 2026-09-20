# Release verification - 1.2.0

The 1.2.0 Release build and all 41 standalone checks pass, including automatic
startup logging, abrupt-exit log persistence, previous-session retention,
runtime-hook validation, preset migration, damage reactions and projectile
flybys. The paraglider timing experiments are reverted; Crowd Modifier's glide
exclusion and buffered diagnostics remain. Cinematic Views and the experimental
Apply button are absent from the active build/menu.

All 21 available Steam executable fixtures pass 210 offline hook scenarios and
47 vtable target checks each. GOG 1.6.659 and 1.6.1179 executable fixtures are
missing. These checks do not launch Skyrim or test gameplay.

The release packager snapshots the working source, builds and tests it, verifies
the DLL/PDB identity and version, checks every archive member/hash, and preserves
the matching symbols privately. The player ZIP contains only the DLL and required
stagger-camera animation; the separate source ZIP includes the startup support
guide, runtime matrix, dependencies, licenses and build instructions.

Release evidence is retained under `build/diagnostics/release-1.2.0-20260919/`
and the timestamped `release/candidate-1.2.0-*/` directory. The candidate manifest
identifies the exact files. An extracted-source build is checked separately
before handing over the archives; its results belong with those release records.

Startup was previously checked on 1.5.97 and 1.7.104 with the logging build
identified in RUNTIME-TESTING.md. These are earlier DLLs, not a fresh launch of
1.2.0. The author has tested the recent effects in the normal 1.6.1170 list.
Gameplay acceptance across all targets remains incomplete. The final packaged
1.2.0 DLL has not been launched in game during release preparation. See
RUNTIME-MATRIX.md and SUPPORT-LOGGING.md for coverage and reporting instructions.

This is local release preparation; no Nexus upload or source tag is published
by the packaging process. The historical evidence below applies to its named
versions and must not be read as additional 1.2.0 testing.

## Previous release verification - 1.1.1

This patch corrects whole-tab matching between the Sheathed and Unsheathed base
rows after the author reported the omission. Automated clipboard checks cover
both directions, environment separation, preserved profile/transition values,
unmatched states and ambiguous identities. The standard packaging script runs
all standalone checks and verifies the DLL, PDB, source and archive hashes;
the resulting manifest and logs record the exact patch artifacts.

The runtime-hook and menu-loading code is unchanged from 1.1.0. The live results
below belong to the identified earlier builds; this patch has not received an
additional in-game tab-paste test. The public source build verification and
release hashes are recorded in the tagged SOURCE-PROVENANCE.md.

## Previous release verification - 1.1.0

The author approved this update for distribution on September 15, 2026 after
testing in the normal Skyrim 1.6.1170 mod list. It includes typed slider values,
whole-tab copy/paste, Target Lock transition Pitch Bias, consistent transition
toggles/heading, and fixed 0.30-second crosshair smoothing.

The final tested DLL SHA256 is
`5B3A303C9E3F7E900E2A06265411397131BFA18FCF276473AA4D89F5AC41D697`.
All 33 automated checks pass, including new numeric-input, Pitch Bias and
late-loaded Menu Framework checks. Packaging verifies the matching PDB, exact
runtime payload, embedded notices, source inventory and archive hashes.
The public source's build verification is recorded in its GitHub provenance
document; the build instructions require no Skyrim installation or Nexus login.

An earlier build with the same runtime fixes passed startup on isolated 1.5.97
and 1.7.104 installations as well as the normal 1.6.1170 list. The final DLL's
23:08-23:10 main-list log confirms hook validation and gameplay on 1.6.1170.
See RUNTIME-TESTING.md for the exact build hashes and test scope. Historical/GOG
targets have not all been launched, and the original reporter's exact
`RuntimeHooks.cpp(24)` failure remains unidentified. Release approval does not
turn these gaps into passing results.

## Previous release verification - 1.0.0

September 14 release audit. This file distinguishes automated evidence from live
gameplay; an unchecked item is not a passing claim. Prior detailed development
checklists and the intake log are retained in the local audit snapshot.

Mysticism projectile tracing was accepted by the author on September 13, 2026.
The accepted projectile-tracing DLL SHA256 was
018804345E6E25A8E3619A1E932F99445547B65778A1DC5DEB67D59C42F424DA.
The author subsequently confirmed staff Ritual selection and the Vanity
close-up retest working on 1.6.1170. The accepted Vanity DLL SHA256 is
81D64585CB96516403903E41A6E3ED365BF4E7F36C060FEF170C4AE7410EB9EA.
Automated checks do not establish gameplay acceptance of every profile variant.
Current hashes are in the candidate manifest; the checks below remain open until
their results are confirmed.

The final whole-project audit confirmed profile lifetime and incomplete reset
bugs, requiring a new DLL after that acceptance. The source changes preserve
the preset format, authored defaults, spell routing and vanity transition logic.
The first smoke-test item below covers the changed paths.

## Automated release gate

The packager runs these checks and records the results in build/check logs and
manifest.json. Publication uses the exact archive those results describe.

The September 14 release cleanup moves routine motion/menu diagnostics behind
Verbose Logging, gates the expensive capture probes, and removes the unused raw
mouse-input experiment. Actual movement-stall warnings/recovery remain enabled.
Release binaries use portable source/PDB paths. Source builds default to no
deployment, and the test-preset generator requires an explicit output path.

- Build the final Release DLL and matching PDB.
- Verify shared trampoline reservation, historical SKSE without a branch pool,
  exhausted-pool fallback, hook chaining and preservation of earlier stubs.
- All 30 CTests pass, including NPC melee phases and POV gates, spell/staff ritual classification and priority,
  NG hook/layout checks, the real settings codec, frozen 1.0 fixtures,
  actual preset file operations, collision filtering, location identity, mounted
  tracking, vanity, navigation, item bindings and override inheritance.
- Hit Shake remains off in formats 1-5 and survives preset round trips in all
  attack entries, weapon types, specific/base/enchantment bindings and both POVs.
  Explicit zeros, indoor/location routing, reset, bounded contact queues,
  simultaneous impacts and timing at 30/60/120/144/240 FPS are covered.
  Four-control checks cover matched first-kick strength, monotonic tempo and
  rebound, continuous layer handoffs, bounded texture, automatic axes/FP roll,
  contact ages, diminishing cleaves and one-time format-6 conversion.
- Archery checks cover Drawing/Zoom/sneaking/mounted routing, all three item
  binding scopes, both POVs, indoor/location copies and explicit-zero persistence.
  Captured launch values survive later equipment/settings changes. Projectile
  identity deduplication, reference-generation reuse, in-place lifetime resets,
  bounded flights/contacts, expiry, zero Strength and POV/menu resets are
  exercised without the game. Mid-frame callback arrival is checked for both
  archery and melee queues. Archery onset/peak coverage across camera frame
  phases at 30/60/120/144/240 FPS, settled tails and continuous limited direction
  are checked. Native callback delivery and visual feel require gameplay checks.
- Reach inline Reset buttons above wide sliders at different panel widths and
  UI scales, while retaining column, pane, popup and scrolling navigation checks.
- Menu layout geometry covers 720p, 1080p, 1440p, 4K, 5K, 8K and ultrawide:
  first-use measurements, edited widths, resolution round trips, dormant pages,
  and floating panels near screen edges or taller than the viewport. These are
  standalone geometry checks; they do not establish rendered visual acceptance.
- Flee Framing smoothly approaches normal distance at full strength. Bound the
  first-frame and subsequent movement on engagement/release, including large
  existing lag and repeated threshold crossings at 30/60/120/240 FPS. Verify
  clearance when approaching from rest, eventual return of ordinary follow lag,
  partial/off strength, shoulder/pitch offsets and close native camera positions.
  Move the follow anchor through 1500-6000 unit/s approaches and 90-180 degree
  turns, including residual momentum, stopping, uneven frames and the shout's
  looseness transition. At 30/60/120/144/240 FPS, verify that hidden lag is not
  restored, release preserves the visible position, partial/off strength stays
  respected, and unrelated fast movement or a cleared state cannot engage it.
  Chain forward movement shouts immediately and after short gaps at sustained
  1500-6000 unit/s speed. They must follow the normal release spring and regain
  configured looseness. Check windup/fire boundaries, duplicate fire reports,
  old velocity on the boundary frame, new approaches, partial/off strength and
  invalid frame times without changing the original fast-turn protection.
- Reset a fully authored preset and edited scalar settings to the released
  defaults; load a sparse preset without retaining the old werewolf feeding
  profile or animation bindings. Keep captured spell/shout effect values valid
  after their map/vector entries are edited, erased and reloaded.
- Reject packaging a build from another source tree, missing build provenance,
  enabled auto-deployment or disabled checks before creating a candidate.
- Spell prediction matches the independently simulated native missile update
  order, including Mysticism Fireball/Firebolt data at 30/60/120/240 FPS.
- Validate x64 PE, SKSE exports, Release CRT, version and PDB GUID/age.
- Verify a filename-only PDB reference; scan the player DLL and source for
  local workspace paths, credentials, personal presets and build artifacts.
- Verify the SKSE Address Library and v5 flags, with no stale fixed runtime array.
- Package from the install allowlist; verify every archive member/hash.
- Player archive contains exactly DietDrCamera.dll and staggercamera.hkx; no
  documentation, standalone license files, symbols, presets or configuration.
- All license notices are preserved in the source ZIP and passive DLL resource.
- Source archive includes all working sources, fixtures, dependency sources,
  license notices and rebuild instructions; it matches the build inputs.
- Extract the source ZIP outside the working checkout and rebuild/test it with
  auto-deployment off. Retain the extracted-build report with the release records.

## Gameplay evidence

The user accepted corrected vanity behavior, mounted target-lock feel and all
four collision categories on September 12. The latest intake game log shows
native collision-filter activity and hold matching, without a crash report.
This is evidence from the previous DLL, not a live test of the final audit build.
The September 13 11:50-11:54 log confirms the accepted NG build loaded on
1.6.1170, validated its hooks, and produced held/released Fireball traces from
both hands. The author confirmed Mysticism tracing now works. The log has no
error or critical entries; its three warnings are menu-framing diagnostics,
ending with a clean 0.00-unit peak camera movement. That log alone does not
establish the remaining preset, menu or movement-stall acceptance items.

On September 14 the author confirmed that Vanity and first-person Hit Shake
feel good on the installed development DLL with SHA256
B652306F90C7968E80B9C00967B2B13F6666BBEFCD79EF0D18B40BF89FC132F4.
This accepts the current Vanity look behavior and corrected first-person feel;
the detailed device, POV, override and lifecycle matrices below remain open.

The September 14 19:52-19:59 log records the momentum-fix DLL loading on 1.6.1170,
with no error/critical entries. Flee diagnostics retain protection through the
fast turn and release as speed settles. This confirms the path executed; final
acceptance of the reported follow-up issue is recorded below.

After the chained-shout fix, the author reported the Flee Framing retest working
on September 14. The accepted DLL SHA256 is
04E33C1C3DB4EBA98008C1A74D0D5CC97FF33F85EADAE49B2DA82323D86CBB89.
The 21:16-21:20 game log records that build loading on 1.6.1170 and validating
its hooks, with no warning, error or critical entries. The author's retest
accepts the reported stuck hold after a forward Whirlwind Sprint; the broader
camera, runtime and lifecycle combinations below retain their recorded status.

## Final smoke test on the packaged DLL

- [ ] During a normal/power attack, shout or spell, open the main DDC menu and
  leave it open for 60 seconds. As in Quick Tune, action duration/fade progress
  must stay held while noise keeps moving in both POVs. Edit Intensity, Speed,
  Rotation Shake and Drift / Jitter in main, compact and specific-item editors;
  only the edited target should change. Switch between main and Quick Tune,
  including both open, then close them; timing must resume without catching up.
  Ordinary game-menu pauses and an unpaused framework window retain their behavior.
- [ ] Development Hit Shake: with Strength at zero, verify existing swing noise
  is unchanged. Raise it on a test preset with swing noise at zero; compare a
  registered melee hit with a miss. Test normal/power, sprint/sneak, directional,
  unarmed, dual wield, werewolf/Vampire Lord and mounted melee attacks in both
  POVs. Check distinct weapon-type, specific/base-item/enchantment and location
  values, including explicit zero overrides, then save/reload the test preset.
  Check `[HITSHAKE]` entry diagnostics. Test actual actor contacts with Precision
  and its own camera shake disabled; world-object contact delivery remains an
  in-game check. Bashes, arrows and spell/enchantment ticks must not add impulses.
  Hit several targets, open/close menus, change POV, enter/exit vanity and load
  another preset during recovery; old effects must clear without changing framing.
- [ ] Hit Shake feel: hold Strength fixed and compare low/high Speed, Bounce and
  Texture on the same attack. Check crisp, springy, heavy and rattling combinations
  at 30/60/120+ FPS. Test Precision's hitstop with only its camera shake disabled.
  Test native hit delivery with Precision absent, then check coexistence with it
  installed. Compare horizontal, overhead, rising, diagonal and spinning/reversing
  animations in both POVs, with both hands, two-handed weapons, fists/claws and
  mounted attacks. Check `[HITSHAKE] ... direction=motion` and changing pitch/yaw
  axes. Verify camera orbit, moving attacks, different weapon meshes, hitstop and
  variable frame rates. The detector measures animated weapon/hand trajectories;
  native hit events confirm impact. There is no Precision API integration or new
  collision/damage system. Missing/stale poses log `direction=attack` and use the
  contextual fallback. Equipment/mesh/race, menu, POV and load changes must clear
  old motion. Check one reaction per hit and unchanged Strength across animations.
  Confirm format-6 conversion is comfortable, then save/reload the four controls.
- [ ] Power-hit zero regression: enable normal Hit Shake, set power Strength to
  zero, then alternate MCO/vanilla normal and power attacks (including sprint,
  sneak and direction/weapon overrides). Zero power contacts must log the power
  entry with `strength=0.00`; normal hits keep their own tuning. Repeat in both
  POVs and after menu holds. Precision's own camera shake remains user-controlled.
- [ ] Archery Hit Shake: enable a modest Strength on bow/crossbow Drawing,
  then test nearby and distant actor/shield impacts, cancelled draws,
  misses, enchanted ammunition and several simultaneous arrows. Expect one
  reaction per projectile contact and no release-only kick from Hit Shake.
  Try distinct sneaking/Zoom, specific/base/enchantment, indoor/location and
  mounted settings in both POVs; disabled overrides must fall back correctly.
  Change stance/equipment while a shot flies: impact must use its launch tuning.
  Set Strength to zero and check it stays off. Open a menu, switch POV or reload
  during flight: discarded impacts must not reappear. Confirm the existing aim
  correction/traces and melee feel remain correct. Check `[HITSHAKE-ARCHERY]`
  requested/entry/strength diagnostics. Repeat rapid shots into the same actor
  for over 30 seconds; recycled IDs must not cause intermittent missing shakes.
  Missed arrows hitting scenery must stay quiet. Small changes in aim must not
  flip pitch or cause full sideways kicks. Inspect flight_ms separately from
  queue_ms when judging delay after a visible hit. Native contact delivery was
  observed in the initial trial, but the corrected identity, timing and feel
  need another in-game pass.
- [ ] Projectile magic Hit Shake: test direct actor hits and deliberate scenery
  misses with Firebolt/Ice Spike/Lightning Bolt and matching staves, with tracing
  on and off. Compare left/right/Both Hands, sneaking, specific spell/staff,
  base/enchantment and location overrides in both POVs; zero must remain off.
  Test modded ritual missiles and Vampire Lord projectiles. Beams (including a
  ritual-classified Lightning Storm staff), continuous flames, cones and damage
  ticks must stay quiet. Fireball direct hits and multi-projectile casts should
  produce one impulse per cast; splash-only hits do not trigger this effect.
  Test rapid casts, independent hands, dual casts and switching equipment while
  missiles fly. Menu/POV/load changes must discard old impacts. Inspect
  `[HITSHAKE-MAGIC]` spell/staff/hand/entry/strength/flight_ms/queue_ms and confirm
  accepted melee and archery behavior. The author accepted the spell/staff feel;
  the full routing and lifecycle matrix remains pending.
- [ ] Projectile distance/first-person tuning: compare nearby, medium and far
  actor hits with bows/crossbows, spells and staves, including hitscan lightning.
  Strength should decline smoothly with distance; moving the camera or changing
  zoom must not change attenuation at the same player/target positions. First
  person should have a clearly stronger usable range while zero stays off.
  Compare a single hit and rapid bursts in both POVs, then confirm settings
  survive save/reload. `[HITSHAKE-ARCHERY]` and `[HITSHAKE-MAGIC]` now report
  distance, falloff and effective strength with separate diagnostic budgets
  for each view. The author accepted corrected first-person feel on September 14;
  the systematic distance/weapon/POV comparison remains pending.
- [ ] First-person Hit Shake axis correction: a forward projectile hit should
  visibly pitch the view rather than mainly rolling the horizon. Compare melee
  attacks with vertical and horizontal motion, low/high Strength and zero.
  The corrected pitch/yaw/roll mapping must survive cast-animation recomposes,
  FOV changes and switching POV without replaying a stale hit.
- [ ] Hit Shake menu: confirm the separator and four sliders below attack
  camera noise, including weapon and location popups. Quick Tune's Hit Shake
  button must appear only for eligible attacks and edit the active attack's
  settings. Open it with a controller, edit/cancel sliders and press B to return
  to its button. Check 1080p, 1440p, 4K and available ultrawide resolutions,
  narrowed/shortened windows and resolution changes with the panel open.
- [ ] Development NPC overrides/Shouts: save and reload distinct weapon-type
  attack overrides. Check NPCs carrying different specific weapons and
  enchantments, including an explicit zero and indoor/location overrides.
  Use bows and crossbows, cancel a draw, and switch weapons after firing; each
  actual projectile should produce one beat using the firing weapon's entry.
  Check `[NPCNOISE] archery` and `contributing` diagnostics (bounded per session).
  Enable Shouts with Magic off in each POV, then reverse the controls. Test named
  and modded specific shouts, sneak/state variants, multiple shouters, menu/POV
  handoffs, and format-4 migration/save/reload. Beast howls still use Transformations.
- [ ] Development NPC melee addition: with NPC Magic at zero, enable Melee in
  each POV separately. Check normal/power swings, bashes, unarmed attacks,
  combos, sneak/sprint and weapon overrides; move out of range and switch POVs
  or open/close menus mid-attack. Dragons/centurions retain their separate controls.
  Verify first-person-only NPC magic (including summons/reanimation), and save /
  reload the two melee amounts. Loading an older preset must turn melee off.
- [ ] NPC Archery: enable each POV separately with the other NPC amounts off.
  Test bows, crossbows, cancelled draws, repeated shots, sneaking/mounted
  entries, distance falloff and menu/POV handoffs. Flying arrows must not retrigger.
- [ ] NPC Transformations: enable each POV separately with Magic/Melee off.
  Test werewolf and Vampire Lord transformations/reverts, normal/power attacks,
  werewolf sprint attacks/howls/feeding, and Vampire Lord spells/Bats. Verify
  their tuned noise and indoor/location overrides; disable Transformations with
  other NPC sources enabled and confirm beast noise stays off. Save/reload all
  four NPC amounts and verify migration of an older combined Magic amount.
- [x] Reported chained Whirlwind Sprint regression: shout toward the camera,
  turn forward and immediately shout again with cooldown bypassed for testing.
  The author accepted the corrected Flee Framing behavior on September 14;
  the new forward dash releases the previous approach's retained hold.
- [ ] With Flee Framing at full strength and Looseness enabled, walk/run toward
  the camera and make quick turnarounds. Confirm gradual movement across the
  approach threshold and on release, then compare the settled distance with the
  active profile. Check close walls, low FOV, horseback and sideways/away movement.
  Check partial/off strength and that dragon flight stays unaffected.
  Whirlwind Sprint toward the camera at maximum Looseness, then turn sideways
  and away while momentum remains. The character should stay framed without a
  lurch as the shout ends; ordinary follow lag should rebuild after slowing.
  Check repeated turns and deliberate camera orbit, plus stopping and menu/POV
  handoffs. The reported chained-shout case is accepted above; broader camera,
  strength and lifecycle combinations remain part of this manual matrix.
  Enable Verbose Logging to inspect `[FLEE]` speed, approach, held, tighten, lag
  and movement-impulse diagnostics.
- [ ] At 1080p, 1440p, 4K and available ultrawide resolutions, check all main
  pages, location/enemy/weapon override popups, preset editing/actions and Quick
  Tune. Confirm readable labels, unclipped controls and reachable Reset buttons.
  Change resolution after opening pages; confirm column and popup widths update.
  Scroll Quick Tune with transitions open, including target lock, and verify
  the final slider is reachable. Shorten the preset pane and check Done/Delete.
- [ ] In the main menu, use Up from a slider to reach its small Reset button,
  then Down to return. Check framing, noise and first-person pages, including
  wide panels. On a test preset, activate Reset and confirm it affects only
  that setting; check neighboring controls and popup navigation as well.
- [ ] On a separate test preset, cast a first-person fire-and-forget spell and
  a shout, open the DDC menu before their effects finish, and load another preset
  or reset the relevant entry. Close the menu and recast. Add/remove an active
  weapon, animation, dialogue or location binding and try entry copy/paste and
  controller slider cancellation. Check for correct routing and responsive
  Quick Tune. Reset All must clear animation bindings and target-lock werewolf
  feeding tuning; switching to a sparse/default preset must do the same.
- [ ] Recheck the accepted vanity return and ordinary/staff spell tracing on
  this rebuilt DLL, then continue the menu/POV and save/restart checks below.
- [ ] Let Vanity enter from both first and third person. Orbit with mouse and
  camera-look stick; the Vanity profile must remain active. Walk with keyboard
  or movement stick, draw a weapon, toggle POV and open a native menu; each
  must exit normally. Check Quick Tune editing, mapped Look/Move sticks, the
  return to the original POV and no close-up carryover. Looking around before
  entry must still postpone the idle timer; small stick drift must not.
- [x] Author confirmed Mysticism projectile tracing works on September 13.
  The current game log records Fireball previews and releases from both hands,
  with hit endpoints. Acceptance applies to Skyrim 1.6.1170 and the DLL above.
- [ ] Cast the Staff of Lightning Storm and a fire-and-forget ritual staff;
  verify each school's Ritual entry in
  Quick Tune, standing/sneaking and target lock. Check noise/Repulse and first
  person, then a second staff using an ordinary cast. Check indoor/location and
  enemy overrides, and save/reload the ritual tuning. Ordinary staff casts must
  continue selecting Concentration or Fire & Forget. Lightning Storm must keep
  continuous beam effects and stop without projectile lag; verify its Ritual
  Repulse slider controls ignition. The first staff candidate incorrectly chose
  Destruction Concentration for this staff; the corrected classifier has automated
  coverage using the installed Mysticism enchantment's casting/delivery/tier data.
  The author confirmed staff selection working after the correction; the
  12:54-12:58 log records Ritual for both continuous and fire-and-forget staves.
- [x] Author confirmed the Vanity close-up retest working on September 13.
  The 13:16:56-13:17:10 log records the return from FOV 50 to the requested 100,
  matching applied/world FOV, followed by return ownership ending at settlement.
  Later sprint routing requests FOV 80. The original close-up investigation found
  no saved camera-setting overwrite. This accepts the reported issue on the DLL above;
  broader menu, target-lock and runtime coverage remains scoped below.
- [ ] Confirm the Outdoor/Indoor collision question marks are gone in-game.
  Their removal is present in the accepted source and binary.
- [ ] In 1.6.1170, create a preset with distinct bow/horseback Archery and Zoomed
  distances, an all-default indoor/location copy of a tuned outdoor profile,
  and a disabled tuned melee override. Switch away/back and restart Skyrim.
  The values, transition settings, active location label and enable flags must persist.
- [ ] Open/close Inventory, Magic, Container, Barter and Favorites, switch menus,
  fast travel and load a save. Menu framing, HUD, activation and movement must work
  after removal of the experimental asynchronous Scaleform pre-warmer.
- [ ] Recheck the earlier movement-stall scenario: werewolf combat, target lock,
  POV controls and menu entry/exit. Stop release if movement still freezes.
- [ ] Repeat the core camera/menu smoke test on SE 1.5.97, early AE, post-629 AE,
  GOG, and 1.7.104. See RUNTIME-SUPPORT.md; address/layout checks do not replace this.
- [x] Author approved GPL-3.0-or-later on September 12, 2026. Original-code grant,
  exceptions, third-party notices and matching dependency sources are packaged.

Public preset compatibility starts at 1.0. No development-preset migration is
required. The scalar distance key is now zoom_offset; use presets created with
the final release build for this test and for the initial public preset uploads.

Distinguish implemented runtime targets from completed gameplay tests.
Publishing remains the author's action after final gameplay acceptance.
