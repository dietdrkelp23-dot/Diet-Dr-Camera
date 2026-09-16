"""
Generates a TOML preset file with a unique, non-default value in every
profile slot so the user can verify the picker resolves to the correct
profile in every state without manually tuning ~700 entries.

Run:
    python tools/gen_test_preset.py --output build/Test_All_Customized.toml

The output is self-contained — load it via the in-game Presets panel.
"""

import hashlib
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True, help='Destination TOML file (choose a separate test preset)')
OUT = parser.parse_args().output
OUT.parent.mkdir(parents=True, exist_ok=True)
format_header = (Path(__file__).resolve().parents[1] / 'include/Settings/PresetFormat.h').read_text(encoding='utf-8')
PRESET_FORMAT = int(re.search(r'kCurrentPresetFormat\s*=\s*(\d+)', format_header).group(1))

# ------------------------------------------------------------------------
# Value generator. Stable hash → 6 floats per profile in safe ranges.
# ------------------------------------------------------------------------
def profile_for(seed: str):
    h = int(hashlib.md5(seed.encode()).hexdigest(), 16)
    side   = ((h % 41)  - 20)       ; h //= 41    # -20..20
    height = ((h % 21)  - 10)       ; h //= 21    # -10..10
    zoom   = ((h % 31)  - 15)       ; h //= 31    # -15..15
    fov    =  72 + (h % 25)         ; h //= 25    # 72..96
    rot    = ((h % 25)  - 12)       ; h //= 25    # -12..12
    pitch  = ((h % 17)  - 8)        ; h //= 17    # -8..8
    return (float(side), float(height), float(zoom), float(fov), float(rot), float(pitch))

def emit(buf, key, seed=None):
    s, h, z, f, r, p = profile_for(seed or key)
    buf.append(f"\n[{key}]")
    buf.append(f"side_offset  = {s}")
    buf.append(f"height       = {h}")
    buf.append(f"zoom_offset  = {z}")
    buf.append(f"fov          = {f}")
    buf.append(f"rotation     = {r}")
    buf.append(f"pitch_offset = {p}")

# ------------------------------------------------------------------------
# Profile keys — mirror the Save/Load layout exactly.
# ------------------------------------------------------------------------
SCHOOLS = ["alteration", "conjuration", "destruction", "illusion", "restoration"]
CASTTYPES_MAGIC  = ["concentration", "fire_and_forget", "ritual"]
CASTTYPES_STAVES = ["concentration", "fire_and_forget", "ritual"]

SHOUTS = [
    "animal_allegiance","aura_whisper","battle_fury","become_ethereal","bend_will",
    "call_dragon","call_of_valor","clear_skies","cyclone","disarm","dismay",
    "dragon_aspect","dragonrend","drain_vitality","elemental_fury","fire_breath",
    "frost_breath","ice_form","kynes_peace","marked_for_death","slow_time",
    "soul_tear","storm_call","summon_durnehviir","throw_voice","unrelenting_force",
    "whirlwind_sprint",
]

SHOUT_STATES = ["sheathed","melee","bow","crossbow","magic","staves"]

ENEMY_KEYS = ["dragons","giants","mammoths","centurions","lurkers"]

# All TL slot keys. Mirror DDC_TL_SLOT_LIST order.
TL_SLOT_KEYS = [
    "sheathed","sheathed.sprint","sheathed.swim","sheathed.sneak",
    "weapons.melee","weapons.melee.sprint","weapons.melee.swim","weapons.melee.attack","weapons.melee.sneak",
    "weapons.blocking",
    "weapons.bow","weapons.bow.sprint","weapons.bow.swim","weapons.bow.draw","weapons.bow.sneak","weapons.bow.sneak.draw",
    "weapons.crossbow","weapons.crossbow.sprint","weapons.crossbow.swim","weapons.crossbow.draw","weapons.crossbow.sneak","weapons.crossbow.sneak.draw",
    "weapons.magic","weapons.magic.sprint","weapons.magic.swim","weapons.magic.sneak",
]
for sch in SCHOOLS:
    for ct in CASTTYPES_MAGIC:
        TL_SLOT_KEYS.append(f"magic.{sch}.{ct}")
for sch in SCHOOLS:
    for ct in CASTTYPES_MAGIC:
        TL_SLOT_KEYS.append(f"magic.{sch}.sneak.{ct}")
TL_SLOT_KEYS += [
    "weapons.staves","weapons.staves.sprint","weapons.staves.swim","weapons.staves.sneak",
]
for sch in SCHOOLS:
    for ct in CASTTYPES_STAVES:
        TL_SLOT_KEYS.append(f"staves.{sch}.{ct}")
for sch in SCHOOLS:
    for ct in CASTTYPES_STAVES:
        TL_SLOT_KEYS.append(f"staves.{sch}.sneak.{ct}")
TL_SLOT_KEYS += [
    "transformations.werewolf","transformations.werewolf.sprint","transformations.werewolf.swim",
    "transformations.vampire_lord.sheathed","transformations.vampire_lord.sprint",
    "transformations.vampire_lord.magic","transformations.vampire_lord.concentration",
    "transformations.vampire_lord.fire_and_forget","transformations.vampire_lord.melee",
    "mounts.horseback","mounts.horseback.sprint","mounts.horseback.swim","mounts.horseback.melee","mounts.horseback.archery",
    "mounts.dragon_riding",
]
# Per-state shouts base keys (also part of TL slot list)
for st in SHOUT_STATES:
    TL_SLOT_KEYS.append(f"shouts.base.{st}")
for st in SHOUT_STATES:
    TL_SLOT_KEYS.append(f"shouts.base.{st}.sneak")

# Indoor-eligible profile keys: same as Categories + Shouts (NO target-lock,
# vanity, dialogue). The actual indoor.<key> namespace mirrors these.
INDOOR_OUTDOOR_KEYS = [
    "sheathed","sheathed.sprint","sheathed.swim","sheathed.sneak",
    "weapons.melee","weapons.melee.sprint","weapons.melee.swim","weapons.melee.attack","weapons.melee.sneak",
    "weapons.blocking",
    "weapons.bow","weapons.bow.sprint","weapons.bow.swim","weapons.bow.draw","weapons.bow.sneak","weapons.bow.sneak.draw",
    "weapons.crossbow","weapons.crossbow.sprint","weapons.crossbow.swim","weapons.crossbow.draw","weapons.crossbow.sneak","weapons.crossbow.sneak.draw",
    "weapons.magic","weapons.magic.sprint","weapons.magic.swim","weapons.magic.sneak",
]
for sch in SCHOOLS:
    for ct in CASTTYPES_MAGIC:
        INDOOR_OUTDOOR_KEYS.append(f"magic.{sch}.{ct}")
for sch in SCHOOLS:
    for ct in CASTTYPES_MAGIC:
        INDOOR_OUTDOOR_KEYS.append(f"magic.{sch}.sneak.{ct}")
INDOOR_OUTDOOR_KEYS += [
    "weapons.staves","weapons.staves.sprint","weapons.staves.swim","weapons.staves.sneak",
]
for sch in SCHOOLS:
    for ct in CASTTYPES_STAVES:
        INDOOR_OUTDOOR_KEYS.append(f"staves.{sch}.{ct}")
for sch in SCHOOLS:
    for ct in CASTTYPES_STAVES:
        INDOOR_OUTDOOR_KEYS.append(f"staves.{sch}.sneak.{ct}")
INDOOR_OUTDOOR_KEYS += [
    "transformations.werewolf","transformations.werewolf.sprint","transformations.werewolf.swim",
    "transformations.vampire_lord.sheathed","transformations.vampire_lord.melee",
    "transformations.vampire_lord.magic","transformations.vampire_lord.concentration",
    "transformations.vampire_lord.fire_and_forget","transformations.vampire_lord.sprint",
    "mounts.horseback","mounts.horseback.sprint","mounts.horseback.swim","mounts.horseback.melee","mounts.horseback.archery",
    "mounts.dragon_riding",
]
# Shouts base + per-shout overrides
for st in SHOUT_STATES:
    INDOOR_OUTDOOR_KEYS.append(f"shouts.base.{st}")
    INDOOR_OUTDOOR_KEYS.append(f"shouts.base.{st}.sneak")
    for s in SHOUTS:
        INDOOR_OUTDOOR_KEYS.append(f"shouts.override.{st}.{SHOUTS.index(s)}")
        INDOOR_OUTDOOR_KEYS.append(f"shouts.override.{st}.sneak.{SHOUTS.index(s)}")

# ------------------------------------------------------------------------
# Build the TOML.
# ------------------------------------------------------------------------
buf: list[str] = []

# [meta] description
buf.append("[meta]")
buf.append(f"format = {PRESET_FORMAT}")
buf.append("description = '''Test preset with every profile customized to a")
buf.append("unique, deterministic value. Generated by gen_test_preset.py.")
buf.append("Loading this preset should produce visibly different camera")
buf.append("framing in every state — switching weapons, mounting a horse,")
buf.append("walking inside, locking onto a target, etc.'''")

# [general]
buf.append("\n[general]")
buf.append("camera_collision = false")           # engine collision active (recommended default)
buf.append("adaptive_collision = false")         # leave off so the user can test it independently
buf.append("adaptive_intensity = 1.0")
buf.append("adaptive_max_adjust = 1.0")
buf.append("transition_base_speed = 0.7")
buf.append("transition_mul_rotation = 1.8")
buf.append("transition_mul_pitch = 1.6")
buf.append("transition_mul_position = 1.1")
buf.append("transition_mul_zoom = 0.9")
buf.append("transition_mul_fov = 0.85")
buf.append("first_person_world_fov = 88.0")
buf.append("first_person_hands_fov = 70.0")
buf.append("death_camera_fov = 100.0")
buf.append("death_camera_hold_duration = 12.0")
buf.append("death_camera_free_look = true")
buf.append("dialogue_enabled = true")
buf.append("dialogue_first_person_enabled = true")
buf.append("dialogue_movement_enabled = true")
# Live state — leave indoor tab on Outdoor so the user starts on the
# familiar set; they can flip to Indoor to verify the other half.
# categories_edit_tab is omitted (defaults to 0).

# [dialogue] + [dialogue.first_person]
emit(buf, "dialogue")
emit(buf, "dialogue.first_person")

# [extras.vanity]
emit(buf, "extras.vanity")

# [workstation] + [furniture]
buf.append("\n[workstation]")
buf.append("forge       = 86.0")
buf.append("workbench   = 84.0")
buf.append("grindstone  = 88.0")
buf.append("smelter     = 92.0")
buf.append("alchemy     = 78.0")
buf.append("enchanting  = 82.0")
buf.append("cooking     = 90.0")
buf.append("tanning     = 76.0")

buf.append("\n[furniture]")
buf.append("chair = 95.0")
buf.append("bed   = 70.0")
buf.append("other = 88.0")

# [noise] — every context tuned to be distinguishable
buf.append("\n[noise]")
buf.append("enabled = true")
contexts = [
    ("idle",            (0.4, 0.5, 1.0, 1.0, 1.0)),
    ("walking",         (0.7, 0.9, 1.1, 1.0, 1.2)),
    ("running",         (1.1, 1.2, 1.0, 1.0, 1.3)),
    ("sprinting",       (1.8, 1.5, 1.2, 1.1, 1.4)),
    ("sneak",           (0.3, 0.6, 1.0, 1.0, 0.9)),
    ("airborne",        (1.0, 1.4, 1.5, 0.8, 1.0)),
    ("mounted",         (1.6, 0.9, 1.0, 1.2, 0.9)),
    ("fire_and_forget", (1.4, 2.2, 1.3, 0.9, 1.5)),
    ("concentration",   (0.9, 1.6, 1.1, 1.0, 1.2)),
]
for name, (amp, spd, t, r, f) in contexts:
    buf.append(f"{name}_amp   = {amp}")
    buf.append(f"{name}_speed = {spd}")
    buf.append(f"{name}_trans = {t}")
    buf.append(f"{name}_rot   = {r}")
    buf.append(f"{name}_freq  = {f}")

# Categories profiles
emit(buf, "sheathed")
emit(buf, "sheathed.sprint")
emit(buf, "sheathed.swim")
emit(buf, "sheathed.sneak")

emit(buf, "weapons.melee")
emit(buf, "weapons.melee.sprint")
emit(buf, "weapons.melee.swim")
emit(buf, "weapons.melee.attack")
emit(buf, "weapons.melee.sneak")

emit(buf, "weapons.blocking")

for w in ["bow", "crossbow"]:
    emit(buf, f"weapons.{w}")
    emit(buf, f"weapons.{w}.sprint")
    emit(buf, f"weapons.{w}.swim")
    emit(buf, f"weapons.{w}.draw")
    emit(buf, f"weapons.{w}.sneak")
    emit(buf, f"weapons.{w}.sneak.draw")

emit(buf, "weapons.magic")
emit(buf, "weapons.magic.sprint")
emit(buf, "weapons.magic.swim")
emit(buf, "weapons.magic.sneak")

# Magic schools use the production codec, including sneaking variants.
for sch in SCHOOLS:
    for ct in CASTTYPES_MAGIC:
        emit(buf, f"weapons.magic.{sch}.{ct}")
        emit(buf, f"weapons.magic.{sch}.sneak.{ct}")

emit(buf, "weapons.staves")
emit(buf, "weapons.staves.sprint")
emit(buf, "weapons.staves.swim")
emit(buf, "weapons.staves.sneak")
for sch in SCHOOLS:
    for ct in CASTTYPES_STAVES:
        emit(buf, f"weapons.staves.{sch}.{ct}")
        emit(buf, f"weapons.staves.{sch}.sneak.{ct}")

# Transformations
emit(buf, "transformations.werewolf")
emit(buf, "transformations.vampire_lord")  # legacy single-blanket key
emit(buf, "transformations.vampire_lord.sheathed")
emit(buf, "transformations.vampire_lord.melee")
emit(buf, "transformations.vampire_lord.magic")
emit(buf, "transformations.vampire_lord.concentration")
emit(buf, "transformations.vampire_lord.fire_and_forget")

# Mounts
emit(buf, "mounts.horseback")
emit(buf, "mounts.horseback.melee")
emit(buf, "mounts.horseback.archery")
emit(buf, "mounts.dragon_riding")

# Shouts — per-state base + per-shout overrides
for st in SHOUT_STATES:
    base = f"shouts.{st}"
    emit(buf, f"{base}.base", seed=f"{base}.base")
    emit(buf, f"{base}.base.sneak", seed=f"{base}.base.sneak")
    for shout in SHOUTS:
        # Override profile (with enabled flag to actually engage it)
        s, h, z, f, r, p = profile_for(f"{base}.{shout}")
        buf.append(f"\n[{base}.{shout}]")
        buf.append(f"enabled      = true")
        buf.append(f"side_offset  = {s}")
        buf.append(f"height       = {h}")
        buf.append(f"zoom_offset  = {z}")
        buf.append(f"fov          = {f}")
        buf.append(f"rotation     = {r}")
        buf.append(f"pitch_offset = {p}")
        s, h, z, f, r, p = profile_for(f"{base}.{shout}.sneak")
        buf.append(f"\n[{base}.{shout}.sneak]")
        buf.append(f"enabled      = true")
        buf.append(f"side_offset  = {s}")
        buf.append(f"height       = {h}")
        buf.append(f"zoom_offset  = {z}")
        buf.append(f"fov          = {f}")
        buf.append(f"rotation     = {r}")
        buf.append(f"pitch_offset = {p}")

# Target Lock — full TL slot tree
for k in TL_SLOT_KEYS:
    storage_key = f"weapons.{k}" if k.startswith(("magic.", "staves.")) else k
    emit(buf, f"target_lock.{storage_key}", seed=f"target_lock.{k}")

# Target Lock bias
buf.append("\n[target_lock.bias]")
buf.append("aim_bias        = 0.6")
buf.append("acquire_seconds = 0.18")

# TL per-shout overrides
for st in SHOUT_STATES:
    for shout in SHOUTS:
        for variant in ["", ".sneak"]:
            seed = f"target_lock.shouts.{st}.{shout}{variant}"
            s, h, z, f, r, p = profile_for(seed)
            buf.append(f"\n[target_lock.shouts.{st}.{shout}{variant}]")
            buf.append(f"enabled      = true")
            buf.append(f"side_offset  = {s}")
            buf.append(f"height       = {h}")
            buf.append(f"zoom_offset  = {z}")
            buf.append(f"fov          = {f}")
            buf.append(f"rotation     = {r}")
            buf.append(f"pitch_offset = {p}")

# Enemy overrides — enable a couple of channels per enemy so the user
# can verify the splice
for ek in ENEMY_KEYS:
    buf.append(f"\n[target_lock.enemy.{ek}.bias]")
    buf.append("aim_bias         = 0.55")
    buf.append("aim_bias_enabled = true")
    for k in TL_SLOT_KEYS:
        seed = f"enemy.{ek}.{k}"
        s, h, z, f, r, p = profile_for(seed)
        buf.append(f"\n[target_lock.enemy.{ek}.{k}]")
        buf.append(f"side_offset  = {s}")
        buf.append(f"height       = {h}")
        buf.append(f"zoom_offset  = {z}")
        buf.append(f"fov          = {f}")
        buf.append(f"rotation     = {r}")
        buf.append(f"pitch_offset = {p}")
        buf.append(f"side_offset_enabled  = true")
        buf.append(f"height_enabled       = false")
        buf.append(f"zoom_enabled         = true")
        buf.append(f"fov_enabled          = false")
        buf.append(f"rotation_enabled     = false")
        buf.append(f"pitch_offset_enabled = true")

# Indoor variants — distinct values from outdoor (different seed prefix)
for k in INDOOR_OUTDOOR_KEYS:
    emit(buf, f"indoor.{k}", seed=f"indoor::{k}")
# Indoor TL variants
for k in TL_SLOT_KEYS:
    emit(buf, f"indoor.target_lock.{k}", seed=f"indoor::target_lock.{k}")

# ------------------------------------------------------------------------
# Write
# ------------------------------------------------------------------------
OUT.write_text("\n".join(buf) + "\n", encoding="utf-8")
print(f"Wrote {OUT}")
print(f"Lines: {sum(1 for _ in (OUT).read_text().splitlines())}")
