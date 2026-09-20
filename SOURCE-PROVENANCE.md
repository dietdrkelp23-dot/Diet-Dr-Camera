# Source corresponding to Diet Dr Camera 1.2.0

Tag `v1.2.0` contains the complete source corresponding to the verified 1.2.0
release package. The implementation, tests, dependencies and build recipes
come from its 5,251-file source archive. Git may normalize CRLF/LF line endings;
source content is checked against the archive before publication.

Two documentation files differ from the archive: this provenance record and
`nexus/CHANGELOG-1.2.0.txt`, which contains the author's simplified changelog.
The repository's `.gitignore` is additional. The runtime and source ZIPs are
unchanged; these text edits do not change the packaged DLL or its build inputs.

The development base revision is `f98c1e7129b72ffdc87f4a6460b5fd1222623930`.
The public tag has its own commit in this repository's publication history.
CommonLibSSE-NG remains pinned to `c5424463bba9af0d75cde8640ba7ddd4cacb9e39`.

| Artifact | SHA-256 |
| --- | --- |
| Diet Dr Camera-1.2.0.zip | `89B14BF033A6B4162CBAE8C12A48D4C2EFECCDCB481B337377719C305994D154` |
| Diet Dr Camera - Source-1.2.0.zip | `467A244EC536056A986996289963130D0CC0CA89BBE955E4FBCDC26DEEBFBF45` |
| DietDrCamera.dll inside the runtime ZIP | `1A41D065F534E3243325E439B46E0502CCEF3C0C5FB569DE9F3D27543213B170` |

The Desktop upload copies use the shorter names `Diet Dr Camera.zip` and
`Diet Dr Camera - Source.zip`; their bytes match the corresponding archives.
The runtime ZIP contains only the plugin and the intentional empty
`staggercamera.hkx` animation override. Matching PDB symbols remain private.

The source includes the plugin, CommonLibSSE-NG, MinHook, all seven linked
vcpkg library source trees, pinned recipes/patches, tests and licenses. No game
executable, private runtime fixture, personal preset or compiled plugin is
included. See [BUILDING.md](BUILDING.md) for complete build instructions.
Skyrim and a Nexus account are not required to compile or run standalone checks.

## Verification and runtime coverage

The release build passed all **41 standalone checks**. The exact source ZIP
was also extracted outside the development checkout and configured and built
with Visual Studio 2026/MSVC 14.50, CMake 4.2.3, the pinned vcpkg baseline and
`x64-windows-static-md`. Auto-deployment was disabled. That clean build also
passed all **41 checks**, including release metadata/PDB pairing and automatic
startup logging. No implementation or build-recipe changes were made for this
publication, so those results apply to the same source content.

Fresh offline checks passed **210 hook scenarios on 21 Steam executables**,
with **47 hooked vtable targets per executable**. GOG 1.6.659 and 1.6.1179
executable fixtures remain unavailable. Offline checks do not execute the game
or establish gameplay compatibility. The final packaged DLL has not had a new
in-game smoke test during release preparation; earlier startup results belong
to the build identities recorded in [RUNTIME-TESTING.md](RUNTIME-TESTING.md).

Version 1.2.0 includes automatic startup diagnostics and retains three previous
DDC logs. See [SUPPORT-LOGGING.md](SUPPORT-LOGGING.md) for reporting failures and
[RUNTIME-SUPPORT.md](RUNTIME-SUPPORT.md) for runtime targets and remaining gaps.
Logging is intended to help investigate other-runtime/mod conflicts; it is
not proof that all reported failures are fixed.

Compiler, SDK and build-path differences can change binary hashes; arbitrary
rebuilds are not promised to be byte-for-byte identical. The artifact hashes
above identify the unchanged release archives, not GitHub's generated ZIPs.
Earlier tags remain available for review of their corresponding releases.
