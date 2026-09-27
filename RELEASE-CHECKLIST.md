# Release verification - 1.3.0

OmniCam 1.3.0 uses the released 1.2.0 camera and external TDM integration with
the additions and fixes in RELEASE-NOTES.md. The 1.2.1 development candidates
are superseded. Existing presets retain their authored values and defaults;
new saves use format 17. No personal settings or presets belong in the main ZIP.

## Package gates

Run tools/package-release.ps1 with DDC_BUILD_CHECKS=ON and
DDC_DEPLOY_TO_MO2=OFF. It must:

- Snapshot the exact working sources, including new files, and reject changed
  inputs during packaging. Build Release with --clean-first so restored source
  timestamps cannot reuse stale objects.
- Pass all 48 CTests, including preset compatibility, runtime layouts/hooks,
  projectile callbacks, flyby continuity, DBVO turns and preference persistence,
  Combat FOV pause/preview, Apply scopes, directional navigation, death/ragdoll
  startup guards, camera/input policy, target-lock handoff and slow-motion cleanup.
  NoiseTransitionChecks includes rapid same-profile shout restart continuity.
- Verify x64, Windows/SKSE version 1.3.0, OmniCam branding, matching private PDB
  identity, required SKSE exports and embedded license notices.
- Verify every main/source ZIP member's size and SHA-256 against the snapshot,
  with complete dependency sources, recipes, licenses and build guidance.
- Keep the main ZIP limited to DietDrCamera.dll and the required stagger-camera
  animation. Keep symbols, logs, game fixtures and personal presets private.

Run all seven native ImGui/menu-controller checks separately against the pinned
framework source. They cover both IO layouts, four UI scales, Apply/Reset and
popup navigation, short lists, sliders and the Vanity tab-to-toggle route.
Confirm the source archive contains all linked root guides and current changes.

The candidate's checks.log, binary-check.txt, manifest.json and SHA256SUMS.txt
record the final build and exact artifacts. The source ZIP must match that
candidate. Desktop and workspace delivery copies must match their checksums.
Only the main/source ZIPs are upload payloads; the approved infinity thumbnail
is page artwork. Local release preparation does not publish a Nexus upload,
GitHub tag or support message.

## Author acceptance

On September 27 the author confirmed **all ten prepared versions work** after
the runtime sweep of candidate
`485e0f0514f56f296ff4f3a0eff0f9799b8c8cdcc31d1c1b016bbeaa4bdc3567`:
1.5.97, 1.6.318, 1.6.323, 1.6.342, 1.6.353, 1.6.629, 1.6.640, 1.6.1130,
1.6.1170 and 1.7.104. Ten new main-profile run records identify that DLL. Their
original default grades and notes remain unchanged; the author's report is
recorded separately. It does not imply every integration profile or every
checklist item was independently graded.

The subsequent third-person shout-noise handoff candidate
`1c237def2af60422fb5caa063fc98dc14d69fd0ebb1d57fcc15500caf4915b53`
was confirmed with **“its good now”** for rapidly repeated three-word Whirlwind
Sprint. The author then requested the final 1.3.0 release preparation. That
follow-up changes overlapping noise handoffs without changing camera-follow
math, runtime hooks, input policy or preset tuning.

The final clean package is built from that accepted gameplay source. Its
manifest identifies the rebuilt DLL; these reports are not relabeled as live
launches of a different binary hash. See RUNTIME-LIVE-RESULTS.md for the dated
acceptance and preserved test records.

Earlier accepted development behavior includes equal R3 hold timing, spatial
controller navigation, the revised Apply popup and the Dismembering Framework
corpse-crossing shake fix. DBVO 2 and its smoothing follow-up were exercised in
Finale. The broader DBVO-COMPATIBILITY.md checklist remains separately ungraded.
Temporary cosmetic-projectile instrumentation was removed; startup and error
diagnostics remain automatic.

## Final feature coverage

- Disable Death Camera and Disable Ragdoll Camera retain normal camera control,
  connected compass heading and TDM framing/lock/unlock/target switching. Both
  default off. Slow Motion remains available independently; other camera tuning
  hides while disabled and retains its values. Death duration, Infinite Duration,
  skip/reload and cleanup remain available.
- Vanity Camera hides other settings while disabled; Down from the tab enters
  its toggle and Up returns to the tab.
- Combat FOV updates live in its settings preview, blends on combat entry/exit,
  and hides its sliders while disabled. DBVO Camera Switching names its optional
  requirement and has no effect without DBVO 2.
- Apply offers Entire Tab, matching Indoor/Outdoor and All Tabs, each with an
  override-inclusive choice. All Tabs includes existing specific weapon/animation
  entries within the section. Override and specific-entry editors show Reset
  only. Per-slider copying preserves neighboring values and override toggles.
- Passive sunlight/self effects do not repeatedly arm Damage Reaction after
  Vampire Lord transformation. Rapid shout chains preserve their outgoing noise
  waveform. Older presets and current saved fields have automated coverage.

## Runtime limits

SE 1.5.97 / SKSE 2.0.20 is the oldest supported combination. The earlier 1.5.73
framework allocation failure remains historical and outside support. GOG
1.6.659/1.6.1179 executable fixtures remain unavailable. 1.6.317's official SKSE
disables native plugin loading; 1.7.99 has no complete prepared live setup.
Do not count those gaps as live passes or claim every possible mod combination
was tested. Offline instruction/layout checks are separate from actual gameplay.
See RUNTIME-SUPPORT.md and RUNTIME-MATRIX.md.

No further gameplay/source changes are introduced during final packaging.
Preserve the author's preset/configuration files, backups and earlier results.
