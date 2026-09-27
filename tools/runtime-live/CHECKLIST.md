# OmniCam 1.3.0 — focused runtime checks

Open **OmniCam 1.3.0 Runtime Tests** on the desktop. Keep Steam running and
close the main Mod Organizer and any Skyrim session before clicking **Launch test**.
Start with **1.6.1170 / OmniCam 1.3.0**, then repeat the core pass on every
version in the selector. Use disposable test saves and a new test preset.

The launcher keeps each version/profile's saves, presets and outputs separate,
records the exact DLL, and restores shared SKSE logs and the Creations catalog.
Keep it open until the test ends and restoration finishes.

## Core pass on all ten versions

- [ ] Reach the main menu without a crash or loader/hook error. F1 opens the framework; select OmniCam.
- [ ] Load a disposable test save, or enter **coc qasmoke** in Skyrim's console at the main menu to create a test character. Walk, sprint, crouch, jump, draw/sheath and orbit.
- [ ] Switch first/third person in both directions. Without TDM, a short R3 tap must not move the camera or change POV; holding for at least 0.2 seconds and releasing switches once in either direction.
- [ ] Open/close inventory, magic, favorites, the framework and Quick Tune. Controls resume afterward. In **Extras > Vanity Camera**, Down from the tab must land on **Disable Vanity Camera**, and Up must return to the tab. Disabling vanity hides its other settings; re-enabling shows the stored values.
- [ ] With **Disable Death Camera** and **Disable Ragdoll Camera** unchecked, confirm their ordinary camera behavior is still available.
- [ ] Enable **Disable Ragdoll Camera** and set its **Slow Motion** to 0%. Cause a recoverable knockdown. The normal camera follows the player; looking and the compass stay connected, and opening/closing a menu does not strand the view. Recovery restores normal gameplay without a snap or stuck camera.
- [ ] Enable **Disable Death Camera** and set its **Slow Motion** to 0%. For more testing time, use **Infinite Duration**. Let an enemy kill the disposable character. Look horizontally and vertically, check the compass, and use the configured death skip/reload control. Confirm the next save loads normally.
- [ ] Check the two disable toggles independently: suppressing one must not suppress the other. Slow-motion controls remain available in both tabs when their camera is disabled.
- [ ] Repeat knockdown/death with a noticeable slowdown, for example 50% for five seconds. Check looking with slow motion on versus off, the fade control, and return to normal time after recovery/reload. Record any sensitivity difference separately from a frozen camera or input failure.
- [ ] Save a NEW preset such as **OmniCam 1.3.0 Smoke**, change a value and reload it. Save the disposable game, quit normally, relaunch the SAME version/profile, and load that save. No startup crash, stuck time effect or stale camera state.

The ten versions are 1.5.97, 1.6.318, 1.6.323, 1.6.342, 1.6.353,
1.6.629, 1.6.640, 1.6.1130, 1.6.1170 and 1.7.104.

## Target lock — 1.6.1170 / OmniCam 1.3.0 - TDM

Wait for the MCM to initialize, then enable/configure TDM Target Lock. This
profile contains stock TDM 2.2.7, SkyUI and MCM Helper.

- [ ] Acquire a live target, switch targets, unlock and relock during ordinary gameplay. R3 taps lock/unlock; holds still switch POV.
- [ ] While locked on, enter a recoverable ragdoll with its camera disabled. Maintain target framing; switch targets and unlock/relock. Unlocking returns free look from the current tracked angle.
- [ ] Repeat during death with its camera disabled and Infinite Duration available. Check target switching, loss/death of the target, unlocking and relocking.
- [ ] Repeat with slow motion on and off; open/close a menu, recover or reload, and verify normal control.

Other versions have no prepared TDM profile. Do not count the base profile as
a target-lock integration pass. The full Finale combat stack is a separate
integration check.

## Additional 1.3.0 checks on 1.6.1170

- [ ] **Combat FOV:** controls hide while disabled; enabling and adjusting FOV/Transition Speed updates the menu preview; entering/leaving combat transitions correctly.
- [ ] **Apply:** the popup uses horizontal rows and centered Cancel. The current indoor/outdoor scope and All Tabs work. Override editors, Specific Weapons, Specific Animations and Transition Overrides do not show per-slider Apply.
- [ ] Load **Author preset - test copy** explicitly for an older-preset compatibility check; save under a new name. The supplied copy and prior test tuning are preserved.
- [ ] In a setup with DBVO 2, test Camera Switching enabled/disabled. The base profiles have no DBVO 2, so verify the requirement label and ordinary NPC dialogue there instead.
- [ ] In the full integration setup, repeat Vampire Lord combat/noise and leaving the area. No unexplained shake persists after its source is gone.

## Comparison and coexistence profiles

- **OmniCam 1.3.0 - CBPC:** available on nine versions; repeat startup, loading and camera movement to check hook coexistence. This is a minimal CBPC profile.
- **Dependencies only:** use for a startup failure without OmniCam.
- **Baseline 1.2.0:** compare the same scene/settings when investigating a regression. New 1.3.0 controls are absent in the baseline.

## Save the actual result

Quit Skyrim normally, then select Startup and Gameplay results, enter what you
tested and click **Save result for this run**. Notes entered while playing are
retained. Use **startup pass / gameplay not-run** for a menu-only launch and
**partial** for an incomplete applicable pass. An empty error log or normal exit
does not establish gameplay success. Keep failures and their exact last action.

Runs retain their original profile IDs and DLL hashes. Existing DDC folder names
are storage identities; the launcher displays the current OmniCam version.
1.5.73 remains excluded. Missing GOG and 1.7.99 setups are not offered; 1.6.317
remains unavailable for native-plugin testing with its official SKSE.
