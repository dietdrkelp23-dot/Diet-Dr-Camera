# Runtime compatibility - 1.1.1

Version 1.1.1 changes only whole-tab clipboard matching and release metadata.
Runtime hooks and menu loading are unchanged from 1.1.0. The live evidence below
identifies the earlier tested builds; no additional runtime launch is claimed
for the clipboard patch.

The author approved 1.1.0 for release on September 15, 2026. The original
reported startup failure has not yet been reproduced with its exact mod setup.
Clean 1.5.97, 1.6.1170 and 1.7.104 executables pass production preflight using
both the released 1.0 hook sources and the candidate, with each version's
released Address Library database. These are offline instruction checks,
not game launches; none reproduces the reported failure.
See [RUNTIME-TESTING.md](RUNTIME-TESTING.md) for the current test matrix.
Live 1.5.97 testing has also reproduced a separate menu startup crash after
successful hook installation. The candidate corrects the menu SDK's early
DLL-handle caching and waits for its ImGui context before applying UI scale.
The corrected candidate has reached the main menu and player-camera initialization
on 1.5.97, and has also loaded its hooks and menu through the 1.7.104 main menu.
The startup-tested candidate has also loaded through the main menu in the
author's normal 1.6.1170 mod list. The author accepted Pitch Bias and approved
the final menu refinements after testing in that list.
Other historical/GOG builds remain unlaunched.
The evidence below describes the previously accepted 1.0 release unless noted.

Diet Dr Camera uses one non-VR CommonLibSSE-NG DLL for SE and AE. Version 1.0
pins alandtse/CommonLibSSE-NG v7.5.4 at
`c5424463bba9af0d75cde8640ba7ddd4cacb9e39` and uses Address Library formats 1, 2 and 5.
Install SKSE, Address Library and SKSE Menu Framework versions compatible with
your executable. The Anniversary content purchase does not determine its version.

## Targets

The loader accepts these known executable versions (all with build component 0):

- SE: 1.5.3, 1.5.16, 1.5.23, 1.5.39, 1.5.50, 1.5.53, 1.5.62, 1.5.73, 1.5.80, 1.5.97.
- AE: 1.6.317, 1.6.318, 1.6.323, 1.6.342, 1.6.353, 1.6.629, 1.6.640,
  1.6.659 (GOG), 1.6.1130, 1.6.1170, 1.6.1179 (GOG), 1.7.99, 1.7.104.

VR is excluded from the build. SKSE does not support Microsoft Store/Game Pass
or Epic executables. Unknown future releases are refused until their layouts are
reviewed. Compatible SKSE/Menu Framework builds are still necessary on historical versions.

## Engine hooks

Custom calls are found at decoded instruction boundaries inside the relevant
Address Library functions, including their chained unwind regions. Camera update,
furniture activation, the main-thread UI drive and dialogue timer are checked before
any DDC hooks are installed. The dialogue decrement is distinguished from the same
function's timer-reset calls. Its multiplier must be -1. The UI job must have the
expected pause guard and UI/console call sequence before that guard is replaced.
Older SE/AE compilers can use different negation instructions; their established
dialogue decrement offsets have a fallback that still verifies the instruction
boundary and exact native call target before accepting the site.

Established SE/AE camera call sites can chain another plugin's executable callback.
Unrecognized or ambiguous code causes an explicit startup failure. Simultaneous
unpaused-menu overhauls that replace the same UI job are not supported by this guard.

The main-thread hook now invokes the actual displaced ScrapHeap::KeepPages function
(IDs 66889/68150), rather than the unrelated AE function 82084. Its manual unpaused
UI drive remains in place. The unused first-person pass-through patch is removed.

NG accessors select pre/post-1.6.629 player data and the 1.7 player, graphics and
attack-handler layouts. The POV button and held-state slots shift by two on 1.7.

DDC reserves one 256-byte trampoline before installing hooks. All camera/menu
call stubs share it; per-hook allocation requests cannot resize NG's one-time
reservation. Older SKSE without a branch-pool interface uses a local allocation.
The startup checks exercise both shared-pool and local fallback paths, retain
previous hook stubs, and verify the chained camera/noise call.

## Evidence and remaining tests

- Release build and 30 CTests pass, including spell/staff ritual classification
  and the unchanged frozen preset fixtures.
- Profile-lifetime, authored-reset, sparse-preset and package provenance checks
  cover the final release sources. Runtime hook selection and the known-runtime
  list are unchanged by the release cleanup.
- All 52 directly referenced relocation/vtable IDs are present in the installed
  SE/AE Address Library databases and the canonical 1.7.99/1.7.104 offset CSVs.
  This establishes address availability, not equivalent engine behavior.
- The production decoder was tested against this machine's 1.6.1170 executable:
  camera +0x1A6, furniture +0x2A2, main UI drive +0xADF, dialogue decrement +0x6E8,
  and the UI job's +0xB guard all match.
- Automated layout checks exercise SE, early AE, post-629 AE, GOG and 1.7 accessors.
- The author confirmed Mysticism projectile tracing working on September 13 on
  1.6.1170. The 11:50-11:54 game log records the corrected NG build loading,
  validated hooks, both-hand Fireball previews and released traces, and menu
  framing opening/closing. It contains no error or critical entries. Earlier
  accepted mounted tracking and collision behavior was tested on
  1.6.1170 with the previous CommonLib version. The author subsequently accepted
  staff Ritual selection and the Vanity close-up retest on the NG build; the
  September 13 13:16-13:17 log shows Vanity FOV returning to the gameplay target
  and return ownership ending. Other final smoke-test items
  remain documented in RELEASE-CHECKLIST.md.
  Test loading, POV switching, bow/magic projectiles, mounted target lock, dialogue,
  unpaused menus and collision before treating another runtime as gameplay-verified.

Version 1.0 writes preset format 7 and reads development formats 1-6. Frozen
default fixtures and migration checks cover the older formats. Staff selection,
Vanity, first-person Hit Shake and the reported chained Whirlwind Sprint/Flee
Framing retest were accepted on 1.6.1170; the full profile/override matrix remains
in the release checklist. Build, installation and compatibility checks do not
write personal presets.

Archery Hit Shake chains ArrowProjectile's SE/AE
UpdateImpl (0xAB) and AddImpact (0xBD) vtable slots, using NG's versioned
Projectile runtime-data accessor. The update observer chains the existing aim
handler; the contact observer forwards every argument unchanged and copies
values for the camera thread. It introduces no new relocation IDs or Precision
dependency. Callback delivery, projectile-mod coexistence and gameplay behavior
remain pending in-game checks on each supported executable.

Projectile spell/staff Hit Shake likewise chains MissileProjectile UpdateImpl
and AddImpact at those slots, after DDC's tracing observer. It requires a
discrete Fire & Forget missile and a player hand source, retains copied tuning
for both hands, and uses the projectile's native dual flag for Both Hands.
Native spell-cast signals distinguish rapid casts; animations omitting them
use a 50ms launch grouping window. Contact deduplication spans the whole cast,
so explosion damage events cannot add impulses. No new address IDs, runtime
dependency or preset format are introduced. Native delivery and modded cast
timing remain in-game validation items; the automated checks test the bridge,
profile routing and persistence rather than executing the game engine.

## References

- [CommonLibSSE-NG runtime targeting](https://github.com/CharmedBaryon/CommonLibSSE-NG/wiki/Runtime-Targeting)
- [Maintained NG source and license](https://github.com/alandtse/CommonLibSSE-NG/tree/v7.5.4)
- [Canonical address mappings](https://github.com/alandtse/skyrim_vr_address_library)
- [SKSE runtimes and platform support](https://skse.silverlock.org/)

DDC is distributed under GPL-3.0-or-later with modding/linking permissions. The
matching source ZIP is supplied alongside the plugin; see LICENSING.md. Third-party
components retain their own terms and notices. The author approved this release
license on September 12, 2026. Gameplay coverage remains as documented above.
