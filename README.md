# Diet Dr Camera

An SKSE camera overhaul with per-state framing, first-person FOV and effects,
dialogue cameras, menu framing, target-lock integration, and shareable TOML presets.
Configure it through SKSE Menu Framework; the mod has no ESP or Papyrus scripts.

See [requirements and installation](Diet%20Dr%20Camera%20-%20Requirements%20and%20Credits.txt)
and [release notes](RELEASE-NOTES.md). One CommonLibSSE-NG DLL targets the known
SE and AE releases, including Steam/GOG 1.6 and Steam 1.7. See
[RUNTIME-SUPPORT.md](RUNTIME-SUPPORT.md) for the version list and validation status.
See [PRESET-AUTHORS.md](PRESET-AUTHORS.md) for sharing presets on Nexus.
For a startup failure, send the complete `DietDrCamera.log`, `skse64.log` and
error message from the same launch. Version 1.2.0 records startup diagnostics
automatically; Verbose Logging is not required. See
[SUPPORT-LOGGING.md](SUPPORT-LOGGING.md) for log locations and reporting steps.

## Build and package

See [BUILDING.md](BUILDING.md) for complete setup, build commands, outputs,
dependency provenance, and troubleshooting suitable for source review.

Use Windows x64, Visual Studio with Desktop development with C++, CMake 4.2+
and vcpkg. Packaging also requires Python 3.11 or newer. The manifest pins the dependency baseline. Keep the bundled
`extern/CommonLibSSE-NG` sources, pinned to alandtse's v7.5.4, and the bundled
MinHook instruction decoder. Visual Studio must support C++23.
Run these commands from the repository root in an x64 Visual Studio developer shell:

```powershell
cmake -S . -B build/release-candidate -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DDDC_DEPLOY_TO_MO2=OFF -DDDC_BUILD_CHECKS=ON
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package-release.ps1 -BuildDirectory build/release-candidate
```

The packaging script builds Release, runs the standalone checks, installs an
explicit file list into a new staging directory, then verifies and hashes the
archives. Output goes to a new timestamped folder under `release/`; existing
archives are retained. It never copies a live mod installation or personal presets.

The main archive uses a Data-root layout (`SKSE/Plugins` and `meshes` at its root).
Only the DLL and required stagger-camera animation are installed. Matching PDB
symbols are retained in the private candidate symbols/ folder for crash analysis.
The source archive includes
the working source, bundled CommonLib, all seven vcpkg library source trees,
exact dependency recipes/patches, checks, and build/package instructions;
it includes pending edits, rather than silently exporting only the last commit.
`manifest.json` records the source revision, pending changes, and file hashes.

Auto-deployment is off by default. For local development, explicitly set
`DDC_DEPLOY_TO_MO2=ON` and provide `MO2_MOD_DIR` or `SKYRIM_MOD_FOLDER`.
Release packaging requires auto-deployment to be off.
Windows DLL metadata and the SKSE version both come from `CMakeLists.txt`.
Keep the manifest version and release text aligned when changing the version.

For a source ZIP, extract it to a short path such as `C:/src/DDC` and run the
same commands there; no Git checkout or submodule download is needed. For a Git
checkout, clone with `--recurse-submodules` to include the pinned CommonLib.
Put Python and CMake on PATH, and replace the example vcpkg path with yours.
The source ZIP contains no game files; building the DLL does not require Skyrim.

Release diagnostics use relative source paths and a filename-only PDB reference.
Keep the matching PDB with the private release records for crash analysis.
Startup diagnostics are always enabled and retain three previous DDC sessions.
Additional in-game tracing is available through Verbose Logging; it is off by default.

## Release verification

Follow [RELEASE-CHECKLIST.md](RELEASE-CHECKLIST.md) before publishing. Passing a
compiler and packaging check cannot establish that gameplay hooks work on a
different Skyrim runtime. Preset changes must follow [PRESET-COMPAT.md](PRESET-COMPAT.md).

DDC's original code is GPL-3.0-or-later with the modding and linking permissions
in EXCEPTIONS.md. See [LICENSING.md](LICENSING.md) and [LICENSE.txt](LICENSE.txt).
Third-party components retain their own terms; see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)
and `licenses/`. Distribute the corresponding source ZIP alongside each DLL release.

Dependency source inputs are verified against the installed vcpkg package metadata.
`tools/dependency-sources.json` pins their versions and hashes; the exact vcpkg
recipes and patches are in `tools/dependency-recipes/`. If changing the dependency
baseline or versions, refresh these together. The packager refuses mismatched inputs.
The source archive's `dependency-sources/` contains extracted upstream trees and
can be reused when repackaging the same dependencies. A normal vcpkg build still
uses the pinned baseline; manual library builds must apply the included recipe patches.
