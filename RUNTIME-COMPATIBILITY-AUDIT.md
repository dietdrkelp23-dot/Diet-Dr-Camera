# Skyrim runtime and NG compatibility audit

September 19, 2026. This describes the current development checkout, not a new
published release. The quoted older-version startup failure is **not yet
diagnosed**: the report supplies neither the executable version nor the full
failure message. An NG build and a list of accepted versions are insufficient
evidence for a claim that every version works.

## What the reported line establishes

`Hooks/RuntimeHooks.cpp(24)` in the released source is DDC's shared fatal-error
handler. Multiple failures reach it: function-boundary inspection, instruction
decoding, missing/ambiguous calls, the dialogue decrement, and the UI job guard.
The source line alone identifies none of those causes. A runtime-specific code
difference and a previously installed DLL patch can both reach this handler.

The complete message, `DietDrCamera.log`, `skse64.log`, SkyrimSE.exe's
ProductVersion and the installed DDC version would identify this user's failed
check directly. They are not prerequisites for independent investigation:
testing the older executables can reproduce a runtime-specific defect without
the user's log. If only a particular mod combination fails, its DLL/mod list
becomes useful. Do not prescribe a game update or announce a fix from the line
number alone.

## What NG provides, and what DDC must provide

CommonLibSSE-NG can produce one DLL for several runtime families. It supplies
runtime-selected addresses and accessors for differing class layouts. Plugin
authors still have to use those facilities correctly and handle their own
instruction offsets, function signatures and virtual slots. Enabling SE and AE
in CMake does not inspect a custom hook for each executable.
[NG runtime-targeting documentation](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Runtime-Targeting).

There are separate compatibility requirements:

| Layer | Requirement | DDC audit finding |
| --- | --- | --- |
| Executable/platform | Native x64 SKSE-capable Skyrim runtime | Build enables SE and AE; VR is disabled. |
| SKSE loader | Matching SKSE; both legacy Query and AE Version exports | Both are in the DLL. The release verifier previously checked only Load/Version; it now requires Query too. |
| Address Library | Exact executable's database and a supported encoding | Pinned NG reads formats 1, 2 and 5. Explicit DDC IDs were checked as described below. |
| Object ABI | Correct members, base adjustments and virtual slots | Tests cover eight representative layouts, including both sides of 1.6.629 and 1.7.99. These are accessor tests, not engine execution. |
| Instruction patches | Correct instruction boundaries, callees and control flow | Ten offline scenarios pass against each of all 21 loader-listed Steam executables. GOG images are missing. |
| Other DLL hooks | Preserve compatible callbacks; detect incompatible UI drivers | Reproduced 1.7.99/1.7.104 chaining rejections are fixed. UI-driver conflicts are diagnosed and still refused. |
| Menu dependency | A compatible framework DLL, exports and initialized ImGui context | Existing delayed module lookup/context checks remain; the menu dependency has its own runtime requirements. |
| Startup/gameplay | Actual SKSE/dependency loading and behavior in the game | The final candidate reaches the main menu on isolated 1.5.97 and 1.7.104. No new gameplay acceptance or normal-exit test is claimed. |

SE-era SKSE uses `SKSEPlugin_Query`; AE uses `SKSEPlugin_Version`. DDC supplies
both and performs common initialization in Load. The `UsesNoStructs` metadata
name is easy to misread: SKSE also permits that flag for plugins that deliberately
handle both pre/post-1.6.629 layouts. It is a loader declaration, not an automatic
layout conversion. The separate Address Library v5 flag concerns the encoding
used from 1.7.99. DDC's emitted flags are checked in the actual Release DLL.
[SKSE's plugin API](https://github.com/ianpatt/skse64/blob/master/skse64/PluginAPI.h),
[SKSE's loader checks](https://github.com/ianpatt/skse64/blob/master/skse64/PluginManager.cpp).

## Runtime families and actual evidence

The executable version determines the native ABI; purchasing Anniversary
content does not turn an older executable into a different native build. An
installation with downgraded executable files must use the matching SKSE and
address database. Compatibility of retained game data/ESLs is a separate test
from this DLL's hooks.

| Runtime | Important distinction | Evidence available here |
| --- | --- | --- |
| SE 1.5.3 through 1.5.80, as individually listed in RuntimeVersion.h | Historical SE targets; dependency/runtime behavior still needs actual launches | Ten image scenarios and all 47 hooked vtable targets pass on every listed executable |
| SE 1.5.97 | Legacy SKSE Query loader; Address Library format 1 | Ten image scenarios, 47 vtable targets and final-candidate main-menu startup pass |
| AE 1.6.317, .318, .323, .342, .353 | AE IDs/loader, but pre-1.6.629 player layout | Ten image scenarios and 47 vtable targets pass on every listed executable |
| AE 1.6.629 and .640 | Post-629 member shifts within the AE family | Ten image scenarios and 47 vtable targets pass on both executables |
| GOG 1.6.659 | Distinct executable/database; post-629 layout selection | Explicit IDs and accessor test; executable/gameplay untested |
| AE 1.6.1130 | Its own executable/database; the Steam image has no SteamStub .bind section | Ten image scenarios and 47 vtable targets pass using the original image |
| Steam 1.6.1170 | Post-629 AE | Ten image scenarios and 47 vtable targets pass; established author gameplay evidence exists for earlier development builds |
| GOG 1.6.1179 | Separate GOG executable/database | Explicit IDs and accessor test; executable/gameplay untested |
| Steam 1.7.99 | New player/input/graphics layouts and Address Library v5 | Released v5 database, ten image scenarios and 47 vtable targets pass; callback regression reproduced and fixed |
| Steam 1.7.104 | Same new layout family, but internal offsets still require inspection | Released v5 database, ten image scenarios, 47 vtable targets and final-candidate main-menu startup pass |
| VR 1.4.15 | Different ABI and camera/input behavior | Excluded; a VR port needs separate implementation and testing |
| Classic/Legendary Edition | Separate 32-bit game/SKSE | This x64 DLL is not a Classic/LE plugin |
| Microsoft Store/Game Pass and Epic | SKSE platform limitation | Not supported by SKSE |

As checked on September 19, the official SKSE page lists 2.0.20 for 1.5.97,
2.3.1 for Steam 1.7.104, 2.2.6 for GOG 1.6.1179, and 2.0.12 for VR 1.4.15.
Historical executable owners need the corresponding archived SKSE build.
The same page explicitly excludes Microsoft Store/Game Pass and Epic.
[Official SKSE downloads and compatibility](https://skse.silverlock.org/).

Address Library v13's main archive is described as covering game versions
through 1.7.104. Its presence does not mean every plugin supports those versions;
the plugin must load the database for the running executable and implement its
ABI. Never rename another runtime's `.bin` to satisfy a missing-file error.
[Address Library's official files](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files).

For example, NG selects PlayerCharacter runtime data at 0x3D8 before 1.6.629,
0x3E0 afterwards, and 0x3E8 from 1.7.99. PlayerInputHandler gains two virtual
slots in 1.7; DDC's button/held-state hooks use its version-aware InputSlot
helper. An `IsAE()` branch alone cannot distinguish all these cases.
[Pinned PlayerCharacter accessors](https://github.com/alandtse/CommonLibSSE-NG/blob/v7.5.4/include/RE/P/PlayerCharacter.h),
[pinned PlayerInputHandler slots](https://github.com/alandtse/CommonLibSSE-NG/blob/v7.5.4/include/RE/P/PlayerInputHandler.h).

DDC pins alandtse/CommonLibSSE-NG v7.5.4. I reviewed the maintained fork's newer
release notes through v8.3.0, including the v8.0.1 PluginDeclaration v5-flag fix.
DDC uses PluginVersionData and its built flags already contain v5 support.
Replacing the dependency is not evidence of a fix for DDC's own hook rejection.
[Maintained NG releases](https://github.com/alandtse/CommonLibSSE-NG/releases).

SKSE Menu Framework's changelog records its own 1.7 update. Therefore DDC's
runtime coverage cannot substitute for a compatible framework installation.
The previously fixed SE startup issue involving early framework-handle caching
is distinct from the RuntimeHooks failure handler.
[Framework changelog](https://www.nexusmods.com/skyrimspecialedition/mods/120352?tab=description).

## Reproduced findings and changes

1. **1.7.99 and 1.7.104 rejected already-hooked camera/furniture calls.** The old code
   considered established callbacks only before 1.7.99. Its generic fallback
   then required the original native callee, so a valid existing callback failed
   preflight. The verified 1.7.99/1.7.104 camera +0x1A6 and furniture +0x2A2 sites now
   receive the same boundary/executable-target checks. This exception is limited
   to those two checked 1.7 executables; it is not extended to unexamined builds.
   Wrong engine targets are still rejected. Callback execution/coexistence in
   gameplay was not tested here.
2. **Competing UI drivers can produce the same reported source location.**
   Skyrim Souls' source disables the UI job and replaces the main-thread and
   dialogue calls that DDC also uses. DDC now recognizes an already-disabled
   UI job and identifies the conflict before the other UI checks. It does not
   install a second driver or bypass validation. The source establishes the
   collision; it does not establish that the reporting user has Skyrim Souls.
   [Upstream UI patches](https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/MenuProcessing.cpp),
   [upstream dialogue patch](https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/DialogueMenuEx.cpp).
3. **Failure evidence was too limited.** Failures now include inspection bytes,
   expected/observed direct-call targets when call selection fails, runtime and
   stage. Targets inside loaded images include the module name; anonymous
   trampoline allocations remain identified as allocations. Logs are flushed.
   The message requests complete DDC/SKSE logs without treating every failure as
   proof that the game is too old.
4. **The release verifier did not require the SE entry point.** It now requires
   Query, Load and Version. A regression removes each export from a private copy
   of the actual compiled DLL and requires rejection.

No observed defect here establishes why the quoted older-version setup fails.
The changed callback rule concerns 1.7.99/1.7.104; all older Steam images pass clean preflight.
That distinction must remain in any user-facing release note.

## Reproducible checks and their limits

`tools/audit-runtime-addresses.py` checks 59 distinct explicit SE/AE ID pairs,
including input-handler macros and damage-effect template vtables. Every ID was
present for all 23 loader-listed runtimes in released database inputs. The
expanded test uses the released v13 1.7.99 database, replacing the initial audit's
CSV-only coverage. This does not cover every relocation used internally by CommonLib or
prove the meaning of an address.

The audit also reports the single-runtime `REL::ID(82325)+0x1B8` reference in the
dormant MenuViewCache experiment. There is no installation call in current
production sources. It must not be enabled without porting and validation.

```powershell
python tools/audit-runtime-addresses.py --libraries C:/private-tests/libraries --csv-directory C:/private-tests/canonical-offsets --report build/runtime-addresses.json
powershell -NoProfile -File tools/test-runtime-image.ps1 -RuntimeImage C:/private-tests/SkyrimSE-1.5.97.exe.unpacked.exe -AddressLibrary C:/private-tests/version-1-5-97-0.bin -Scenario ui-driver-conflict
python tools/test-runtime-matrix.py --images C:/private-tests/images --libraries C:/private-tests/libraries --report build/runtime-matrix
```

For every one of the 21 Steam targets, ten scenarios pass: clean, existing
camera callback, existing furniture callback, wrong engine callee, disabled UI
job, replaced main-thread call, replaced dialogue decrement, actual UI-branch
installation, changed call opcode after preflight, and changed UI branch after
preflight. Prepare leaves the inspected sites untouched. Installation preserves
the native branch destination and surrounding bytes. Later conflicting changes
are refused without overwriting them. No mapped game code or callback executes.

An independent Capstone review verifies five patch sites in every image.
Steam's manifest sizes and full SHA-1 hashes verify all 21 original executables.
The main-thread offset is +0x61A on the listed SE releases, +0xADA on Steam
1.6.317 through 1.6.640, +0xADF on 1.6.1130/1170 and +0xAF1 on 1.7.99/104.
The fixture scenarios now account for the previously untested +0xADA group.
All 47 vtable targets that DDC hooks point into executable image sections on
each tested build. This checks address validity, not function semantics or ABI.

The original hook selection passed the clean 1.7.104 image and failed the two
callback scenarios. The initial audit's fix still failed both callbacks on
1.7.99; that failure was reproduced before extending the reviewed-version rule.
The final candidate passes all three on both versions. The private baseline
changes only fatal reporting to a C++ exception so a negative test cannot open
a blocking game-style error dialog.

The Release build and 38 CTests pass, including the new binary-export regression.
Initial evidence remains under `build/diagnostics/runtime-audit-20260919/`.
Expanded evidence is under `build/diagnostics/runtime-matrix-20260919/`:
`final/matrix.json` and 210 scenario logs, `fixture-manifests.json`,
`reviewed-sites.json`, the failing 1.7.99 baseline, address audit and build logs.
The development DLL is
`build/release-candidate/Release/DietDrCamera.dll`, SHA-256
`531678CCFDC744C6995EAB8504F703A4D46DA1DC9BC8D76C0854DFA2E759F830`.
It was temporarily installed only in two private startup-test profiles; their
previous binaries were hash-verified after restoration. No normal-installation
deployment or publishing occurred.

The desktop-control helper remained unavailable, but Steam's command-line
`-console +download_depot` interface successfully retrieved the missing owned
Steam executables into its separate content cache. No game-installation files
were replaced. Steam download receipts and executable versions were verified
before preserving each fixture. Missing logs did not prevent this testing.

The final candidate also passes isolated main-menu startup on 1.5.97 with SKSE
2.0.20 and 1.7.104 with SKSE 2.3.1, both using Menu Framework 3.14.1. Logs show
hook installation, data loading, DDC menu registration and main-menu opening;
the process remains alive ten seconds later. Tests end by controlled process
termination, so normal exit and gameplay remain untested. The shared Creations
catalog is restored after each run. One initial harness cleanup race was
corrected and that run is explicitly excluded from acceptance.

GOG 1.6.659/1179 executable tests, startup on the other 19 Steam versions and
gameplay acceptance remain outstanding. Missing matrix fixtures produce exit 2
unless explicitly allowed; they are never recorded as passes. See
[the complete matrix and reproduction instructions](RUNTIME-MATRIX.md).
