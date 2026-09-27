# OmniCam

Formerly Diet Dr Camera. The 1.3.0 rebrand keeps the existing installed DLL,
settings folder and preset defaults, so existing presets keep working.
The legacy `DietDrCamera.dll`, `DietDrCamera.pdb` and
`SKSE/Plugins/DietDrCamera/` paths are intentional upgrade compatibility names.

Main category sliders in Third Person, Target Lock, Camera Noise and First Person
have **Apply** beside **Reset**. Override editors and Specific Weapons/Animations
editors keep Reset only. Apply copies that value to matching sliders in the
current tab, matching Indoor or Outdoor profiles where applicable, or all tabs
in the same section. Each scope offers a separate choice to include overrides.
All Tabs includes existing Specific Weapons and Specific Animations entries.
Other values and override toggles are preserved; Apply does not create bindings.

An SKSE camera overhaul with per-state framing, first-person FOV and effects,
dialogue cameras, menu framing, target-lock integration, and shareable TOML presets.
Configure it through SKSE Menu Framework; the mod has no ESP or Papyrus scripts.

See [requirements and installation](OmniCam%20-%20Requirements%20and%20Credits.txt)
and [release notes](RELEASE-NOTES.md). One CommonLibSSE-NG DLL targets the known
SE and AE releases starting at SE 1.5.97, including Steam/GOG 1.6 and Steam 1.7. See
[RUNTIME-SUPPORT.md](RUNTIME-SUPPORT.md) for the version list and validation status.
See [PRESET-AUTHORS.md](PRESET-AUTHORS.md) for sharing presets on Nexus.
For a startup failure, send the complete `OmniCam.log`, `skse64.log` and
error message from the same launch. Version 1.3.0 records startup diagnostics
automatically; Verbose Logging is not required. See
[SUPPORT-LOGGING.md](SUPPORT-LOGGING.md) for log locations and reporting steps.

Version 1.3.0 adds Combat FOV, optional DBVO 2 camera switching, entry-to-tab
paste and spatial controller navigation. It also fixes persistent sunlight
Damage Reaction shaking, cosmetic blood-triggered noise, and R3 hold timing.
It builds on the released 1.2.0 camera and external TDM integration. Existing
presets remain readable; newly saved presets use format 17.

**Disable Death Camera** and **Disable Ragdoll Camera** are at the top of their
Extras tabs. Each keeps your normal gameplay view instead of switching to the
native camera. Looking and the compass stay connected; TDM framing and
lock/unlock/target switching continue in both states. Both toggles default off.
Slow Motion remains available independently; the other camera controls hide
while preserving their tuning. Death duration and the skip hotkey stay available.

Disabling Vanity Camera hides its remaining controls. Controller navigation
enters its disable toggle directly from the tab. Repeated shouts, including
full three-word Whirlwind Sprint, retain a smooth camera-noise handoff.

Combat FOV is under Extras > Cinematic Effects. Enable it to reveal FOV and
Transition Speed controls; selecting the panel previews changes live, including
while the settings menu pauses gameplay. It is off by default.

With DBVO 2 installed, third-person dialogue can frame the player's head while
choosing an option and through their voiced line. **Camera Switching (Requires
DBVO 2)** in Dialogue > Global controls this behavior. Turn it off to keep the
camera on the NPC; it has no effect when DBVO 2 is absent. The integration
follows native audio playback, including skipped lines.
See [DBVO 2 compatibility and testing](DBVO-COMPATIBILITY.md).

Copy an entry, hover a configuration tab header, and use your Paste binding to
apply it throughout that tab. Right-clicking the tab also offers **Paste Entry
to All**. This includes both Outdoor and Indoor settings and uses the same
compatible entry types and overrides as an ordinary entry paste. The controller
and keyboard hints distinguish **Paste Entry to All** from **Paste Entire Tab**
according to what you copied. Directional navigation follows nearby controls
and panels in the direction you press. A toggle directly below a tab comes
first, then the entry list if it is below that toggle; other layouts follow
their own control positions.

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
Startup diagnostics are always enabled and retain three previous OmniCam sessions.
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
