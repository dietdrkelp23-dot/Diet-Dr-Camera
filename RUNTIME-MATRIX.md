# Runtime test matrix — 1.2.0, September 19, 2026

The 1.2.0 release candidate repeated the offline matrix. All **21 Steam executable
targets pass 210 offline scenarios** and checks of **47 hooked vtable targets
per executable**. The Release build and all **41 CTests** pass, including the
subsequent startup-logging checks. The exact older
runtime/mod combination behind the reported RuntimeHooks.cpp(24) error remains
unknown; that line alone does not identify a failed check.

Current evidence: `build/diagnostics/release-1.2.0-20260919/runtime-matrix/`.
The two missing GOG images remain missing and produce the runner's explicit
incomplete-coverage exit code 2. No scenario failed.

An earlier logging build repeated this matrix and both main-menu startup checks.
See [that build's evidence](RUNTIME-TESTING.md#startup-logging-verification-september-19-2026)
for its DLL hash and logs. The startup results below belong to that earlier
build; the final packaged 1.2.0 DLL has not been launched in game.

| Runtime | Offline scenarios | Hooked vtable targets | Earlier logging-build startup | Gameplay |
| --- | --- | --- | --- | --- |
| 1.5.3 | 10/10 | 47/47 | Not run | Not run |
| 1.5.16 | 10/10 | 47/47 | Not run | Not run |
| 1.5.23 | 10/10 | 47/47 | Not run | Not run |
| 1.5.39 | 10/10 | 47/47 | Not run | Not run |
| 1.5.50 | 10/10 | 47/47 | Not run | Not run |
| 1.5.53 | 10/10 | 47/47 | Not run | Not run |
| 1.5.62 | 10/10 | 47/47 | Not run | Not run |
| 1.5.73 | 10/10 | 47/47 | Not run | Not run |
| 1.5.80 | 10/10 | 47/47 | Not run | Not run |
| 1.5.97 | 10/10 | 47/47 | Main menu passed | Not run |
| 1.6.317 | 10/10 | 47/47 | Not run | Not run |
| 1.6.318 | 10/10 | 47/47 | Not run | Not run |
| 1.6.323 | 10/10 | 47/47 | Not run | Not run |
| 1.6.342 | 10/10 | 47/47 | Not run | Not run |
| 1.6.353 | 10/10 | 47/47 | Not run | Not run |
| 1.6.629 | 10/10 | 47/47 | Not run | Not run |
| 1.6.640 | 10/10 | 47/47 | Not run | Not run |
| 1.6.659 GOG | Missing executable | Not checked | Not run | Not run |
| 1.6.1130 | 10/10 | 47/47 | Not run | Not run |
| 1.6.1170 | 10/10 | 47/47 | Not run | Prior-build evidence only |
| 1.6.1179 GOG | Missing executable | Not checked | Not run | Not run |
| 1.7.99 | 10/10 | 47/47 | Not run | Not run |
| 1.7.104 | 10/10 | 47/47 | Main menu passed | Not run |

All versions have build component 0. The address audit finds every one of DDC's
59 explicit relocation IDs in released databases for all 23 targets, including
GOG. Address presence does not establish executable or gameplay compatibility.
VR, Classic/LE, Microsoft Store/Game Pass and Epic are outside this DLL's
supported platform scope; see [the compatibility audit](RUNTIME-COMPATIBILITY-AUDIT.md).

## What the expanded testing found

The initial 1.7.104 callback fix still rejected existing camera and furniture
callbacks on **1.7.99**. The real 1.7.99 image passed clean preflight but failed
both callback tests. After independently verifying its call sites, the
production rule now accepts those callbacks on both reviewed 1.7 releases.
Wrong engine targets and conflicting UI drivers remain rejected. No claim is
made that this is the reporting user's exact failure.

Steam 1.6.317 through 1.6.640 use main-thread hook offset **+0xADA**, compared
with **+0xADF** on 1.6.1130/1170 and **+0xAF1** on 1.7.99/104. The production
scanner already handles this difference; the test fixtures now cover it.

## What each scenario proves

1. Clean production preflight recognizes all five instruction patch sites.
2. An existing camera callback outside the game image is accepted.
3. An existing furniture callback outside the game image is accepted.
4. A camera call redirected to the wrong engine function is rejected.
5. An already-disabled UI job is diagnosed and rejected.
6. A replaced main-thread UI-driver call is rejected.
7. A replaced dialogue decrement is rejected.
8. UI-job installation changes only the intended branch opcode, preserving its destination.
9. A changed call opcode after preflight is rejected.
10. A changed UI branch after preflight is rejected without overwriting it.

Tests use private mapped copies and run DDC's production validation/patch
functions. They never execute game instructions or synthetic callbacks. The
47 vtable checks cover camera states, POV/orbit input, all 15 CanProcess hooks,
dialogue/menu hooks and seven damage observers. Executable target validation
does not prove a C++ signature, object layout or in-game behavior.

All 21 original executable sizes and complete SHA-1 digests match Steam's depot
manifests. An independent Capstone disassembly verifies the five test mutation
sites in each executable. Unpacking changes only private inspection fixtures;
1.6.1130 is inspected directly because its original image has no SteamStub
`.bind` section. Exact image/database/checker hashes accompany the results.

## Startup evidence and limits

The earlier logging DLL passed isolated startup on 1.5.97/SKSE 2.0.20 and
1.7.104/SKSE 2.3.1, both with SKSE Menu Framework 3.14.1. Logs show instruction
validation, hook installation, data loading, menu registration and main-menu
opening. Each process remains alive ten seconds after the checkpoint.

These tests end by controlled process termination. Normal exit, save loading,
camera movement, combat, dialogue, mounts, unpaused menus and coexistence with
real mod combinations still need gameplay acceptance. Previous binaries and
the shared Creations catalog were restored after both tests. An initial
harness cleanup race was repaired and that first run is excluded from passes.
The author's normal installation was not deployed to or used for these tests.

## Repeat the matrix

Build with `DDC_BUILD_CHECKS=ON` and `DDC_DEPLOY_TO_MO2=OFF`. The matrix runner
requires Python and `pefile`. Supply private executable fixtures named
`SkyrimSE-1.6.353.exe.unpacked.exe` (or `.exe` for an already-unwrapped image)
and the exact released Address Library files. The runner reads the actual PE
ProductVersion and database header before testing; filenames alone are not
accepted as proof of version.

```powershell
python tools/test-runtime-matrix.py --images C:/private-tests/images --libraries C:/private-tests/libraries --report build/runtime-matrix-new
```

`--images` and `--libraries` can be repeated. Each report directory must be new
so earlier evidence cannot be overwritten. The JSON/Markdown matrix lists
every version from `include/Hooks/RuntimeVersion.h`, including missing inputs.
Exit 0 means all requested checks passed with no missing versions; exit 1
means a test/input failed; exit 2 means executable/database inputs are missing.
`--allow-missing` relaxes only the exit code, never the recorded coverage.
Use `--scenarios clean` for initial inspection of a newly acquired GOG image;
mutation scenarios require an independent site review before enabling it.

## Evidence

Private evidence root: `build/diagnostics/runtime-matrix-20260919/`.

- `final/matrix.json`, `final/matrix.md` and 210 individual scenario logs.
- `fixture-manifests.json`: all 21 original Steam content hashes and receipts.
- `reviewed-sites.json`: independent site disassembly and fixture hashes.
- `before-1.7.99-fix/`: clean pass and both callback failures before correction.
- `addresses.json`, `build-final.log`, `ctest-final.log`.
- `startup-1.5.97-20260919-100057/` and `startup-1.7.104-20260919-100201/`:
  startup results, game/SKSE logs, catalog restoration and binary backups.

Earlier audit candidate: `build/release-candidate/Release/DietDrCamera.dll`.
SHA-256: `531678CCFDC744C6995EAB8504F703A4D46DA1DC9BC8D76C0854DFA2E759F830`.
No release was published.
