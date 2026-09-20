"""Run offline hook scenarios for every loader-listed runtime; never launch Skyrim.

Requires Python with pefile and a built RuntimeLayoutChecks.exe. Executable
fixtures remain private. Missing versions produce exit 2 unless --allow-missing
is explicitly supplied; a failed check always produces exit 1.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import pefile

SCENARIOS = (
    'clean', 'camera-chain', 'furniture-chain', 'camera-wrong-target',
    'ui-driver-conflict', 'main-call-conflict', 'dialogue-call-conflict',
    'ui-job-install', 'call-changed-after-preflight', 'ui-changed-after-preflight',
)


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def image_version(path):
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories([pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
        versions = {table.entries[b'ProductVersion'].decode().strip()
                    for group in pe.FileInfo for info in group if hasattr(info, 'StringTable')
                    for table in info.StringTable if b'ProductVersion' in table.entries}
        if len(versions) != 1:
            raise ValueError(f'No unique ProductVersion in {path}')
        return versions.pop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--images', type=Path, action='append', required=True)
    parser.add_argument('--libraries', type=Path, action='append', required=True)
    parser.add_argument('--checker', type=Path, default=Path('build/release-candidate/Release/RuntimeLayoutChecks.exe'))
    parser.add_argument('--report', type=Path, required=True, help='New evidence directory; existing directories are refused')
    parser.add_argument('--scenarios', nargs='+', choices=SCENARIOS, default=list(SCENARIOS))
    parser.add_argument('--allow-missing', action='store_true')
    args = parser.parse_args()
    checker = args.checker.resolve(strict=True)
    source = args.source.resolve(strict=True)
    versions = [tuple(map(int, match)) for match in re.findall(
        r'Version\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}',
        (source / 'include/Hooks/RuntimeVersion.h').read_text())]
    if not versions or len(versions) != len(set(versions)):
        raise ValueError('Loader runtime list is empty or contains duplicate versions')
    report = args.report.resolve()
    report.mkdir(parents=True, exist_ok=False)
    records = []
    for version in versions:
        runtime = '.'.join(map(str, version))
        short = '.'.join(map(str, version[:3]))
        item = dict(runtime=runtime, status='not-tested', scenarios=[], gameplay='not-run')
        records.append(item)
        images = [root / f'SkyrimSE-{short}.exe.unpacked.exe' for root in args.images]
        images += [root / f'SkyrimSE-{short}.exe' for root in args.images]
        image = next((p.resolve() for p in images if p.is_file()), None)
        library_name = ('versionlib-' if version >= (1,6,0,0) else 'version-') + '-'.join(map(str,version)) + '.bin'
        library = next((root / library_name for root in args.libraries if (root / library_name).is_file()), None)
        if image is None or library is None:
            item['status'] = 'missing-image' if image is None else 'missing-address-library'
            print(f'{runtime}: {item["status"]}', flush=True)
            continue
        try:
            library = library.resolve()
            if image_version(image) != runtime:
                raise ValueError('Fixture ProductVersion differs from its matrix row')
            with library.open('rb') as stream:
                kind, *actual = struct.unpack('<5I', stream.read(20))
            if kind not in (1,2,5) or tuple(actual) != version:
                raise ValueError('Address Library header does not match the executable')
            item.update(image=str(image), imageSha256=sha256(image),
                        addressLibrary=str(library), addressLibrarySha256=sha256(library))
            for scenario in args.scenarios:
                log = report / f'{runtime}-{scenario}.log'
                with log.open('w', encoding='utf-8') as output:
                    try:
                        result = subprocess.run([str(checker), '--image', str(image), str(library), scenario],
                                                stdout=output, stderr=subprocess.STDOUT, timeout=30)
                        exit_code = result.returncode
                    except subprocess.TimeoutExpired:
                        exit_code = 'timeout'
                output = log.read_text(encoding='utf-8', errors='replace')
                marker = (f'Complete hook preflight passed against mapped Skyrim {"-".join(map(str,version))}'
                          if scenario == 'clean' else f'Runtime scenario passed: {scenario}')
                passed = exit_code == 0 and marker in output
                item['scenarios'].append(dict(scenario=scenario, passed=passed, exitCode=exit_code, log=log.name))
                slots = re.search(r'(\d+) hooked vtable targets passed', output)
                if slots:
                    item['vtableTargetsChecked'] = int(slots[1])
            item['status'] = 'offline-pass' if all(s['passed'] for s in item['scenarios']) else 'failed'
        except (OSError, ValueError, struct.error, pefile.PEFormatError, AttributeError) as error:
            item.update(status='failed', error=str(error))
        print(f'{runtime}: {item["status"]}; {sum(s["passed"] for s in item["scenarios"])}/{len(args.scenarios)} scenarios', flush=True)
    failures = sum(r['status'] == 'failed' for r in records)
    missing = sum(r['status'].startswith('missing-') for r in records)
    sources = [source / path for path in (
        'src/Core/Diagnostics.cpp', 'include/Core/Diagnostics.h',
        'src/Hooks/RuntimeHooks.cpp', 'src/Hooks/RuntimePatchInspection.cpp',
        'include/Hooks/RuntimeVersion.h', 'tests/RuntimeImageCheck.h',
        'tests/RuntimeImageScenario.h', 'tests/RuntimeLayoutChecks.cpp')]
    result = dict(testedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  scope='Offline production hook checks and native vtable target addresses; no game code executed',
                  checker=str(checker), checkerSha256=sha256(checker),
                  sourceSha256={p.relative_to(source).as_posix():sha256(p) for p in sources},
                  passed=sum(r['status'] == 'offline-pass' for r in records), failed=failures,
                  missing=missing, complete=not failures and not missing, runtimes=records)
    (report / 'matrix.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    lines = ['# Offline runtime matrix', '', result['scope'], '',
             '| Runtime | Result | Scenarios passed | Vtable targets | Gameplay |',
             '| --- | --- | --- | --- | --- |']
    for item in records:
        lines.append(f'| {item["runtime"]} | {item["status"]} | '
                     f'{sum(s["passed"] for s in item["scenarios"])}/{len(args.scenarios)} | '
                     f'{item.get("vtableTargetsChecked", "not checked")} | not run |')
    (report / 'matrix.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(f'Matrix: {result["passed"]} passed, {failures} failed, {missing} missing; gameplay not tested.')
    return 1 if failures else 2 if missing and not args.allow_missing else 0


if __name__ == '__main__':
    raise SystemExit(main())
