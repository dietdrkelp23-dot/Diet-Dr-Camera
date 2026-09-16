# Frozen public preset contract

These are 1.0 reference artifacts, not presets to distribute to players.

- `preset-v1-defaults.json` records actual initialized/reset settings, including
  every registered camera profile and the persisted scalar members in the `.inc`
  inventories. Fresh-install and Reset All values must agree.
- `preset-v1-authored.toml` is a preset written through the production codec.
  It contains distinct framing, transitions, indoor and location snapshots,
  dialogue and animation IDs, weapon bindings, noise and first-person tuning.
- `preset-v1-authored.json` records its resolved values independently of sparse
  TOML serialization. Loading the TOML must reproduce this snapshot.

`PresetCompatibilityChecks` compiles the real SettingsManager and PresetManager.
Only engine notifications, camera reset and game-setting writes are stubbed.
The test also exercises repeated serialization, disabled tuning, key collisions,
Unicode filenames, description retention, global hotkeys, malformed/newer
formats, and failed file updates. It does not launch Skyrim.

**Do not regenerate these files to make a later release pass.** Fix the regression
or implement a versioned reader/migration that keeps the old fixture's meaning.
Add new-version fixtures separately. Newly added neutral entries may need new
comparison coverage, but existing reference values must remain intact.

The explicit `--record-v1` switch exists only to establish the initial unreleased
baseline; normal builds and packaging never invoke it. No fixture is installed
into the player archive. See `PRESET-COMPAT.md` for the maintenance contract.
