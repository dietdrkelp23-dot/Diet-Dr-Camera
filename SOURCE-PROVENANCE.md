# Source corresponding to Diet Dr Camera 1.1.1

Tag `v1.1.1` contains the complete source corresponding to the 1.1.1 release.
All 5,173 files from its source archive were verified by SHA-256 against
this checkout. Only this provenance document and the repository ignore file
are additional. The plugin implementation, dependency sources and build recipes
match the archive. The development release commit is `f98c1e7129b72ffdc87f4a6460b5fd1222623930`;
the public tag has its own commit in this repository's source-publication history.

| Artifact | SHA-256 |
| --- | --- |
| Diet Dr Camera-1.1.1.zip | `5A685E31F4BA0E3558E1E252EAE86FA02501D15B7EE26E78F8ABB20F270B179C` |
| Diet Dr Camera - Source-1.1.1.zip | `53E6BE095906440CC70CEE303C0381C4E3ECB9E5591DE5E50076EF3769E2CA4C` |
| DietDrCamera.dll inside the runtime ZIP | `9EA052B8B5C06EC976E692AAA222914E27FB122FF9BE611784B120523A0B1F53` |

The desktop upload copies use the shorter names `Diet Dr Camera.zip` and
`Diet Dr Camera - Source.zip`; their bytes and hashes match the archives above.
GitHub uses compact archive filenames with the same bytes and hashes.
The runtime package contains only the SKSE plugin and the intentional empty
`staggercamera.hkx` marker that disables the vanilla camera stagger animation.

The source contains the plugin, CommonLibSSE-NG, the MinHook instruction decoder,
all seven linked dependency source trees, pinned recipes/patches, tests and
licenses. No Bethesda executable, game data, personal preset or compiled plugin
is part of the source tree. See [BUILDING.md](BUILDING.md) for complete setup,
build, test and packaging instructions. Skyrim is not required for these steps.

This patch fixes Sheathed/Unsheathed tab copying in both directions. Runtime
testing of the preceding 1.1.0 builds and its limits are recorded in [RUNTIME-TESTING.md](RUNTIME-TESTING.md). Different compiler,
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
checks passed, and the rebuilt x64 DLL's 1.1.1 metadata and matching PDB were
verified. The release artifacts above retain the exact packaged patch DLL;
the independent build establishes that the published source compiles and tests.

Git applies upstream `.gitattributes` line-ending rules to 3026 dependency files.
Their only archive-to-Git differences are CRLF/LF normalization; their source
content was separately verified. All other archive files match their Git blobs
byte for byte. The archive hashes above refer to the unmodified release ZIPs.
