# Live runtime results — OmniCam 1.3.0

## September 27: author-confirmed runtime sweep

After the latest test preparation, the author reported **“they all work.”**
Ten new main-profile runs record candidate SHA-256
`485e0f0514f56f296ff4f3a0eff0f9799b8c8cdcc31d1c1b016bbeaa4bdc3567`, one for each prepared version:
1.5.97, 1.6.318, 1.6.323, 1.6.342, 1.6.353, 1.6.629, 1.6.640, 1.6.1130,
1.6.1170 and 1.7.104. Their original run grades and notes remain unchanged.
This records the author's version smoke-test acceptance; it does not infer that
all CBPC/TDM/comparison profiles or every checklist item were separately tested.

The author subsequently confirmed the separate rapid three-word Whirlwind
Sprint fix: **“its good now.”** That focused acceptance belongs to candidate
`1c237def2af60422fb5caa063fc98dc14d69fd0ebb1d57fcc15500caf4915b53`
in the main setup. It adds only the overlapping shout-noise handoff to the
runtime-tested gameplay source. Both reports are retained with their own hashes.
Evidence and the runtime author report are in
`build/diagnostics/whirlwind-jerk-2026-09-27T22-00-47-439Z/`.

## Earlier 1.2.1 evidence

Follow-up on 1.6.1170: the author confirmed the R3 tap movement is gone in run `1.6.1170-DDC_1.2.1-20260926-180557-536586` (DLL `1dba473dd28a172b33e48678892f30b4626bfd1c09251339c97333bbc4fb6ee0`). The same test exposed shorter first-to-third hold timing. The subsequent candidate (`77b637ba80975d07b6dc6da0fee70b10fa2f12361256102e8b1f8e4379763087`) adds the matching first-person input guard; in-game timing acceptance is pending. This is a fourteenth captured session, separate from the original batch reviewed below. Original run records remain unchanged.

Support decision after this batch: **SE 1.5.97 is the oldest supported version**. 1.5.73 is excluded from the active test plan; its original failed runs remain below. The R3 tap fix was subsequently confirmed in the follow-up above; its timing follow-up still needs testing. These saved runs refer to the preceding candidate hash and have not been rewritten.

Reviewed September 26, 2026. **All 11 prepared runtimes have now been attempted: ten have confirmed startup and in-game camera activity; 1.5.73 is blocked by SKSE Menu Framework.** No DDC error/critical entries or SKSE plugin-load failures were found in the ten startup-confirmed runs. Each captured manifest identifies candidate SHA-256 `a3c8f63a31887e88ac6e9d30d9aa0fa089d65bdce8a6b7095c49032fdc5b4a7a`, and each log identifies the requested Skyrim runtime.

There are 13 captured sessions: ten saved candidate runs, the earlier 1.6.1170 Anniversary-prompt session, and two failed 1.5.73 attempts. The saved verdicts remained `startup: unreviewed / gameplay: not-run`, with empty notes. The table below records observed log evidence; it does not replace those original records with an assumed full gameplay pass. User confirmation of the camera/dialogue behavior has been requested.

| Skyrim runtime | Startup | In-game evidence | Saved run |
| --- | --- | --- | --- |
| 1.5.73 | Failed — framework allocation | DDC loaded, but PostLoad/gameplay not reached | 1.5.73-DDC_1.2.1-20260926-170413-935864; 1.5.73-DDC_1.2.1-20260926-170426-219838 |
| 1.5.97 | Confirmed | Camera, POV input and dialogue; ungraded | 1.5.97-DDC_1.2.1-20260926-163515-514798 |
| 1.6.318 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.318-DDC_1.2.1-20260926-163719-409490 |
| 1.6.323 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.323-DDC_1.2.1-20260926-163851-749656 |
| 1.6.342 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.342-DDC_1.2.1-20260926-164019-453896 |
| 1.6.353 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.353-DDC_1.2.1-20260926-164543-768684 |
| 1.6.629 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.629-DDC_1.2.1-20260926-164749-486411 |
| 1.6.640 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.640-DDC_1.2.1-20260926-164916-946148 |
| 1.6.1130 | Confirmed | Camera, POV input and dialogue; ungraded (see warning below) | 1.6.1130-DDC_1.2.1-20260926-165027-117531 |
| 1.6.1170 | Confirmed | Camera, POV input and dialogue; ungraded | 1.6.1170-DDC_1.2.1-20260926-163238-018173 |
| 1.7.104 | Confirmed | Camera, POV input and dialogue; ungraded | 1.7.104-DDC_1.2.1-20260926-165125-424874 |

All ten latest run journals record successful restoration of the shared files. The 1.6.1170 retry and the other newer-runtime runs reached the world after the Anniversary prompt fix. This confirms they got past the prompt; it is not a visual assessment of every menu.

## 1.5.73 dependency failure

The author reported a trampoline error from SKSE Menu Framework. Both captured runs corroborate it: SKSE 2.0.15 reports `unknown QueryInterface 00000007`, and SMF logs `Failed to handle allocation request`. DDC successfully returns from SKSEPlugin_Load, but the framework stops startup before DDC receives PostLoad, reserves its own trampoline, or installs camera hooks. This is a failed startup of the tested combination, not a DDC gameplay pass.

The official archived SKSE SDK/changelog confirms that interface 7 is the trampoline interface, introduced in SKSE 2.0.18. It is absent from 2.0.15. The inspected [framework hook installer](https://raw.githubusercontent.com/QTR-Modding/SKSE-Menu-Framework-3/ff6b9d0cc3137a4a8430dfe56456ffedd5a0213e/src/Hooks.cpp) requests 56 trampoline bytes through CommonLib. Its [pinned allocator](https://github.com/alandtse/CommonLibVR/blob/adb3e2c4dff61d151370a6777ed9e77f8e005c62/src/SKSE/API.cpp) supplies a local fallback when an existing interface cannot allocate, but none when the interface itself is missing. This matches the observed failure. The stock framework DLL matches the recorded package hash.

This finding applies to SMF package 3.18 (embedded version label 3.14.0.0), SHA-256 `0c107ad4c471c904186b68e1b50b10a66311affb40eb0ba2b8fcca36735d55a2`, with SKSE 2.0.15 on Skyrim 1.5.73. Older framework archives are available locally but have not been verified as replacements for this test configuration. A newer SKSE build is tied to a different game runtime and is not a drop-in repair. **1.5.97 / SKSE 2.0.20 is the oldest combination with successful live startup in this batch.**

Failure evidence, source snapshots and the official changelog are preserved under `manifests/framework-1.5.73-20260926/`. Both runs restored their shared files. Their original run.json files are retained; the user-reported and log-confirmed failure is recorded separately in the review and coverage manifest.

## Warning review

Only 1.6.1130 produced DDC warnings: one movement-watchdog episode, logged at 16:51:09 local time and ending after 1.60 seconds when movement input was released. It reports `unpaused=0`, `menuF=true`, `smfBlock=false`, `blockPlayerInput=false` and `timeMult=1.000`. The definition in `src/Hooks/HookManager.cpp:8624` means the Dialogue Menu was open. The preceding lines show dialogue face-lock and a POV switch. This is consistent with movement being suppressed during dialogue; the log alone does not establish a stuck-movement defect or prove movement resumed afterward.

SKSE Menu Framework reports the same Polish-font size discrepancy in every run and continues loading successfully. No corresponding framework initialization failure was found.

## Remaining coverage

- **1.5.73:** tested twice and blocked by the current framework dependency. A compatible framework build or allocator fix is needed before DDC’s runtime hooks and gameplay can be assessed.
- **Full gameplay acceptance:** camera/dialogue observations need confirmation because the saved grades remained at their defaults. Logs do not prove completion of combat/projectile, save/reload, preset round-trip, collision, damage, knockdown and other checklist steps. In 1.5.97 the log records switching into first person but no explicit return-to-third-person entry.
- **Integration and comparison profiles:** no 1.2.1 CBPC/TDM, 1.2.0 baseline or dependencies-only run is recorded in this batch.
- **Other loader targets:** historical versions with missing inputs, 1.6.317 without official native-plugin loading, and both GOG targets keep their existing coverage gaps.

## Result-form correction

The launcher used to reset the outcome fields and notes when the game finished. This could discard observations entered while a run was still active. It now resets them at the start of a new run, preserves them on completion, explains that logs save automatically, and clearly reports when a save contains no selected verdict. This is a confirmed launcher defect; whether it caused these particular blank verdicts is unknown. No historical run records or game DLLs were altered. All 14 launcher checks pass, including a hidden GUI test that enters observations during a simulated run and verifies the saved values after completion.

## Evidence

Private run folders: `C:/Skyrim Runtime Tests/DDC-1.2.1/runs/`. The structured review and SHA-256 hashes of the original records, captured manifests and logs are in `manifests/live-results-review.json`; the before-review coverage record and launcher backups are in `manifests/live-review-20260926/`.
