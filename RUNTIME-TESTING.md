# Runtime testing and release evidence

Version 1.1.1 fixes Sheathed/Unsheathed whole-tab matching. Its regression checks
exercise both directions and environment separation. This patch changes no
runtime-hook or menu-loading code and has not received an additional in-game
tab-paste test. The live results and DLL hashes below describe 1.1.0 and its
earlier candidates.

The author approved version 1.1.0 for release on September 15, 2026 after
testing in the normal 1.6.1170 mod list. The original reporter's exact runtime
and mod combination remain unknown. A message ending in
`Hooks/RuntimeHooks.cpp(24)` identifies the shared failure handler.
Retain the complete message, `DietDrCamera.log`, Skyrim's ProductVersion,
SKSE version, Address Library package version, and the active plugin list.

## Current evidence (September 15, 2026)

| Executable | Released 1.0 hook sources | Candidate hook sources | Candidate gameplay |
| --- | --- | --- | --- |
| Steam 1.5.97.0 | Offline preflight passes | Offline preflight passes | Pending |
| Steam 1.6.1170.0 | Offline preflight passes | Offline preflight passes | Author-approved release; broader combinations remain untested |
| Steam 1.7.104.0 | Offline preflight passes | Offline preflight passes | Pending |
| Reporting user's exact version/mod combination | Not yet identified | Pending | Pending |
| GOG 1.6.659 / 1.6.1179 | Address/layout checks only | Executable unavailable here | Pending |

None of these three clean images reproduces the report with the released 1.0
hook sources. The baseline and candidate use identical images and the matching
released Address Library databases, including the actual format-5 database
from Address Library v13 for 1.7.104 (not a generated CSV fixture).

| Executable | Camera update | Furniture | Main UI drive | Dialogue decrement |
| --- | --- | --- | --- | --- |
| 1.5.97.0 | `+0x1A6` | `+0x295` | `+0x61A` | `+0x4F9` |
| 1.6.1170.0 | `+0x1A6` | `+0x2A2` | `+0xADF` | `+0x6E8` |
| 1.7.104.0 | `+0x1A6` | `+0x2A2` | `+0xAF1` | `+0x6E8` |

All three also pass UI job control-flow validation. The main-thread call moved
on 1.7.104; both the released and candidate scanners resolve its new position.
These checks exercise hook preparation, not hook installation inside a running
game or interactions with other plugins that modify those instructions.

Interactive testing on 1.5.97 with SKSE 2.0.20 and Menu Framework 3.14.1 found
a separate startup crash. The dependencies-only profile loaded game data and
remained open. Released 1.0 and the initial 1.1 candidate installed their hooks,
then crashed in the first UI-scale update: the menu SDK had retained a null
framework DLL handle from DDC's static initialization. Matching PDB crash logs
identify `EnsureMenuUiScale` through `MenuUI::TickUiScale` in the camera hook.
The fix resolves the module at API use and checks context readiness. A Windows
DLL fixture tests dependency loading after the consumer starts, including an
initially absent ImGui context. This does not establish the cause of the original
`RuntimeHooks.cpp(24)` report or complete gameplay acceptance.

The startup-fix candidate (`58868D76520DFDC6C2592C3123C0B3AAD57C53A6F4A6D7DF930627AA1BCE1CE1`)
passes all 33 automated checks. Its isolated 1.5.97 run reached DataLoaded,
registered the DDC menu, entered the main menu and subsequently initialized the
player camera and menu input. The previous startup crash did not recur during
this session, and the author quit normally (Steam process exit code 0).
The same DLL also reached DataLoaded and the main menu on 1.7.104 with SKSE
2.3.1, installed its hooks and registered its menu. These results establish
startup on those two clean runtime/dependency combinations.

The author's current test scope is startup checks on the isolated runtimes,
with new-feature acceptance in the normal 1.6.1170 mod list. A subsequent UI
refinement keeps typed slider values in place with cursor-style highlighting;
its visual acceptance is part of that main-list test. The older AE/GOG versions
listed in RUNTIME-SUPPORT.md have not all been launched here, and the exact
reporting user's runtime/mod combination is still unknown.

After that refinement, candidate
`E004ABAD2E60FA991014BA9ECF707066341488900432A5B97EC63EDBC5A99303`
passed startup on both isolated runtimes and in the normal 1.6.1170 mod list.
All reached DataLoaded and the main menu with DDC's hooks and menu installed;
the normal list also connected to TDM and chained its existing camera callback.
The isolated runs ended by controlled termination after reaching the menu.
The author tested features in 1.6.1170 and closed the game normally.

The author accepted Pitch Bias and requested removal of its explanatory UI text.
The subsequent slider revision fixes immediate first-click focus, uses yellow
selection, and fits the input to its digits plus the one-pixel caret. A private
interaction check runs the production widgets against ImGui 1.90.8 (the installed
Menu Framework version): selection and yellow rendering occur in the mouse-down
frame, typing replaces the selection, Enter/click-away commit, Escape cancels,
and switching between rows preserves both edits. Main and Quick Tune widgets
pass at 100%, 160% and 240% font scale. The author subsequently approved the
release after main-list testing and the final UI refinements. The final 1.1.0 DLL is
`5B3A303C9E3F7E900E2A06265411397131BFA18FCF276473AA4D89F5AC41D697`;
the September 15 23:08-23:10 main-list log records 1.6.1170 loading the plugin,
validating its hooks and entering gameplay. The isolated startup results above
identify the earlier DLL with the same runtime-hook and menu-loading fixes.
Approval does not establish gameplay coverage for every listed runtime.

The first return to the normal list exposed a test-isolation problem: 1.7.104
had rewritten the shared LocalAppData `ContentCatalog.txt` with UUID content
identifiers. The 1.6.1170 engine threw `invalid stoull argument` while parsing it.
Preserving that incompatible cache and removing it from the shared location
allowed the same candidate and mod list to start. Test launchers now snapshot
and restore this file, with separate runtime caches. Recovery checks cover a
pre-existing catalog, an originally absent catalog, and a failed launch.

## Obtain and preserve private fixtures

Use your own Steam/GOG entitlement. Keep game executables and data outside
public source control. Neither release archive includes Bethesda game files.
Do not replace the live installation to obtain a test executable.

Steam's console depot downloads go under
`steamapps/content/app_489830/depot_489833/`. On this machine,
`download_depot 489830 489833 2289561010626853674` returned Skyrim 1.5.97.0.
Its original executable SHA-256 is
`693E5A51EA2680119A68620BCF5080E81745549872B5D06BBD3F51131B67ABAB`.
Preserve that file under a versioned private filename before downloading another
manifest of the same depot, which uses the same content directory.
Verify each file's ProductVersion and hash after download.

On September 15, 2026, `download_depot 489830 489833` returned manifest
`4886117324142477814`, containing Skyrim 1.7.104.0. Its original executable
SHA-256 is `846EFCCF0C1374D71F892907F46549560F2FCB0A75CB87A3EED438BAA0F1402F`.
The matching `versionlib-1-7-104-0.bin` from the official
[Address Library v13 archive](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)
has SHA-256 `8AAB3DD251D135B849BD983F86A4A205C920FA3E81F8E30C0E63CCFEF9423842`.
All three executable copies and the extracted test database remain private.

[DepotDownloader](https://github.com/SteamRE/DepotDownloader#usage) is an
alternative with explicit app, depot, manifest, and output-directory options.
Authenticate directly with Steam; do not put account passwords in repository
files or command histories. Historical manifest availability is controlled by Steam.

An executable alone is sufficient for the instruction check below. It is not a
complete playable version. Gameplay tests need a separate full game directory
with matching data and engine dependencies, matching
[SKSE](https://skse.silverlock.org/), Address Library, and SKSE Menu Framework.
Use separate mod-manager instances/profiles with isolated INIs and test saves.
Never open a valuable existing save under the downgraded test setup.

## Offline production hook check

Build with `DDC_BUILD_CHECKS=ON` as described in [BUILDING.md](BUILDING.md).
Use a private executable copy whose code section is unpacked, plus the exact
version's Address Library database. The checker maps the PE sections and unwind
metadata and runs DDC's production `RuntimeHooks::Prepare()` implementation.
It does not run the executable's entry point or call any Skyrim code.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test-runtime-image.ps1 -RuntimeImage C:\private-tests\SkyrimSE-1.5.97.exe.unpacked.exe -AddressLibrary C:\private-tests\version-1-5-97-0.bin
```

The report records file hashes, runtime, checker exit code, and `gameplay: not-run`.
Its JSON and log live under the ignored `build/diagnostics/runtime-tests/` folder.
The SE fixture's actual runtime is read through CommonLib's ProductVersion string
reader; its fixed numeric Windows version fields misleadingly say 1.0.0.0.

For a before/after comparison, extract a previous release's `RuntimeHooks.cpp`
and `RuntimePatchInspection.cpp` into a private directory. Configure with
`-DDDC_HOOK_BASELINE_DIR=C:/private-tests/old-hooks` and build
`RuntimeBaselineChecks`. Pass that executable to the script's `-Checker` option.
The game image and Address Library must be identical for both checks.

## Gameplay acceptance before an update

Run the reporting user's exact executable and relevant mod combination first,
then the older/current baseline runtimes. Record the DDC DLL hash and all dependency
versions. Keep failures and passes rather than replacing a failed log with a pass.

- Reproduce the original failure with 1.0; confirm the candidate starts and the
  expected hooks install without ambiguous or skipped checks.
- Load a disposable test save. Exercise first/third person, target acquisition,
  target switching, mounting, dialogue, unpaused menus, furniture, and collision.
- Test Pitch Bias at negative, zero, and positive values while approaching and
  leaving an enemy. Verify unlock, target death, POV change, preset reset,
  indoor/location/enemy overrides, and a bias-only animation override.
- Type slider values in the full menu and Quick Tune. Check Enter, click-away,
  Escape, negative/decimal values, range limits, and displayed speed sliders.
  Confirm normal mouse dragging and controller adjustment still work.
- Copy/paste complete camera, target-lock, noise, first-person, weapon, and
  animation tabs. Check both environments, disabled tuned entries, per-hand,
  directional, enemy, and location settings. Reorder locations/load a test preset
  between copy and paste. Unrelated settings must remain unchanged.
- Save and reload format-8 presets; load released format-7 presets without
  changed tuning. Use copies so the released build's presets remain available.

Complete the broader [release checklist](RELEASE-CHECKLIST.md) as well. Passing
an offline test is evidence about those instructions, not a substitute for this
gameplay acceptance or a promise of compatibility with every other plugin.
