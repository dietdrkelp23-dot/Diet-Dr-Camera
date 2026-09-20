# Diet Dr Camera 1.2.0

Adds automatic startup diagnostics for compatibility reports, expanded Damage
Reaction and projectile flyby noise, Crowd Modifier, more Target Lock biases,
and separate first-/third-person projectile tracing. Existing presets remain
readable. See SUPPORT-LOGGING.md for the logs to include with a startup error;
full gameplay compatibility across every runtime is still unverified.

- Disable Crowd Modifier while paragliding and clear its stored adjustment.
  Keep buffered diagnostics for update timing, follow corrections and wind
  modulation; report after flight or on pause instead of during flight.

- Match the Reset button size in Projectile Tracing, Death Camera and Ragdoll
  Camera to Cinematic Effects, including both projectile-tracing views.

- Add proximity-scaled flyby noise to NPC magic projectiles, including
  firebolts, fireballs, ice spikes, Ice Storm and creature spit. Use the existing
  NPC Magic amounts in each view; transformed casters retain Transformations
  ownership. Support compatible mod projectiles through their spell/projectile
  metadata. Keep dragon/centurion cinematic ownership, continuous streams and
  instant beams separate. Player Repulse settings do not shape flyby noise.
  Give magic twice the flyby radius and 50% higher base intensity than arrows:
  600 units and 1.5 intensity versus 300 units and 1.0 for archery. Retain the
  smooth distance falloff and each view's existing NPC Noise amounts.

- Replace NPC bow/crossbow release noise with cinematic noise from passing
  arrows and bolts. Closer passes are stronger, using the existing NPC Noise
  Archery controls for each view. Player weapon noise and Repulse settings no
  longer shape NPC archery noise. Wall stops and known player hits do not add
  false flybys; repeated updates do not replay the same projectile.
  Fix the initial implementation's flight gate to use native linear velocity,
  increase its cinematic motion to a visible level, and report bounded flyby
  diagnostics without requiring Verbose Logging.

- Add a clear contact response and nearby cast noise for frostbite-spider spit.
  Expand hostile magic reactions for webs, paralysis, resource drains and area
  staggers, using the base game, DLC and Creation Club attack/effect records.
  Preserve creature melee noise. Keep dragon breath and centurion/Forgemaster
  breath under their dedicated cinematic noise controls, without generic NPC
  noise doubling them. Continuous incoming damage remains smooth.

- Simplify the Flee Framing and Damage Reaction descriptions. Expand incoming
  creature reactions using UESP and the game's race/attack records: distinguish
  frost atronach arms, heavy claws and bites, tusks, hooves/rams, tentacles,
  mechanical weapons, ballista bolts and spirit contacts. Cover expansion and
  Creation Club creature variants, including vampire lords, lurkers and bone
  colossi. Keep the existing intensity controls and motion/recovery limits.
- Improve startup diagnostics with Skyrim/SKSE and DLL build identification,
  Address Library header details, loaded DLL versions and startup checkpoints.
  Hook errors include relevant instruction bytes, expected/observed targets
  and the log location. Keep three previous DDC logs across launches and use
  a temporary log if the normal directory cannot be written. These diagnostics
  work without enabling Verbose Logging. See SUPPORT-LOGGING.md for reporting.
- Keep the preset detail card on the correct file after Save as New, Quick
  Tune selection and renaming by tracking preset names instead of row indices.
  Clicking any preset still reloads its latest save and discards unsaved edits.
- Add third-person Crowd Modifier under Cinematic Effects. Independent Zoom
  Intensity and FOV Intensity sliders control widening as nearby engaged enemies
  accumulate, with distance weighting, wall checks and smooth recovery. Intensity
  scales strength and crowd response; extra enemies contribute with diminishing
  returns instead of reaching a fixed four-enemy ceiling.
- Defer Cinematic Views pending a new implementation. Remove its menu, Quick
  Tune controls and runtime from this build. Existing preset definitions remain
  readable and writable for future use.
- Persist preset selection, rename and deletion immediately, including when a
  launcher opens SKSE Menu Framework without opening Skyrim's Journal. Track
  direct framework window changes for menu navigation and preference saving.
  Reviewed Risa All-In-One Menu 5.2 source and reproduced/fixed the selection
  persistence gap. Reviewed SkyZoom's source without finding DDC file access.
  The separate report of updated preset TOMLs reverting to defaults remains
  unconfirmed; these repairs do not establish its cause.
- Ignore non-damaging stat modifiers in Damage Reaction. Adamant's block
  slowdown was being mistaken for a magic impact, producing an extra camera
  kick when blocking. Real weapon/blocked contacts and damaging magic retain
  their existing strength and motion.
- Prevent R3 from resetting the native third-person and mounted camera orbit.
  Target-lock input, held POV changes and other button handling still run.
- Keep first-person cinematic and nearby-enemy noise on independent phases
  with their own character settings. Entering block or changing the player's
  noise profile no longer redirects those effects or retains their old motion
  in a profile crossfade. Damage Reaction tuning is unchanged.
- Split Projectile Tracing into Third Person and First Person sections using
  the same separators as Cinematic Effects and the same control/text scale
  as the Death and Ragdoll Camera tabs. Each view has
  independent bow/crossbow and spell/staff toggles, reticle size/thickness and
  sneak-eye offsets. Spell/staff tracing alone exposes the sneak-eye controls.
  Preset format 11 carries older shared tuning into both views, then saves
  independent values. View changes, toggle changes and preset/game loads clear
  old tracing state.
- Follow fired spell/staff projectiles by their native identity, position and
  velocity, refreshing the remaining trajectory as enemies move. A predicted
  arrival no longer starts a hit marker's settling timer: only an engine
  contact supplies the final impact point. Lost projectiles disappear without
  a fabricated impact. Zero-gravity spell previews use the same collision-
  checked path as gravity-affected spells, including native parallel first-
  person aim, and fired shots no longer inherit a stale preview endpoint.
- Add Damage Reaction under Extras > Cinematic Effects, with separate first-
  and third-person Intensity sliders. Motion and recovery are tuned automatically. Incoming
  blades, blunt weapons, arrows, fire, frost, shock, poison and other damage
  have distinct reactions. Greatswords, battleaxes and warhammers carry more
  weight than one-handed weapons, and power attacks recoil more strongly.
  Neither current/maximum health nor damage amount affects motion. Weapon hit
  events trigger the effect directly, including blocked contacts. Projectile contacts
  contribute their incoming trajectory; other hits use the damage source's
  direction when known. Directional recoil is stronger in both views, especially
  first person, and side hits deflect the view away from the incoming source.
  First-person impacts retain their quick impact/recovery and have twice the
  directional pitch/yaw travel, with restrained roll and fine texture.
  Giant club/swipe/stomp attacks, centurion
  axe/hammer hands, creature bites/claws/heavy blows and dragon bite/wing/tail
  attacks have distinct responses. Centurion hand detection uses the active
  attack clip; chops compress downward and mirrored sweeps follow the striking
  arm. Unknown creature attacks retain a generic response. Duplicate spell
  notifications in one impact are coalesced, and shield bashes are accepted.
  Magic streams ease into continuous directional pressure with elemental sway
  or vibration, then fade out smoothly. Repeated magic ticks
  refresh contact without restarting the motion, and
  overlapping impacts have bounded rotation with tighter first-person roll limits.
  Both views default to off. Preset format 10 preserves each view's intensity;
  the experimental Recovery, Direction and Texture keys are retired.
- Remove the experimental Damage Reaction health reader and health-change hook.
  Harmful magic applications produce consistent motion independent of tick
  magnitude; healing and expiring beneficial effects produce none.
- Controller hints on DDC sections now follow DDC's controls: D-pad Right
  enters the page, B returns to sections, and bumpers switch tabs or make
  larger slider adjustments. Copy, Paste and Quick Tune show their current
  bindings; keyboard keys and stick clicks are identified explicitly. Each
  binding and action share an outlined group, with clearer descriptions and
  two-line layouts when space is limited. Copy/paste hints appear only over a
  supported clipboard target and identify its contents, including transition,
  location, weapon and enemy overrides, environments and entire tabs. Override
  names match the buttons' wording and capitalization. Paste hints stay hidden
  until something has been copied into the menu clipboard. Each action has a
  fixed position and text size during navigation; hidden hints leave their
  space empty, and longer labels do not move neighboring controls. Visible
  boxes fit their current text without padding short bindings to longer ones.
  Clipboard hints remain steady while scrolling selected list entries into view. Sliders
  and Reset buttons show no clipboard hints. The existing footer space is reused
  without adding scrolling. Other mods and framework settings retain the
  framework's hints and input behavior.
- Target Lock's General tab cannot be copied or pasted. Category tabs retain
  their existing whole-tab clipboard controls.
- Add Height Bias, Zoom Bias and FOV Bias wherever Target Lock Pitch Bias is
  available, including Quick Tune and nested overrides. The order is Aim,
  Height, Zoom, FOV, Pitch. Each new control defaults to disabled and zero.
  Panels accommodate the extra controls without adding scrolling.
- Like Pitch Bias, the effects increase as the locked target approaches, fade
  out at 600 game units, and ease back to neutral when lock ends. Height moves
  the camera vertically, Zoom changes camera distance, and FOV changes view
  width. Existing zoom limits and collision handling still apply.
- Preset format 9 preserves the new controls, disabled tuning and explicit
  zero overrides. Earlier presets keep their existing camera behavior.

# Diet Dr Camera 1.1.1

- Whole-tab copying now treats the Sheathed and Unsheathed base rows as matching
  settings in either direction. This applies to Third Person, Target Lock,
  Camera Noise and First Person tabs. Each environment keeps its own values;
  other sub-states still match by name. Distinct transformation states remain
  separate. Matching entries retain the existing nested-override copying.
- Regression checks cover both directions, reordered rows, indoor/outdoor
  separation, unmatched entries and ambiguous identities.

# Diet Dr Camera 1.1.0

Adds direct numeric editing, whole-tab copying, Target Lock Pitch Bias,
startup compatibility fixes, and menu refinements.

- Click a slider's displayed value to type a number. Enter or clicking away
  commits a valid value within the slider's range; Escape cancels. This works
  in the main menu and Quick Tune, including sliders with a displayed speed scale.
  The first click immediately selects the whole number in yellow, ready to type,
  with the cursor's pulsing outline and green confirmation. The editor fits the
  digits, including when UI scale is increased. Opening an editor without
  changing its text preserves the original precision.
- Transition override toggles use the same size on every row, including Rotation.
- Projectile Tracing uses fixed crosshair smoothing equivalent to the former
  Smoothing slider's 0.30 setting. The slider is removed; saved legacy values no
  longer affect it. Trajectory and impact-marker lifetimes are unchanged.
- Copy and paste whole configuration tabs from their headers using the existing
  copy/paste bindings, or right-click a tab for Copy Tab / Paste Tab. Matching
  entries include both environments, enable flags, and nested overrides. Specific
  Weapons and Specific Animations carry their identities; missing bindings are
  added on paste. Different types of settings are rejected. Entries match by
  identity instead of list position, including reordered location overrides.
- Target Lock transition overrides have Pitch Bias directly below Aim Bias.
  Positive values add upward pitch as an enemy approaches; negative values add
  downward pitch. The effect fades smoothly to zero at 600 game units, with
  15 degrees per unit of bias at contact. It defaults to zero and has no global
  slider. Entry, enemy, weapon, animation, and location profiles store the override.
- Fix a reproduced startup crash on Skyrim 1.5.97: the menu SDK retained a null
  DLL handle when DDC loaded before SKSE Menu Framework. Resolve the dependency
  when its API is used and wait for its ImGui context before applying UI scale.
  This is a separate finding from the still-unidentified `RuntimeHooks.cpp(24)` report.
- Hook preflight accepts additional equivalent compiler encodings and validates
  established SE/AE call sites without decoding unrelated trailing code. Failure
  logs identify the function and relocation IDs being inspected. The reported
  `RuntimeHooks.cpp(24)` failure is still under investigation; that line alone
  identifies the shared failure handler, not the rejected hook. Both released
  and candidate hooks pass offline preflight on clean 1.5.97, 1.6.1170 and
  1.7.104 executables with their matching released Address Library databases.
- Preset format 8 adds the neutral Pitch Bias override. Existing released
  defaults and preset meanings are preserved.

See [RUNTIME-TESTING.md](RUNTIME-TESTING.md) for the evidence and remaining coverage,
and [BUILDING.md](BUILDING.md) for the full source build instructions.

# Diet Dr Camera 1.0.0

Initial public release for Skyrim SE and AE on Windows x64. Configure the camera
in game through SKSE Menu Framework and Quick Tune. No ESP, MCM or Papyrus scripts
are included.

## Features

- Third-person framing and transitions for movement, combat, horseback,
  transformations and other gameplay states. Independent indoor, location,
  weapon, animation and target-lock overrides provide more specific tuning.
- First-person FOV and camera effects, with separate settings for each view.
- Player camera noise and nearby NPC noise. NPC Magic, Melee, Archery, Shouts
  and Transformations have separate amounts and distance falloff. Specific
  weapon, spell and shout entries supply the effect tuning.
- Player Hit Shake for registered melee hits and direct arrow, bolt, spell and
  staff missile hits on actors. Strength, Speed, Bounce and Texture are available
  in eligible entries in both views, including item and location overrides.
  Strength defaults to zero. Projectile feedback softens gradually with distance;
  each shot retains its launch tuning and contributes once. Scenery hits, damage
  over time, beams and splash-only damage do not trigger projectile Hit Shake.
- Projectile previews and traces, plus configurable projectile Repulse.
- Dialogue, menu, death and ragdoll cameras.
- Replacement Vanity Camera. Looking around keeps Vanity active; movement and
  actions end it. Looking around before entry resets its idle timer.
- Flee Framing keeps approaching movement in view. Fast approaches retain their
  protection through a turn while momentum settles, then release smoothly even
  with high camera looseness. A new Whirlwind Sprint starts fresh: a forward
  dash cannot inherit the previous approach's momentum hold.
- Horseback Drawing/Zoomed profiles and stabilized mounted target tracking.
- Staff Ritual entries across all five schools, including mod-added rituals,
  sneaking and target lock. Continuous ritual beams retain continuous timing.
- Separate indoor/outdoor collision options for ground, doors, trees and walls.
- Shareable TOML presets, entry copy/paste, controller navigation and live noise
  previews while tuning. The main archive contains no presets or configuration.

True Directional Movement and Open Animation Replacer are optional integrations.
Hit Shake uses native hit/projectile events and does not require Precision.

## Requirements and compatibility

Install matching SKSE64, Address Library and SKSE Menu Framework 3, including the
framework's requirements. See REQUIREMENTS.txt for installation and download links.

One DLL implements the known SE/AE runtime paths listed in RUNTIME-SUPPORT.md.
Gameplay testing and author acceptance recorded for this release are on Skyrim
1.6.1170; other executable versions have address/layout checks but incomplete
gameplay coverage. Skyrim VR, Microsoft Store/Game Pass and Epic builds are excluded.

Use one third-person camera overhaul and one camera-noise implementation.
SmoothCam and Camera Noise SE overlap with DDC. Other dialogue, death-camera or
unpaused-menu mods can also compete for camera/input hooks. Improved Camera has
specific first-person FOV handling; not every combination has been tested.

Projectile predictions model ordinary missile flight. Scripted steering, homing
and changing frame times can produce a different path.

The author accepted the reported Whirlwind Sprint/Flee Framing retest on
September 14. An earlier intermittent movement-stall report involving werewolf
combat, target lock and menus has guarded recovery, but complete resolution has
not been verified. Recheck that scenario during final gameplay acceptance.
RELEASE-CHECKLIST.md records accepted behavior and the remaining manual coverage;
automated checks do not establish every gameplay combination.

## Presets and source

Version 1.0 writes preset format 7 and reads development formats 1-6. Public
preset compatibility begins with this release: later updates must preserve its
tuning and omitted defaults. Unsupported newer formats cannot be loaded or
overwritten by an older build. See PRESET-AUTHORS.md and PRESET-COMPAT.md.

The corresponding source ZIP includes DDC, CommonLibSSE-NG, the instruction
decoder, all seven dependency source trees, exact recipes/patches, tests and
build/package instructions. Auto-deployment is off by default; local development
requires an explicit mod-folder destination. Routine diagnostics use Verbose
Logging, which is off by default. Matching PDB symbols remain with the developer.

Licensed under GPL-3.0-or-later with modding/linking permissions. See LICENSING.md,
EXCEPTIONS.md and the included third-party notices.
