# Live runtime testing — OmniCam 1.3.0

Open **OmniCam 1.3.0 Runtime Tests** on the desktop. Start with
**1.6.1170 / OmniCam 1.3.0**, then repeat the focused checklist on all ten
available versions. Close the main Mod Organizer and Skyrim first; keep Steam
running. The launcher opens the isolated test manager after validating its files.

Current test DLL: SHA-256
`485e0f0514f56f296ff4f3a0eff0f9799b8c8cdcc31d1c1b016bbeaa4bdc3567`.
This includes the death/ragdoll controls, repaired startup guard, target-lock
handling, vanity visibility/navigation and the preceding 1.3.0 features.
After the sweep, the author confirmed all ten versions work with this DLL.
The saved per-run grades remain unchanged; integration/checklist items were
not individually graded by that report. The subsequent rapid three-word
Whirlwind Sprint noise fix was also confirmed in the main setup; see
[RUNTIME-LIVE-RESULTS.md](RUNTIME-LIVE-RESULTS.md) for both tested hashes.

Prepared versions: **1.5.97, 1.6.318, 1.6.323, 1.6.342, 1.6.353, 1.6.629,
1.6.640, 1.6.1130, 1.6.1170 and 1.7.104**. The 40 active profiles comprise
ten main candidate profiles, nine CBPC profiles, one TDM profile on 1.6.1170,
ten baselines and ten dependencies-only profiles. TDM is stock 2.2.7 with
SkyUI and MCM Helper. Other versions retain their integration coverage gaps.

Use **Open checklist** for the current procedure. It emphasizes startup and
reload, POV/menu controls, Vanity tab-to-toggle navigation, both camera-disable
toggles, free look/compass, slow motion on/off, recovery and target-lock switching.
Additional 1.3.0 feature checks are separated from the per-version core pass.
F1 opens SKSE Menu Framework in these profiles.

Existing test saves, presets and outputs remain separate per version/profile.
**Author preset - test copy** is available for explicit compatibility testing;
use a new name for edited presets. Result notes entered while playing survive
completion. Quit normally and keep the launcher open until it restores shared
SKSE logs and the Creations catalog, then grade only what you tested.

The installed harness stays at `C:/Skyrim Runtime Tests/DDC-1.2.1/`. Legacy
profile/folder IDs preserve previous results; the visible names identify OmniCam
1.3.0. Candidate identities, setup validation and historical evidence are in its
`manifests/` directory; new runs go to `runs/`. Previous runs keep their
original hashes and grades. See [the earlier results](RUNTIME-LIVE-RESULTS.md).

SE 1.5.73 and older are excluded. GOG 1.6.659/1.6.1179 need matching owned game
files. 1.6.317's official SKSE disables native-plugin loading; 1.7.99 lacks the
matching prepared SKSE/data setup. None is silently substituted with another
version. Offline hook checks, setup verification and live acceptance remain
separate.

Source and launcher checks are in [tools/runtime-live](tools/runtime-live/README.md).
The author approved the Whirlwind Sprint follow-up and requested the final
1.3.0 release preparation. Packaging and source checks are documented in
[RELEASE-CHECKLIST.md](RELEASE-CHECKLIST.md).
