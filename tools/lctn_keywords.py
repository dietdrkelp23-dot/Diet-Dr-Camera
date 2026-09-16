#!/usr/bin/env python3
"""Dump LCTN records with their LocType keywords, to settle city-vs-town.

Reuses esm_scan.py's record-walking approach. Two passes:
  1. KYWD  -> {formID: EDID}
  2. LCTN  -> EDID + KWDA (array of keyword formIDs) resolved to EDIDs

Cross-plugin formIDs are resolved by mod-index remap against each plugin's
master list, which is what makes Update/Dawnguard overrides land correctly.
"""
import os
import struct
import sys
import zlib

DEFAULT_DATA = r"C:\Steam\steamapps\common\Skyrim Special Edition\Data"
PLUGINS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]
WANTED = {"KYWD", "LCTN"}

REC_HDR = struct.Struct("<4sIIIIHH")
SUB_HDR = struct.Struct("<4sH")
FLAG_COMPRESSED = 0x00040000


def parse_subrecords(data):
    subs, off, override, n = [], 0, None, len(data)
    while off + 6 <= n:
        stype, ssize = SUB_HDR.unpack_from(data, off)
        off += 6
        if stype == b"XXXX":
            override = struct.unpack_from("<I", data, off)[0]
            off += ssize
            continue
        if override is not None:
            ssize, override = override, None
        subs.append((stype, data[off:off + ssize]))
        off += ssize
    return subs


def zstring(b):
    return b.split(b"\0", 1)[0].decode("cp1252", "replace")


def masters_of(buf):
    """Master list from the TES4 header, in order."""
    out = []
    t, dsize = struct.unpack_from("<4sI", buf, 0)
    for stype, sdata in parse_subrecords(buf[24:24 + dsize]):
        if stype == b"MAST":
            out.append(zstring(sdata).lower())
    return out


def walk(buf, start, end, remap, sink):
    off = start
    while off + 24 <= end:
        if buf[off:off + 4] == b"GRUP":
            gsize, label, gtype = struct.unpack_from("<I4si", buf, off + 4)
            if gtype == 0 and label.decode("ascii", "replace") in WANTED:
                walk(buf, off + 24, off + gsize, remap, sink)
            off += gsize
            continue
        t, dsize, flags, formid, _v, _vr, _v2 = REC_HDR.unpack_from(buf, off)
        body = off + 24
        off = body + dsize
        tstr = t.decode("ascii", "replace")
        if tstr not in WANTED:
            continue
        data = buf[body:body + dsize]
        if flags & FLAG_COMPRESSED:
            try:
                data = zlib.decompress(data[4:])
            except zlib.error:
                continue
        sink(tstr, remap(formid), parse_subrecords(data))


def main():
    data_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATA
    keywords, locations = {}, {}

    for plugin in PLUGINS:
        path = os.path.join(data_dir, plugin)
        if not os.path.exists(path):
            print(f"## MISSING {path}")
            continue
        with open(path, "rb") as f:
            buf = f.read()
        masters = masters_of(buf)
        order = {m: i for i, m in enumerate(masters)}
        self_idx = len(masters)

        def remap(fid, order=order, self_idx=self_idx, masters=masters):
            # Local mod-index -> global index in PLUGINS order.
            mi = fid >> 24
            name = masters[mi].lower() if mi < len(masters) else plugin.lower()
            gi = next((i for i, p in enumerate(PLUGINS) if p.lower() == name), 0xFF)
            return (gi << 24) | (fid & 0x00FFFFFF)

        def sink(tstr, fid, subs):
            edid = ""
            kwda = []
            for stype, sdata in subs:
                if stype == b"EDID":
                    edid = zstring(sdata)
                elif stype == b"KWDA":
                    kwda = [remap(k) for (k,) in struct.iter_unpack("<I", sdata)]
            if tstr == "KYWD":
                keywords[fid] = edid
            else:
                locations[fid] = (edid, kwda)

        walk(buf, 24 + struct.unpack_from("<I", buf, 4)[0], len(buf), remap, sink)

    targets = ["whiterun", "windhelm", "solitude", "markarth", "riften",
               "falkreath", "morthal", "dawnstar", "winterhold",
               "riverwood", "rorikstead", "ivarstead", "shorswatch", "karthwasten"]
    print(f"# {len(keywords)} keywords, {len(locations)} locations\n")
    for fid, (edid, kwda) in sorted(locations.items(), key=lambda kv: kv[1][0]):
        low = edid.lower()
        if not any(t in low for t in targets):
            continue
        names = sorted(keywords.get(k, f"0x{k:08X}") for k in kwda)
        loc = [n for n in names if n.startswith("LocType")]
        if not loc:
            continue
        print(f"{edid:34s} 0x{fid & 0xFFFFFF:06X} p{fid >> 24} {' '.join(loc)}")


if __name__ == "__main__":
    main()
