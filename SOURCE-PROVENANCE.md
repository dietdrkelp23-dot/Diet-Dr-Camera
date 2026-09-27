# Source provenance - OmniCam 1.3.0

The 1.3.0 update is based on the hash-verified September 19 1.2.0 release
snapshot, with the subsequent fixes listed in RELEASE-NOTES.md. Native
movement/combat experiments and their OAR bridge are absent from this source.
The pre-rollback development work is preserved privately in the local evidence
directory.

The corresponding source is `OmniCam - Source-1.3.0.zip`. It contains
the working source used for this release, including changes since the base
Git revision, bundled CommonLibSSE-NG/MinHook sources, all seven vcpkg library
source trees, exact dependency recipes, tests, licenses and build instructions.
No game executables, runtime databases, personal presets or logs are included.

`tools/package-release.ps1` snapshots these files before building, verifies
that its inputs have not changed, rebuilds from clean objects, runs the
standalone checks, validates the
DLL's version and matching PDB identity, and checks every archive member's
size and SHA-256. The private candidate's `manifest.json` records the base
revision, working changes, vendor revision, file inventory and artifact hashes.
`SHA256SUMS.txt` identifies the two archives and that manifest. The matching
PDB is retained privately for reports that identify this build in their log.

The source ZIP is complete without Git metadata. Follow `BUILDING.md` to
configure and build it with automatic deployment disabled. Compiler/SDK
differences may change binary hashes; arbitrary toolchain reproducibility is
not promised. Automated checks do not establish gameplay compatibility with
every Skyrim runtime. See `RUNTIME-SUPPORT.md` and `RELEASE-CHECKLIST.md`.

Release preparation creates local artifacts. This document does not assert
that a version 1.3.0 GitHub tag or Nexus upload has been published.
