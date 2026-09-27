# Runtime and hook audit — September 24, 2026

> Historical audit. The fixes below are retained in 1.2.1; the native movement
> experiments were subsequently removed. See RELEASE-CHECKLIST.md and
> RUNTIME-MATRIX.md for current validation.

The audit found and corrected a projectile callback ABI defect, incomplete call-site
revalidation, and collision-entry patch ownership/range weaknesses. These changes
are in the current development workspace. They have not been published or tested
in a live game session. The earlier CBPC startup fix is included; see
[CBPC-STARTUP-FIX.md](CBPC-STARTUP-FIX.md).

| Verification | Result |
| --- | --- |
| Release configuration, native movement disabled | Build and 47/47 CTests pass |
| Development configuration, native movement enabled | Build and 47/47 CTests pass |
| Offline executable scenarios | 735/735 pass: 35 scenarios on each of 21 Steam versions |
| Final method-identity checks | 67 shipping and 12 development vtable targets checked on each of those 21 images |
| Explicit source relocation inventory | 79 selected IDs present for every one of 23 listed runtimes, including both GOG versions |
| CBPC entry-pointer variation | 4,096 deterministic pointer values checked, in addition to the exact reported bytes |
| GOG executable testing | Missing 1.6.659 and 1.6.1179 images |
| Live startup/gameplay of these candidates | Not run |

The 21 Steam targets span SE 1.5.3 through 1.5.97 and AE 1.6.317 through
1.7.104; the exact list is in `include/Hooks/RuntimeVersion.h`. Passing an offline
matrix is evidence for the inspected hooks, not certification of every game
feature or mod combination. Earlier startup evidence belongs to earlier binaries.

## Projectile impact callbacks discarded an engine return value

`ArcheryHitShakeController::AddImpact`, `MagicHitShakeController::AddImpact`, and
`ConeImpact` were declared `void`, following the incomplete CommonLib declaration
for virtual slot `0xBD`. Native Arrow, Missile, and Cone implementations return an
`ImpactData*`, or null. The already-correct Beam callback used that return type.

Native callers consume the pointer. For example, in the 1.6.1170 executable,
the virtual call at RVA `0x7C9DDA` is followed by saving/testing RAX and subsequently
reading the returned object's fields. The arrow and missile hooks called their
observers after the original function without preserving its result. That can
replace the pointer with an unrelated return-register value and can cause a combat
crash or incorrect impact handling. This is a confirmed ABI mismatch, not a
reproduced in-game crash report.

All four callbacks now use the shared `ProjectileImpact::Function` type. Arrow
and missile retain the result across their observer calls; cone returns it
directly. Typed registration rejects a callback with a different function type.
The result is not dereferenced after the original call, which may destroy the
projectile. Observer eligibility and projectile trajectory behavior are unchanged.

Evidence:

- `native-contracts.json` records native method return sequences and virtual-call
  consumers across all 21 images. It also records the animated-camera RTTI and
  ammo-getter checks described below.
- `abi-before/symbols.txt` records the three old compiled `void` callbacks.
- `abi-before/check.log` shows the new ABI check rejecting the old objects.
- `abi-after.log` and both CTest logs show the corrected four production callbacks
  passing. `ProjectileImpactABIChecks.ps1` inspects compiled production symbols,
  rather than a separate implementation of the callbacks. It checks the return
  type; it does not execute combat or prove all observer behavior.

## A changed call destination could pass revalidation

`RequireCall` previously checked only for opcode `E8`. Redirecting a validated
call to a different function while retaining that opcode was accepted. The new
`call-target-changed-after-preflight` image scenario reproduced the acceptance
before the correction; see `repro-call-target.log`.

Preflight now remembers all five call bytes. `InstallCall` rechecks those bytes
and verifies that the callback is executable immediately before installation.
It records DDC's replacement so DDC's own camera post-update/noise chain can
still install twice at the same call site. All active five-byte call writers,
including the opt-in movement code, use this path.

The image scenarios cover a changed opcode, a changed destination, an unvalidated
site, a non-executable callback, and the two-step camera chain. Existing supported
third-party camera/furniture callbacks remain accepted during preflight; invalid
engine destinations and competing UI drivers remain rejected.

## Collision patch ownership and jump range

The camera collision toggle wrote its saved six bytes, false-return stub, or
diversion without verifying the current entry. It could overwrite a patch
installed by another DLL after DDC saved the original entry. The temporary
restore/rearm around a filtered cast had the same issue.

Each transition now verifies the complete six-byte state DDC expects before
writing. A mismatch stops DDC's collision overrides for that session, records the
bytes and logs the conflict. The `caster-owned-entry` and `caster-entry-conflict`
scenarios exercise normal transitions, restore/rearm, and foreign byte changes
while checking that surrounding code remains intact.

The diversion also used an unchecked narrowing conversion to a signed 32-bit
jump displacement. It now rejects an unreachable stub, caches the validated
encoding, and flushes executable writes. Boundary tests cover both signed-rel32
limits, either side outside the range, and instruction-address overflow. No real
out-of-range allocation was observed; this correction closes a latent failure
path. If allocation/range validation fails, the existing full-pass-through
fallback remains in effect.

The collision implementation still temporarily restores executable entry bytes
to call the original function. These ownership checks do not make patching atomic
against arbitrary concurrent execution or concurrent foreign patch writers.
Replacing that mechanism with a permanent, correctly relocated original-call
trampoline would require a separate implementation and live collision testing.

## Coverage gaps and issues ruled out

The original image checks omitted 16 installed projectile, animation, and Havok
vtable targets, plus the three animated-camera event tables found by RTTI. The
shipping inventory is now 67 targets. Checks compare native method identities
against Address Library IDs, as well as requiring executable image addresses.
An adjacent slot pointing to a different valid function therefore fails.

An independent PE/RTTI walk found the same three animated-camera handler tables
as Address Library on all 21 images, with a unique matching object locator for
each handler. The development inventory also includes the animation-graph and
sprint-button hooks. Its final count is 12; eight development call sites and the
first-person spring guard are inspected separately.

The recent upstream CommonLib `GetCurrentAmmo` ordinal correction was examined.
It changes the cross-VR wrapper. DDC's flat SE/AE build uses the correct compiler
virtual slot `0x9E`; a production-dispatch fixture confirms this, and that slot
matches native IDs 37658/38612 on all 21 executable images. The newer dependency's
existence alone was not a reason to replace the pinned library during this fix.
See the [upstream correction](https://github.com/alandtse/CommonLibSSE-NG/commit/33e6a966001d8c86e52be6e2b38262e6c346353e).

The address-audit script incorrectly treated the legacy first-person spring
constant and its 1.7 replacement as unconditional dependencies. It reported a
missing legacy ID on 1.7 and flagged the guarded replacement as an unsupported
raw ID. The audit now models the explicit version conditional: 509804/382518
before 1.7.99, 563959 afterward. Unknown active raw-ID references still fail the
audit. The unused `MenuViewCache` raw-ID path remains classified as dormant.
This was a tooling false positive, not a new missing-address runtime defect.

The review also checked active raw memory/patch sites, runtime-selected actor and
projectile accessors, input-slot shifts, callback chaining, trampoline allocation,
and menu-framework load/context guards. The 47-check suite includes those existing
regressions and settings/preset checks. It is not exhaustive coverage of every
implicit CommonLib relocation or every gameplay path.

## GOG and remaining validation

Both GOG databases pass the explicit-ID audit, and the accessor tests cover GOG
1.6.659/1.6.1179 layout selection. Actual GOG executable fixtures are unavailable.
The matrix keeps these rows marked missing instead of substituting a nearby Steam
executable. Startup with the matching GOG SKSE and menu-framework build, followed
by gameplay testing, remains necessary. The official
[SKSE downloads](https://skse.silverlock.org/) distinguish Steam and GOG builds.

Historical Steam versions likewise require matching SKSE and compatible dependency
builds. The loader list describes known SKSE-capable SE/AE targets; it does not
include VR, Classic/LE, Microsoft Store/Game Pass, Epic, or unknown future updates.

Live acceptance should exercise arrow/crossbow, missile, cone and beam impacts;
collision enable/disable and per-layer filters; save loading; dialogue and
unpaused menus; and the reporting user's CBPC/TDM combination. A loaded-module
list alone does not establish other conflicts.

## Reproduction and artifacts

All paths below are under
`build/diagnostics/runtime-deep-audit-20260924-195453/`:

| Artifact | Contents |
| --- | --- |
| `source-before/`, `changes.patch`, `source-after-sha256.json` | Turn-specific source evidence, preserving earlier workspace edits |
| `baseline-tests.log` | 46 checks passed before this audit's new regression check |
| `release-tests.log`, `native-tests.log` | 47/47 checks in each build configuration |
| `final-runtime-matrix/matrix.json` | 735 scenarios; 21 passed runtimes, zero failed, two missing GOG images |
| `final-vtable-matrix/matrix.json` | Follow-up clean checks with the final 67 shipping / 12 development identity inventory |
| `address-audit.json` | 79 explicit selected IDs for all 23 known runtime versions |
| `native-contracts.json`, `impact-virtual-callers.txt` | Independent native code and RTTI observations |
| `candidate/` | Release-configuration DLL/PDB, hashes and binary verification |
| `development-candidate/` | Native-movement-enabled DLL/PDB and verification |

The final vtable follow-up adds the missing development sprint slot and pins the
animation-graph callback identity. The production sources are unchanged from the
735-scenario run; the two reports retain their separate checker/source hashes.

The release-configuration DLL SHA-256 is
`F665188D951A1CAB83918A62232A3F9531D80B82C0C1AC9C3E94EA758E2D39E3`.
Both candidates retain version 1.2.0 and include pre-existing development changes.
They are review/test artifacts, not isolated replacements for the published
1.2.0 package. Deployment was disabled in both builds.

Repeat the executable checks with:

```powershell
python tools/test-runtime-matrix.py --images build/diagnostics/runtime-images --libraries build/diagnostics/runtime-libraries --libraries "C:/SkyrimMo2/mods/Address Library for SKSE Plugins/SKSE/Plugins" --report build/diagnostics/new-runtime-matrix
```

The runner exposes 35 scenarios; the PowerShell single-image runner accepts the
same set. A missing executable remains incomplete coverage (runner exit 2),
even when every available fixture passes.
