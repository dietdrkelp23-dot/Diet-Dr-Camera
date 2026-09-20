"""Audit DDC's explicit IDs/vtables against private Address Library files.

This checks address availability only. It does not validate instructions, ABI,
implicit CommonLib calls, SKSE dependencies, or gameplay. No game files are copied.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct


def read_library(path):
    data = path.read_bytes()
    position = 0

    def read(fmt):
        nonlocal position
        values = struct.unpack_from("<" + fmt, data, position)
        position += struct.calcsize("<" + fmt)
        return values[0] if len(values) == 1 else values

    kind = read("i")
    version = read("4I")
    if kind == 5:
        read("64s")
        pointer_size, data_format, count = read("3i")
        if pointer_size != 8 or count < 0 or count > (len(data) - position) // 4:
            raise ValueError(f"Invalid v5 header: {path.name}")
        offsets = read(f"{count}I") if count else ()
        if count == 1:
            offsets = (offsets,)
        return version, kind, {key: value for key, value in enumerate(offsets) if value}
    if kind not in (1, 2):
        raise ValueError(f"Unsupported library format {kind}: {path.name}")
    name_size = read("i")
    if not 0 < name_size <= 4096:
        raise ValueError(f"Invalid library name: {path.name}")
    read(f"{name_size}s")
    pointer_size, count = read("2i")
    if pointer_size != 8 or count < 0 or count > len(data) - position:
        raise ValueError(f"Invalid library header: {path.name}")

    def delta(kind, previous):
        if kind == 0:
            return read("Q")
        if kind == 1:
            return previous + 1
        if kind in (2, 3, 4, 5):
            return previous + (1 if kind in (2, 4) else -1) * read("B" if kind < 4 else "H")
        if kind in (6, 7):
            return read("H" if kind == 6 else "I")
        raise ValueError(f"Invalid compressed entry type {kind}: {path.name}")

    result = {}
    key = offset = 0
    for _ in range(count):
        kinds = read("B")
        key = delta(kinds & 15, key)
        scale = pointer_size if kinds & 0x80 else 1
        offset = delta((kinds >> 4) & 7, offset // scale) * scale
        if key in result:
            raise ValueError(f"Duplicate ID {key}: {path.name}")
        result[key] = offset
    return version, kind, result


def source_inventory(root):
    # Ignore comments and ordinary string literals so examples/log messages do
    # not become apparent dependencies. The two current vtable wrappers follow.
    ignored = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)
    pair_pattern = re.compile(r'(?:RELOCATION_ID|RelocationID|VariantID)\s*[({]\s*(\d+)\s*,\s*(\d+)')
    inventory = {}
    raw_ids = []
    tables = {}
    sources = []
    for directory in ("src", "include"):
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp", ".inl"):
                continue
            relative = path.relative_to(root).as_posix()
            source = ignored.sub(" ", path.read_text(encoding="utf-8-sig"))
            sources.append(source)
            for a, b in pair_pattern.findall(source):
                inventory.setdefault((int(a), int(b)), set()).add(relative)
            for table, slot in re.findall(r'RE::(VTABLE_\w+)\s*\[\s*(\d+)\s*\]', source):
                tables.setdefault((table, int(slot)), set()).add(relative)
            for handler in re.findall(r'\bDDC_PATCH\((\w+)\)\s*;', source):
                tables.setdefault(("VTABLE_" + handler, 0), set()).add(relative)
            for effect in re.findall(r'EffectObserver<RE::(\w+)>::Install\(\)', source):
                tables.setdefault(("VTABLE_" + effect, 0), set()).add(relative)
            for raw in re.findall(r'REL::(?:ID|Offset)\s*[({]\s*(\d+)', source):
                raw_ids.append({"file": relative, "value": int(raw),
                    "dormant": relative == "src/Unpause/MenuViewCache.cpp"})
    if re.search(r'MenuViewCache\s*::\s*Install\s*\(', "\n".join(sources)):
        for item in raw_ids:
            if item["file"] == "src/Unpause/MenuViewCache.cpp":
                item["dormant"] = False
    definitions = (root / "extern/CommonLibSSE-NG/include/RE/Offsets_VTABLE.h").read_text()
    for (table, slot), sources in tables.items():
        match = re.search(r'\b' + re.escape(table) + r'\s*\{([^;]+)\}', definitions)
        if not match:
            raise ValueError(f"Unresolved vtable {table}")
        entries = pair_pattern.findall(match[1])
        if slot >= len(entries):
            raise ValueError(f"Unresolved vtable slot {table}[{slot}]")
        pair = tuple(map(int, entries[slot]))
        inventory.setdefault(pair, set()).update(sources | {f"{table}[{slot}]"})
    return inventory, raw_ids


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--libraries", type=Path, action="append", required=True,
                        help="Directory of released .bin files; repeat for multiple directories")
    parser.add_argument("--csv-directory", type=Path, help="Optional canonical offsets-1-7-*.csv mappings")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    inventory, raw_ids = source_inventory(args.source)
    reports = []

    def record(path, version, kind, offsets):
        wanted = {pair[version >= (1, 6, 0, 0)] for pair in inventory}
        missing = sorted(key for key in wanted if not key or not offsets.get(key))
        reports.append({"runtime": ".".join(map(str, version)), "file": path.name,
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        "format": kind, "ids": len(wanted), "missing": missing})
        print(f"{reports[-1]['runtime']}: {len(wanted)} explicit IDs; " +
              (f"MISSING {missing}" if missing else "all present") + f" ({kind})")

    files = sorted({path.resolve() for directory in args.libraries for path in directory.glob("*.bin")})
    if not files:
        raise ValueError("No Address Library files found")
    for path in files:
        version, kind, offsets = read_library(path)
        record(path, version, kind, offsets)
    if args.csv_directory:
        for path in sorted(args.csv_directory.glob("offsets-1-7-*.csv")):
            match = re.fullmatch(r'offsets-(\d+)-(\d+)-(\d+)\.(\d+)\.csv', path.name)
            if not match:
                continue
            with path.open(newline="", encoding="utf-8-sig") as stream:
                offsets = {int(row["aeid"]): int(row["ae_addr"], 16) for row in csv.DictReader(stream)}
            record(path, tuple(map(int, match.groups())), "canonical CSV (not a released binary)", offsets)
    known_source = (args.source / "include/Hooks/RuntimeVersion.h").read_text()
    known = {".".join(parts) for parts in re.findall(r'Version\{(\d+),(\d+),(\d+),(\d+)\}', known_source)}
    absent = sorted(known - {item["runtime"] for item in reports})
    result = {"scope": __doc__.strip(), "libraries": reports, "runtimesWithoutMappings": absent,
              "singleRuntimeReferences": raw_ids,
              "inventory": [{"se": a, "ae": b, "references": sorted(refs)}
                            for (a, b), refs in sorted(inventory.items())]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Known runtimes without mappings: {absent}; single-runtime references: {raw_ids}")
    return int(bool(absent) or any(item["missing"] for item in reports) or any(not item["dormant"] for item in raw_ids))


if __name__ == "__main__":
    raise SystemExit(main())
