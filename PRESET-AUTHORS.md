# Sharing Diet Dr Camera presets

Create and save presets from the 1.0 release or later. Development presets are
outside the public compatibility contract.

1. In Presets, use **Save As** with a distinctive name. Save the completed tuning
   before copying the file. Test it by loading a different preset, loading yours
   again, and restarting Skyrim.
2. Package just `SKSE/Plugins/DietDrCamera/Presets/Your Preset.toml` in your ZIP.
   This is a Data-root layout, suitable for installing as a separate mod in a
   mod manager. Do not include `DietDrCamera.toml`, hotkeys, logs, or DDC binaries.
3. State the minimum DDC version and any mods used by your weapon, NPC,
   location, or animation bindings on the Nexus page. Use an original filename
   so your preset does not replace another author's work.
4. Tell users to select the preset in DDC after installation. Users who want to
   customize it should save a separate copy under their own name; your future
   preset updates can then replace your original without replacing their copy.

Presets store plugin names and local form IDs for bindings, and stable IDs for
dialogue looks and animation entries. Renaming or reordering looks through DDC
keeps their location bindings attached. A binding to an absent mod cannot match.

DDC updates contain no personal presets or generated camera configuration.
Settings omitted from a 1.0 preset retain their 1.0 defaults in later releases.
New features must preserve the behavior of earlier public presets. A build that
cannot read a newer preset format refuses to load or overwrite it. Do not edit
`[meta].format` to bypass that check.

Diet Dr Camera 1.0 writes format 7, with optional player Hit Shake's
Strength, Speed, Bounce and Texture in attack noise profiles. Strength defaults
to zero. Its tuning follows attack, weapon, POV and location entries. Format-6
Feel/Recovery tuning converts to similar first-kick strength, timing and rebound;
the new response has a different settling curve. See PRESET-COMPAT.md for the
conversion and its timing limits. Formats 1-5 keep Hit Shake off.
Projectile spell/staff hits use the same fields in Fire & Forget/Ritual entries,
including spell hand overrides and specific bindings (spell Casting; staff
Unsheathed/Sneaking). These controls affect direct missile actor contacts only;
concentration, lingering damage and splash-only hits do not trigger them.
Old presets gain no enabled magic Hit Shake, and the format remains 7.
Projectile impacts apply automatic distance falloff and a stronger first-person
gain at runtime. These do not rewrite the four saved controls or add preset keys.
Independent NPC Magic, Shouts, Melee, Archery and Transformations amounts remain
available for each view. Formats 1-6 still load.
Formats 1-3 copy the old Magic amount to Transformations; formats 1-4 copy it to
Shouts. Archery starts off in formats 1-3. Earlier builds refuse format-7 presets;
keep their format stamp intact. Specific melee weapons, bows/crossbows and shouts
use their bound noise entries for nearby NPCs too, including explicit zero mutes.

With MO2, newly saved files may be in Overwrite or your configured output mod.
Move the finished preset into its own mod before packaging. Keep a backup of
the exact TOML you published and check your own updates with that file.
