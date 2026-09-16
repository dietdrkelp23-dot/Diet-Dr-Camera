#!/usr/bin/env python3
"""esm_scan.py — enumerate vanilla records for DDC's event-beat form tables.

Walks the Bethesda ESMs record-by-record (top-level GRUPs filtered by record
type, compressed records inflated) and prints every record matching the
category filters below as:

    category  plugin  RECTYPE  0xLOCALID  EDID  [MODL]

The output is curated by hand into the constexpr {plugin, localID} tables in
src/ (house style: hand-checked tables with provenance comments). Re-run after
a game update to re-verify. Only records DEFINED by each plugin are listed
(mod-index byte == master count), so the local IDs are directly usable with
TESDataHandler::LookupForm(localID, plugin).

Usage:
    python tools/esm_scan.py [data_dir]
        data_dir defaults to the Steam SSE install's Data directory.
"""

import os
import struct
import sys
import zlib

DEFAULT_DATA = r"C:\Steam\steamapps\common\Skyrim Special Edition\Data"
PLUGINS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]

# category -> (record types, EDID/MODL substrings (lowercase), also-match-MODL)
CATEGORIES = {
    "coffin_furn":    ({"FURN"}, ["coffin", "sarcoph"], True),
    "coffin_acti":    ({"ACTI", "CONT"}, ["coffin", "sarcoph"], True),
    "ambush_furn":    ({"FURN"}, ["ambush", "draugr"], True),
    "lurker_furn":    ({"FURN"}, ["lurker"], True),
    "standing_stone": ({"ACTI"}, ["doomstone"], False),
    "word_wall":      ({"ACTI"}, ["wordwall"], False),
    "black_book":     ({"BOOK", "ACTI"}, ["blackbook"], False),
    "eye_magnus":     ({"ACTI", "MSTT", "STAT"}, ["eyeofmagnus", "magnuseye", "mg07eye", "mgreye"], True),
    "soul_cairn":     ({"ACTI", "MSTT", "STAT"}, ["cairn", "portal"], False),
    "magelight":      ({"ACTI", "MSTT", "LIGH"}, ["magelight", "mglight", "collegelight", "collegemage"], True),
    "fire_static":    ({"ACTI", "MSTT", "STAT"}, ["brazier", "campfire", "firepit", "bonfire", "embers", "fireplace"], True),
    "dragon_mound":   ({"ACTI", "STAT", "FURN"}, ["dragonmound", "burialmound"], True),
    "hazard":         ({"HAZD"}, [], False),
    "explosion":      ({"EXPL"}, [], False),
    "imad":           ({"IMAD"}, ["wordwall", "elderscroll", "scroll", "absorb", "blackbook", "dragonsoul",
                                  "learn", "apocrypha", "soultrap"], False),
    "divine":         ({"NPC_", "TACT"}, ["augur", "barbas", "meridia", "peryite", "hircine", "boethiah",
                                          "hermaeus", "hermamora", "hermodamora", "nocturnal", "azura",
                                          "clavicus", "malacath", "mephala", "molag", "namira", "sanguine",
                                          "sheogorath", "vaermina"], False),
}

WANTED_TYPES = set()
for types, _, _ in CATEGORIES.values():
    WANTED_TYPES |= types

REC_HDR = struct.Struct("<4sIIIIHH")  # type, dataSize, flags, formID, vc, version, vc2
SUB_HDR = struct.Struct("<4sH")
FLAG_COMPRESSED = 0x00040000


def parse_subrecords(data):
    subs = []
    off = 0
    override = None
    n = len(data)
    while off + 6 <= n:
        stype, ssize = SUB_HDR.unpack_from(data, off)
        off += 6
        if stype == b"XXXX":
            override = struct.unpack_from("<I", data, off)[0]
            off += ssize
            continue
        if override is not None:
            ssize = override
            override = None
        subs.append((stype, data[off:off + ssize]))
        off += ssize
    return subs


def zstring(b):
    return b.split(b"\0", 1)[0].decode("cp1252", "replace")


def walk_group(buf, start, end, master_count, plugin, out):
    off = start
    while off + 24 <= end:
        rtype = buf[off:off + 4]
        if rtype == b"GRUP":
            gsize, label, gtype = struct.unpack_from("<I4si", buf, off + 4)
            inner_end = off + gsize
            # Only descend groups that can contain wanted top-level records:
            # type 0 = record-type group. Cell/world children (types 1..10)
            # never hold the base records we table.
            if gtype == 0 and label.decode("ascii", "replace") in WANTED_TYPES:
                walk_group(buf, off + 24, inner_end, master_count, plugin, out)
            off = inner_end
            continue
        t, dsize, flags, formid, _vc, _ver, _vc2 = REC_HDR.unpack_from(buf, off)
        body_off = off + 24
        off = body_off + dsize
        tstr = t.decode("ascii", "replace")
        if tstr not in WANTED_TYPES:
            continue
        if (formid >> 24) != master_count:
            continue  # override of another plugin's record — not defined here
        data = buf[body_off:body_off + dsize]
        if flags & FLAG_COMPRESSED:
            try:
                data = zlib.decompress(data[4:])
            except zlib.error:
                continue
        edid = ""
        modl = ""
        for stype, sdata in parse_subrecords(data):
            if stype == b"EDID":
                edid = zstring(sdata)
            elif stype == b"MODL" and not modl:
                modl = zstring(sdata)
        low_e = edid.lower()
        low_m = modl.lower()
        local_id = formid & 0x00FFFFFF
        for cat, (types, needles, use_modl) in CATEGORIES.items():
            if tstr not in types:
                continue
            if needles:
                hit = any(nd in low_e for nd in needles)
                if not hit and use_modl:
                    hit = any(nd in low_m for nd in needles)
                if not hit:
                    continue
            out.append((cat, plugin, tstr, local_id, edid, modl))


def scan_plugin(path, plugin, out):
    with open(path, "rb") as f:
        buf = f.read()
    # TES4 header record first — count MAST subrecords for the mod index.
    t, dsize, flags, _fid, _vc, _ver, _vc2 = REC_HDR.unpack_from(buf, 0)
    assert t == b"TES4", f"{plugin}: not a TES4 plugin"
    tes4 = buf[24:24 + dsize]
    if flags & FLAG_COMPRESSED:
        tes4 = zlib.decompress(tes4[4:])
    master_count = sum(1 for stype, _ in parse_subrecords(tes4) if stype == b"MAST")
    walk_group(buf, 24 + dsize, len(buf), master_count, plugin, out)


def main():
    data_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATA
    out = []
    for plugin in PLUGINS:
        path = os.path.join(data_dir, plugin)
        if not os.path.isfile(path):
            print(f"## MISSING {path}", file=sys.stderr)
            continue
        scan_plugin(path, plugin, out)
    out.sort()
    cur = None
    for cat, plugin, tstr, local_id, edid, modl in out:
        if cat != cur:
            print(f"\n== {cat} ==")
            cur = cat
        extra = f"\t{modl}" if modl else ""
        print(f"{plugin}\t{tstr}\t0x{local_id:06X}\t{edid}{extra}")


if __name__ == "__main__":
    main()
