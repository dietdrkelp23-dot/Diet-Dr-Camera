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
