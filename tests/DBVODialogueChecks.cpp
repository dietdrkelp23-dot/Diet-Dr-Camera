#include "Dialogue/DBVODialogueTurn.h"

#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    using DietDrCamera::DBVODialogueTurn;
    using DietDrCamera::IsDBVO2Version;
    Require(IsDBVO2Version(0x02000022), "DBVO 2.0.2.2 was not recognized");
    Require(IsDBVO2Version(0x02000015), "DBVO 2.0.1.5 was not recognized");
    Require(!IsDBVO2Version(0x01000100) && !IsDBVO2Version(0) &&
            !IsDBVO2Version(0x03000000), "unsupported DBVO protocol was enabled");

    DBVODialogueTurn turn;
    DBVODialogueTurn::Frame f;
    f.active = true;
    f.npcSpeaking = true;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "NPC greeting targeted the player");
    f.npcSpeaking = false;
    for (int i = 0; i < 600; ++i) turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "choosing an option timed out");
    Require(!turn.IsPlayerVoiceActive(), "option selection was mistaken for audio");
    Require(!turn.ShouldLookAtPlayer(true, false), "Disabled camera switching still framed option selection");

    // A delayed sound load must not hand the shot to an NPC topic that DBVO
    // hasn't cleared yet. There is no guessed timeout on a valid queued sound.
    f.soundID = 41;
    f.soundValid = true;
    f.soundQueued = true;
    f.npcSpeaking = true;
    for (int i = 0; i < 600; ++i) turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "queued player line lost the reverse shot");
    Require(!turn.ShouldLookAtPlayer(true, false), "Queued player audio ignored the camera toggle");
    f.soundPlaying = true;
    turn.Update(f);
    f.soundQueued = false;
    for (int i = 0; i < 3600; ++i) {
        f.npcSpeaking = (i % 3) != 0;  // NPC starts early / topic changes mid-line
        turn.Update(f);
        Require(turn.ShouldLookAtPlayer(true), "NPC activity cut off a player line");
        Require(!turn.ShouldLookAtPlayer(true, false), "Player playback forced a disabled reverse shot");
    }
    f.npcSpeaking = true;
    f.soundPlaying = false;
    f.soundQueued = true;  // stale DBVO greeting/timer after natural completion
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true) && !turn.IsPlayerVoiceActive(),
            "completed sound stayed latched by a stale duration");
    for (int i = 0; i < 120; ++i) turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "completed sound rearmed without a new line");

    // The same prompt can be selected again: each playback has a new sound ID.
    f.soundID = 42;
    f.soundPlaying = true;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "repeat response did not hold its new playback");
    f.paused = true;
    f.soundPlaying = false;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "pausing audio was mistaken for line completion");
    Require(!turn.ShouldLookAtPlayer(true, false), "Disabling camera switching while paused kept the player shot");
    Require(!turn.ShouldLookAtPlayer(false), "reverse shot leaked into first person");
    f.paused = false;
    f.soundPlaying = true;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "returning from first person lost an active line");
    f.soundID = DBVODialogueTurn::kNoSound;
    f.soundValid = false;
    f.soundPlaying = false;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "skipping a line did not release the shot");

    // Missing/failed audio goes through to the NPC instead of getting stuck.
    f.soundID = 43;
    f.soundValid = true;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "queued line was not held");
    f.soundValid = false;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "failed playback held the NPC indefinitely");
    f.soundID = DBVODialogueTurn::kNoSound;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "unvoiced option blocked the NPC");
    f.npcSpeaking = false;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "next option list did not return to the player");

    // Finishing before a delayed response must not cause a one-frame shot flip.
    f.soundID = 44;
    f.soundValid = true;
    f.soundPlaying = true;
    turn.Update(f);
    f.soundPlaying = false;
    f.soundQueued = false;
    turn.Update(f);
    Require(turn.ShouldLookAtPlayer(true), "gap before NPC response flickered to the NPC");
    f.npcSpeaking = true;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "finished player audio blocked a delayed response");

    f.soundID = 45;
    f.soundPlaying = true;
    turn.Update(f);
    f.active = false;  // close, dialogue disabled, missing DBVO or missing speaker
    f.paused = true;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true) && !turn.IsPlayerVoiceActive(),
            "close/disable failed to clear voice ownership while paused");
    f = {};
    f.active = true;
    f.npcSpeaking = true;
    turn.Update(f);
    Require(!turn.ShouldLookAtPlayer(true), "voice state leaked into a new conversation");
    f.soundID = 46;
    f.soundValid = true;
    f.soundPlaying = true;
    turn.Update(f);
    turn.Reset();  // game load / new game / speaker handoff
    Require(!turn.ShouldLookAtPlayer(true), "load/reset retained the previous player shot");
    std::cout << "DBVO 2 dialogue-turn checks passed\n";
}
