# Source corresponding to Diet Dr Camera 1.1.0

Tag `v1.1.0` contains the complete source corresponding to the 1.1.0 release.
All 5,172 files from its source archive were verified by SHA-256 against
this checkout. Only this provenance document and the repository ignore file
are additional. The plugin implementation, dependency sources and build recipes
match the archive. The development release commit is `598081ff78c926c927274b1f61648f30a57aa4b8`;
the public tag has its own commit in this repository's source-publication history.

| Artifact | SHA-256 |
| --- | --- |
| Diet Dr Camera-1.1.0.zip | `EC01C003E337C4093F6B50BB154429C4F802BAD20CD11ABFB8D1653DBFC312E5` |
| Diet Dr Camera - Source-1.1.0.zip | `6FBCE00396A4224576DC064D4CA0EE5CE0CE505970A33EA708240FE73F2B78EC` |
| DietDrCamera.dll inside the runtime ZIP | `5B3A303C9E3F7E900E2A06265411397131BFA18FCF276473AA4D89F5AC41D697` |

The desktop upload copies use the shorter names `Diet Dr Camera.zip` and
`Diet Dr Camera - Source.zip`; their bytes and hashes match the archives above.
The runtime package contains only the SKSE plugin and the intentional empty
`staggercamera.hkx` marker that disables the vanilla camera stagger animation.

The source contains the plugin, CommonLibSSE-NG, the MinHook instruction decoder,
all seven linked dependency source trees, pinned recipes/patches, tests and
licenses. No Bethesda executable, game data, personal preset or compiled plugin
is part of the source tree. See [BUILDING.md](BUILDING.md) for complete setup,
build, test and packaging instructions. Skyrim is not required for these steps.

The author approved this release on September 15, 2026. Runtime testing and its
limits are recorded in [RUNTIME-TESTING.md](RUNTIME-TESTING.md). Different compiler,
SDK or build paths can change binary hashes; arbitrary rebuilds are not promised
to be byte-for-byte identical.

For the originally flagged 1.0.0 upload, use
[v1.0.0-source.1](https://github.com/dietdrkelp23-dot/Diet-Dr-Camera/tree/v1.0.0-source.1)
and [its provenance](https://github.com/dietdrkelp23-dot/Diet-Dr-Camera/blob/v1.0.0-source.1/SOURCE-PROVENANCE.md).
That immutable tag remains available for review of the earlier DLL.

## Public source build verification

This exported source checkout was independently configured and built with
Visual Studio 2026/MSVC 14.50, CMake 4.2.3, the pinned vcpkg baseline and the
`x64-windows-static-md` triplet. Auto-deployment was disabled. All 33 standalone
checks passed, and the rebuilt x64 DLL's 1.1.0 metadata and matching PDB were
verified. The release artifacts above retain the exact author-tested DLL;
the independent build establishes that the published source compiles and tests.

Git applies upstream `.gitattributes` line-ending rules to 3026 dependency files.
Their only archive-to-Git differences are CRLF/LF normalization; their source
content was separately verified. All other archive files match their Git blobs
byte for byte. The archive hashes above refer to the unmodified release ZIPs.
