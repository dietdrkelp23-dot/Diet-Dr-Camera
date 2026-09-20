# Logs and startup reports - 1.2.0

For a startup error, ask the user for the **complete `DietDrCamera.log`,
`skse64.log`, and the full error message** from the same launch. A source
filename and line number alone do not identify which hook failed. Verbose
Logging is not required for startup diagnostics.

Suggested reply:

> Please reproduce the problem once, then attach DietDrCamera.log and
> skse64.log along with the full error message. Include a Crash Logger report
> if the game crashes. Tell us whether it happens before the main menu, while
> loading a save, or during play, and what you were doing when it happened.
> You do not need to turn on Verbose Logging for a startup error.

## Where to find the files

Use Windows' Documents folder, including its redirected/OneDrive location:

- Steam: `Documents\My Games\Skyrim Special Edition\SKSE\`
- GOG: `Documents\My Games\Skyrim Special Edition GOG\SKSE\`

The current file is `DietDrCamera.log`. Version 1.2.0 keeps three previous
sessions as `DietDrCamera.1.log`, `.2.log`, and `.3.log`, newest first. Copy the
files soon after reproducing the issue; `skse64.log` has its own retention
policy. If the user has restarted, collect the DDC log from the failed launch
and any retained matching SKSE/Crash Logger output.

If the normal log directory cannot be used, DDC attempts
`%TEMP%\DietDrCamera\DietDrCamera.log`. DDC's hook-validation error includes
the actual DDC log path. If neither destination works, DDC reports the logging
failure and does not initialize. Failure before SKSE calls DDC's load entry
point cannot produce a new DDC log; use `skse64.log` and the loader error then.

## What a normal log now records

- Session date/time, process ID, DDC version, DLL location, and PE/PDB build
  identity. The PDB identity distinguishes builds sharing a version number.
- Skyrim and SKSE versions before CommonLib initialization; the known
  Steam/GOG runtime classification, executable location and working directory.
- The Address Library candidate's filename, size, header format and runtime
  match. A readable header is not proof that every relocation is valid.
- A snapshot of loaded native modules before DDC installs hooks, with file
  versions where available, load addresses and image sizes. A module being
  present is not proof that it caused a conflict.
- Startup checkpoints covering SKSE initialization, hook validation and
  installation, input, settings/preset loading, menu integration and unpause
  initialization. Save-load/new-game messages are recorded when received.
- On hook rejection: the operation, relocation IDs, expected/observed call
  targets, bytes at relevant patch sites and any memory-read error. Recognized
  jump stubs are followed up to three times to identify a destination DLL;
  an unknown stub remains unidentified. Changes after validation report the
  actual installation site and, for the UI branch, the saved expected bytes.

Info and fatal lines flush immediately. Per-frame debug output remains off
unless the user enables Verbose Logging. Rotation happens at launch so a
long session retains its startup header; verbose sessions can still be large.
Logs are stored locally and are not uploaded automatically.

For gameplay issues that need extra detail, ask the user to enable Verbose
Logging for one short reproduction and supply their active preset as needed.
These logs do not replace a crash dump or establish compatibility with an
untested runtime.

For high-speed paraglider stutter, leave Verbose Logging **off** initially.
The first four glide captures per launch are buffered automatically. Reproduce
the entry, then land and wait two seconds or open a pause menu so the deferred
`[PARAGLIDE-TRACE]` report can be written. Exiting/crashing mid-flight can lose
the pending in-memory capture. Each capture retains pre-roll and up to the
first 12 seconds/4096 updates, with context rows and neighbours around peaks
reported afterward. Long flights do not produce continuous traces.
The report compares unclamped camera-update timing with DDC follow changes,
profile values and wind modulation. Timing covers CPU camera calls, not GPU
presents; a slow frame does not by itself identify the responsible mod.
`frameMs` retains the actual update interval. Experimental September 19 timing
builds also emitted `followMs`; that filter and field have since been reverted.

## Author verification

`DiagnosticsChecks` verifies retention across five child processes that exit
without destructors, immediate fatal-log persistence, default verbosity,
startup metadata, missing/malformed database headers, loaded-module capture,
bounded memory reads, jump-stub identification and an unusable log destination.
The private runtime-image scenarios exercise production hook failures against
real executables without executing game code. Preserve the exact release DLL
and matching PDB privately so a reported build identity can be matched to its
symbols.
