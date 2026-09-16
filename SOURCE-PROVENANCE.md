# Source corresponding to Diet Dr Camera 1.0.0

This repository starts from the complete source archive supplied with version
1.0.0. All 5,159 original source/archive files were checked against that archive
before preparing the repository. The only changes for GitHub preparation are
additional build/provenance documentation, an ignore file, the README's build
instructions, and packaging metadata. The packager retains these documents in
source exports and correctly identifies bundled vendor files without Git metadata.
The plugin implementation and dependencies are unchanged.

The recorded release artifacts are:

| Artifact | SHA-256 |
| --- | --- |
| Source ZIP | `6BA01A3358AA6616BE526D40AFFC128E56677BA8423BDF2C7BD962A2511CAEF4` |
| Runtime ZIP | `72135C5D0B880B3C50094E79E2CE1DCF741E7E1E4D1D5A77DEDF87752034E21E` |
| DietDrCamera.dll inside the runtime ZIP | `04E33C1C3DB4EBA98008C1A74D0D5CC97FF33F85EADAE49B2DA82323D86CBB89` |

The source includes the plugin, CommonLibSSE-NG, the MinHook instruction decoder,
all linked dependency source trees and their pinned recipes, tests, and licenses.
No Bethesda game executable, personal preset, or compiled plugin is included.
The empty `staggercamera.hkx` installation marker is intentional; it is created
by the packaging rules to disable the vanilla camera stagger animation.

See [BUILDING.md](BUILDING.md) for complete instructions to configure, compile,
test, and recreate distributable archives. Compiler/SDK variations can change
the rebuilt binary's hash. Runtime/gameplay verification is documented separately
in [RUNTIME-SUPPORT.md](RUNTIME-SUPPORT.md).

This source publication supports review of the existing release. It is not an
announcement that a later compatibility update has passed gameplay testing.
