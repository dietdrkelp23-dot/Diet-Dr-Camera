# Public preset compatibility

The compatibility promise begins with the public **1.0** release. A preset
authored for 1.0 must retain its tuning and meaning in later DDC versions.
Development files before release need no migrations or repair heuristics.

## One codec and frozen defaults

`SettingsManager::BuildSaveTable` and `ApplyTable` are the only settings codec.
`PresetManager` delegates to them. Engine notifications are separate so the real
codec and filesystem manager can run in automated checks without Skyrim.

Loading a named preset resets settings before applying it; missing values must
never leak from the previously selected preset. Global hotkeys stay global.
The authored-reset and sparse-preset regression cases cover animation bindings
and target-lock werewolf feeding, which were omitted from the earlier reset.
Invalid/newer-format files are rejected **before** that reset. Updates use atomic
replacement and preserve the author description; failure keeps the old file.

Writes are sparse. Missing keys resolve to the released initializers, so those
initializers are part of the file format. Never retune them after release. Ship
an optional preset if different defaults are wanted. New settings must start
off, neutral, or reproduce the old behavior. Do not generate a second defaults
configuration for installation.

The writer baseline must match the reader seed, with no exceptions:

- Ordinary camera profiles use `CameraProfile::Default3p()`.
- Werewolf, Vampire Lord and mounts use their corresponding factories.
- An absent indoor table mirrors the loaded outdoor profile. A present indoor
  table is an independent snapshot against Default3p, including all-default values.
- Location camera snapshots use Default3p, independently of their parent tuning.
  Presence is the binding; an empty value set retains a `set = true` marker.
- Animation, binding, dialogue and noise snapshots must preserve their own
  baselines, metadata, transitions and disabled-but-authored tuning.

## Stable keys and identities

Never rename, reuse or repurpose a released key. Retire it by ignoring it. Never
change stored units, sign, ranges or interpretation without a versioned reader
that preserves previous public values. UI remapping must not rewrite storage.

Camera distance is `zoom_offset`. The name `zoom` is reserved for Zoomed
sub-state tables. The nested table writer detects scalar/table and duplicate-key
collisions and fails the save instead of silently dropping tuning.

`GetIndoorEligibleProfiles()` keys identify outdoor/indoor camera profiles,
location slots, noise and first-person entries. They are persisted identities.
Shouts use catalog names, not enum positions.

Binding categories, cast types and `s0`...`sN` binding sub-state positions are
append-only contracts, pinned by static assertions in SettingsManager.h. Do not
change the assertions just to permit insertion. Prefer named keys for new lists.
Dialogue look and animation UIDs must survive save/load, rename and reordering.
Form bindings use plugin name plus local form ID; never persist load-order IDs.

## Format versions

`[meta].format = 1` is the frozen original schema. Format 2 adds the staff ritual
profiles and associated override keys. Format 3 adds independent third-person
and first-person NPC melee noise amounts (both default to zero). Format 4 adds
NPC archery amounts and separates transformation amounts from Magic. Both new
controls default to zero; formats 1-3 migrate their old combined Magic amounts
to Transformations as well, preserving the authored amount in each POV.
Format 5 separates Shouts from Magic with `shout_intensity` and
`shout_intensity_fp` under `[cinematic.npc_noise]`. Both default to zero.
Formats 1-4 copy their old combined Magic/Shouts amount to Shouts in each POV;
format-5 explicit or omitted zeros remain off. Format 6 adds per-attack
`hit_shake_strength`, `hit_shake_feel`, `hit_shake_recovery` and `hit_shake_set`
to noise profiles, including first-person, weapon and location snapshots.
Strength defaults to zero; formats 1-5 retain their existing camera behavior.
The authored marker preserves explicit zero overrides in optional FP attack
directions. Format 7 replaces Feel/Recovery with `hit_shake_speed`,
`hit_shake_bounce` and `hit_shake_texture`. Strength still defaults to zero;
Speed and Bounce default to 0.5 and Texture to zero. Current development builds
write format 7 and read formats 1-6.

Format-6 Hit Shake converts once on load: Strength preserves the old wave's
first-peak amplitude before axis mixing/limiting, Speed matches its first-peak
time within the new 30-140 ms range, and Bounce matches its rebound/peak ratio.
Texture starts at zero. This preserves the main characteristics, not the exact
old waveform or its extreme recovery times. Explicit zeros/authorship survive.
The source file stays untouched until saved; saving writes all four modern
values so reload cannot migrate them again. Modern keys take precedence over
leftover legacy keys. Every customized format-7 hit row includes the modern
keys, even when they equal defaults; preset authors should retain them.
This is not a preset author's own version number or
the DLL version. Missing stamps are outside the public contract.

Staff ritual entries use the existing profile defaults. Their named keys and
target-lock slots are additive, and all original indoor/location and target-lock
slot indices remain unchanged. Format-1 fixture values remain immutable.

Increment `kCurrentPresetFormat` whenever adding persisted fields or changing
their meaning. Even an additive field needs a new number: an older reader must
refuse the file rather than erase the new setting when it updates the preset.
Later readers must retain support for prior public formats, with explicit
migrations where necessary. Bumping the number alone is not a migration.

Newer formats, malformed metadata and non-finite numeric values fail without
changing current settings or overwriting the preset. Do not weaken this check
to make a newer author's file load on an older DLL.

## Required checks before every update

`PresetCompatibilityChecks` compiles the production codec, reset, validation,
registry and filesystem manager. It checks fresh/reset defaults, every registered
profile against multiple baselines, authored indoor and location snapshots,
dialogue/animation/weapon identities, noise and first-person settings, disabled
tuning, stable rewrites, UTF-8 filenames, descriptions, hotkeys and failed loads
and writes. CameraQualityChecks also tests atomic replacement failures.

The immutable 1.0 defaults and authored preset are in `tests/fixtures/`. Later
releases must load that exact TOML to the same recorded values. **Never regenerate
the 1.0 fixtures to silence a regression.** Add later-version fixtures separately.
When adding neutral entries, extend comparison coverage without changing any
previous reference value. Review initializers and key registries as part of that
change; no test can establish the gameplay meaning of a new feature by itself.

The release packager always runs the checks and packages an explicit allowlist.
The player ZIP must contain no preset TOMLs, generated defaults or global config.
See PRESET-AUTHORS.md for the workflow for Nexus preset authors.
