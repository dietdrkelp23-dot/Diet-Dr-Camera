# Building Diet Dr Camera from source

These instructions build the Windows x64 SKSE plugin and its standalone checks.
Skyrim, SKSE, a mod manager, and a Nexus account are **not required to build**.
They are needed for in-game testing. No step below installs the plugin into a game.

## Tools

The verified local toolchain is Visual Studio 2026 Community, MSVC 14.50.35717,
CMake 4.2.3, Windows SDK, Python 3.14.3, and vcpkg's `x64-windows-static-md` triplet.

Install:

1. [Visual Studio](https://visualstudio.microsoft.com/downloads/) with **Desktop
   development with C++**, the MSVC x64/x86 compiler, and a Windows SDK.
2. [CMake](https://cmake.org/download/) **4.2 or newer** for the Visual Studio 2026
   generator. Add CMake to PATH. The project requires C++23.
3. [Git for Windows](https://git-scm.com/downloads/win).
4. [Python](https://www.python.org/downloads/windows/) **3.11 or newer**, on PATH,
   for source packaging and the Python checks. No pip packages are required.

Open PowerShell. Use a short source path such as `C:\src\Diet-Dr-Camera`.

## Source and dependencies

Extract the corresponding source ZIP, or clone this repository. The public source
contains `src/`, `include/`, `extern/CommonLibSSE-NG/`, and `extern/minhook/`.
Verify that `extern/CommonLibSSE-NG/CMakeLists.txt` exists before configuring.
If using a development checkout whose CommonLib is a submodule, run
`git submodule update --init --recursive` first. The complete public source
archive and the prepared public repository already contain its source files.

For the tagged 1.1.0 source:

```powershell
git clone https://github.com/dietdrkelp23-dot/Diet-Dr-Camera.git C:\src\Diet-Dr-Camera
Set-Location C:\src\Diet-Dr-Camera
git checkout v1.1.0
```

Set up vcpkg once, outside the project:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\src\vcpkg
& C:\src\vcpkg\bootstrap-vcpkg.bat
```

Use an existing vcpkg checkout instead if you have one. Its complete Git history
must include baseline `2b65c20fc66eda893aa15a15a453c3cf09500b19`, pinned in
`vcpkg.json`. CMake invokes vcpkg manifest mode to acquire and build dependencies;
do not manually install a different set of library versions.

CommonLib is pinned to alandtse/CommonLibSSE-NG v7.5.4, commit
`c5424463bba9af0d75cde8640ba7ddd4cacb9e39`. `tools/dependency-sources.json` lists
the seven linked vcpkg source packages, including transitive dependencies, their
upstream versions, and archive hashes. Exact recipes and patches are under
`tools/dependency-recipes/`. The published source also includes their complete
extracted trees in `dependency-sources/` for inspection. The normal CMake build
uses vcpkg's pinned recipes and can download tools/dependencies on its first run.
This is not a claim that the initial build is offline.

## Configure, compile, and check

Run from the DDC source root. Replace `C:/src/vcpkg` if needed:

```powershell
cmake -S . -B build/release-candidate -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DDDC_DEPLOY_TO_MO2=OFF -DDDC_BUILD_CHECKS=ON
cmake --build build/release-candidate --config Release --parallel 4
ctest --test-dir build/release-candidate -C Release --output-on-failure
```

Stop if any command fails. The compiled plugin is
`build/release-candidate/Release/DietDrCamera.dll`; its matching debug symbols
are `DietDrCamera.pdb` beside it. The checks exercise runtime layout selection,
hook decoding, camera calculations, settings, and frozen preset compatibility.
They do not launch Skyrim or establish gameplay compatibility with every runtime.

## Recreate the distributable archives

With the same build directory configured above:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package-release.ps1 -BuildDirectory build/release-candidate
```

The script builds Release, runs the checks, stages only declared installation
files, checks the DLL, verifies bundled dependency sources, and hashes every
archive entry. It creates a **new** timestamped directory under `release/` with
the runtime ZIP, corresponding source ZIP, manifest, logs, and private symbols.
The runtime ZIP contains `SKSE/Plugins/DietDrCamera.dll` and the required
stagger-camera animation under `meshes/`. No personal presets are packaged.

`manifest.json` records the source revision and pending source changes, compiler
metadata, and SHA-256 hashes. It connects that candidate's source and binaries.
Different compiler/SDK versions or build paths can produce different binary
hashes; byte-for-byte reproducibility across arbitrary toolchains is not promised.

## Troubleshooting

- **Missing CommonLib CMakeLists:** obtain the full source archive or initialize
  the development submodule. The DLL cannot be built from only `src/`.
- **Unknown Visual Studio 18 generator:** update CMake to 4.2+ and check
  `cmake --help`. CMake added this generator in 4.2.
- **No C++ compiler or SDK:** add the C++ workload and Windows SDK through the
  Visual Studio installer, then open a fresh shell.
- **vcpkg baseline missing:** use a complete vcpkg clone or fetch the missing
  history. Keep the manifest baseline and dependency inventory aligned.
- **Python not found:** ensure the Python executable is on PATH and verify
  `python --version` from the same shell.
- **Build-directory source/toolchain mismatch:** configure into a new directory
  rather than reusing another checkout's CMake cache.
- **Runtime hook error:** retain the full DDC/SKSE logs, executable version, and
  mod list. A successful build does not resolve a runtime-specific hook failure.

See [runtime support](RUNTIME-SUPPORT.md), [release checks](RELEASE-CHECKLIST.md),
and [license information](LICENSING.md).
