# DBVO 2 dialogue camera

OmniCam 1.3.0 offers Camera Switching (Requires DBVO 2) under Dialogue > Global. When
enabled and SKSE reports a loaded DBVO plugin with major version 2, it uses
the player reverse shot described below. Disable the toggle to keep framing
on the NPC during choices and player speech. First-person dialogue keeps its
existing NPC-facing behavior. DBVO 1 is outside this integration.

In third person, the existing dialogue rig sweeps to the player's head while
choosing a response. A queued/playing player voice keeps that shot, including
when DBVO starts the NPC response early. After playback ends or is skipped,
the NPC owns the shot when delivering a response; a delay before that response
does not produce a brief back-and-forth camera move. Closing dialogue, loading
a game, changing speaker, or disabling dialogue clears the transient state.

The reverse sweep accelerates from rest and preserves velocity when its target
changes mid-turn. Closing from the player shot carries its lateral anchor
offset back into gameplay instead of dropping it on the close frame. Temporary
DBVO handoff/exit event logging was removed after the diagnostic capture.

The camera preference is stored as general.dialogue_dbvo_camera_switching in
format-16 presets. It defaults to true so existing presets retain their behavior.
Disabling it clears transient voice/shot ownership, including while paused;
the existing reverse sweep eases back to the NPC. Voice settings, DBVO mod files
and hooks are untouched. The retired dialogue_lock_on_player_face key stays
retired. DBVO is optional and must support the user's Skyrim executable itself.

## Playback evidence

Reviewed the author's DBVO 2.0.2.2 archive on 2026-09-26:
[Dragonborn Voice Over 2](https://www.nexusmods.com/skyrimspecialedition/mods/84329).
The installed DBVO.dll SHA-256 is:

```text
be4929707e578c1add6186d7f29ef0746f7219130160b573a2b61f8c7fb26159
```

The DLL exposes SKSE registration exports, not a public playback API. Its
native playback path calls Actor::SpeakSound. In the 1.6.1170 executable,
SpeakSound stores the returned sound handle through AIProcess at channel 0,
HighProcessData::soundHandles[0] (offset 0x2FC). The DBVO start/stop paths use
that same channel; the start path sets greetingPlayer and soundDelay, including
its audio-only fallback when no lip resource is available.

DDC reads those public CommonLib fields and uses BSSoundHandle::IsValid and
IsPlaying. A local copy clears assumeSuccess before querying, so the engine
reports actual audio status rather than a stale copied kPlaying value. It
never writes the actor's sound handle. A queued valid handle is held until
playback begins; after observed playback ends, a stale greeting/duration cannot
re-arm it. Paused menus retain the turn state. There is no estimated line
length and no subtitle dependency. VOICE_STATE is shout state and is not used.

The compatibility implementation has no hardcoded DBVO addresses or game
function hooks. Private disassembly used for this review remains under ignored
build/diagnostics/dbvo2-20260926 and is not distributed in either archive.

## Verification

DBVODialogueChecks exercises NPC greetings, indefinite option selection,
delayed audio startup, long player lines with changing NPC activity, repeated
prompts, actual completion with stale timers, skip/failure, paused audio,
first-/third-person switches, delayed NPC replies, close/disable and load reset.
PresetCompatibilityChecks runs the unchanged production preset codec and
immutable released fixtures. All 45 release CTests pass with the transition
repair. The initial DBVO candidate's clean offline hook
preflight passed on all 12 available supported Steam executables (the two GOG
images remain missing). Independent native-voice inspection verified channel 0
and the three sound-handle fields on 1.5.97, 1.6.640, 1.6.1170 and 1.7.104.
These checks establish code/layout behavior, not in-game camera acceptance.

The first in-game session (19:22-19:30 on 2026-09-26, DLL 744436ec...) loaded
DBVO successfully, but the author reported occasional snaps. Its normal log
has no per-turn camera trace, so the final snap cannot be attributed to a
specific handoff. Source review found the abrupt reverse-sweep start and the
missing lateral exit carry described above. DialogueCameraMotionChecks covers
30/60/72/144 Hz sweeps, retarget continuity, skipped/rapid turns, long frames,
and closing from either side while the gameplay profile changes.

The follow-up 20:09-20:26 Finale session (DLL 460210bb...) captured the final
exit at 20:26:36.953. Position had reached the sheathed profile by about 0.51s,
while FOV was still 69.6 on its way from 60 to 100 (94.2 at about 1.01s). The
configured durations were 0.379s for position and 1.377s for FOV. This supports
the author's assessment that differing transition timing explains the perceived
zoom. Timing and framing are unchanged; the investigation is closed at the
author's request. Both the global configuration and active preset have verbose
logging off. This does not certify every DBVO gameplay case.

Gameplay acceptance remains pending. Use the installed DBVO 2.0.2.2 with the
selected voice pack and test:

1. Let the NPC finish a greeting, then wait on the option list. The camera
   should settle on the player's head.
2. Pick a long voiced answer. It should stay on the player for the whole line,
   then return for the NPC response. Repeat with player subtitles disabled.
3. Skip a player line; select an option with no matching audio; repeat the same
   prompt. None should leave the camera stuck on the player.
4. Test DBVO's negative and positive NPC response delay, pause during speech,
   switch POV mid-line, and close/reopen dialogue. Close while choosing a
   response and partway through a turn; walk/sprint immediately after closing.
5. Disable DBVO and verify ordinary DDC dialogue. Confirm the existing R3 tap
   and equal hold timing on the final mod list.
