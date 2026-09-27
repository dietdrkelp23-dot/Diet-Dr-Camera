# Runtime test launcher source

The installed test environment is `C:/Skyrim Runtime Tests/DDC-1.2.1`. Use the
**OmniCam 1.3.0 Runtime Tests** desktop shortcut. Legacy directory/profile IDs
remain stable so existing saves, test presets and result records are preserved.
Public profile labels come from each setup manifest; the launcher resolves them
to the original profile ID before validation and launch. The current candidate
manifest supplies the window's version label.

Ten supported runtimes offer 40 profiles: ten main OmniCam profiles, nine CBPC
profiles, one TDM profile on 1.6.1170, ten 1.2.0 baselines and ten dependencies-only
profiles. SE 1.5.97 is the oldest active target. Excluded targets remain in the
private historical evidence and are omitted from both the selector and
`--check-all`. Missing setups are not presented as ready.

The launcher expects prepared `manifests/setup-1.*.json`,
`manifests/targets.json`, private game roots, portable managers, and
`CHECKLIST.md` beside it. Private manifests record machine paths and exact
package hashes; game binaries and saves are not included in this source folder.

Run commands through the windowless Python launcher described in AGENTS.md.
`test_launcher.py` checks shared catalog/log restoration, missing originals,
damaged-backup refusal, process enumeration, hidden process flags, concurrent
sessions, manager path validation, profile normalization, candidate label
routing, excluded targets, first-run preferences and preservation of observations
entered while a test is running. Its GUI fixture stays withdrawn.

`Runtime Tests.pyw --check-all` verifies all 40 active prepared profiles without
launching Skyrim. Add `--full` to hash all game data when content integrity is
in doubt. Both public labels and stable profile names are accepted by
`--profile`. Actual launching uses the desktop shortcut; the automation job
cannot provide MO2's required child-process breakaway.

Fresh profiles suppress the Anniversary download prompt with
`[General] bFreebiesSeen=1` on 1.6+; 1.6.1130+ also gets
`bUpsellOwned=1`. Before launch, the launcher repairs these flags only in the
selected test profile. It does not install Creations or alter Steam DLC settings.

Logs save automatically. Outcome fields reset when a run begins and retain
observations entered during it. Saving untouched default verdicts reports that
no outcome was selected. Runtime setup checks and historical logs do not count
as live acceptance of a new DLL.

The focused checklist is [CHECKLIST.md](CHECKLIST.md); the local runbook is
[RUNTIME-LIVE-TESTING.md](../../RUNTIME-LIVE-TESTING.md). Earlier results remain in
[RUNTIME-LIVE-RESULTS.md](../../RUNTIME-LIVE-RESULTS.md). Preparing the test harness
does not regenerate release archives.
