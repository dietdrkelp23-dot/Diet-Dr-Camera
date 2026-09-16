#pragma once

namespace RE { class TESObjectREFR; }

namespace DietDrCamera::DialogueLookPicker
{
    // Called from HookManager::OnDialogueMenuOpenChange when the dialogue
    // menu opens. Resolves the active speaker, classifies them into a
    // DialogueCategory, picks the POV, and selects an active look from
    // the matching bucket. If the bucket's randomEnabled flag is set,
    // picks a random look. If the matched bucket is empty, walks the
    // fallback chain until it finds one with looks. Horseback takes priority
    // while mounted, including over SpecificNPC. Updates
    // SettingsManager's activeDialogueBucketIdx / activeDialogueLookIdx
    // live-state pointers. allowReroll=false suppresses the random pick (used by
    // RefreshForPovChange so a mid-dialogue POV switch keeps the look).
    void OnDialogueOpen(bool allowReroll = true);

    // Called when the dialogue menu closes. Clears the active selection.
    void OnDialogueClose();

    // Per-frame poll (HookManager). Re-picks the active look for the live POV if it
    // changed mid-dialogue, so manually switching POV uses that POV's presets.
    void RefreshForPovChange();

    // Cycles the active look forward (+1) or backward (-1) within the
    // currently active bucket. Wraps. No-op if no bucket is active or
    // the bucket has no looks. Called from the input sink (Phase 3).
    void CycleActiveLook(int direction);

    // Called when the player selects a dialogue option (clicks an answer).
    // No-op unless settings.dialogueRandomEnabled is true. Picks a new
    // random look from the active bucket, scoped to the current NPC if
    // the active look is NPC-bound. Driven by HookManager's per-frame
    // poll on MenuTopicManager::lastSelectedDialogue.
    void OnDialogueOptionSelected();

    // Called when the NPC starts delivering a NEW line (MenuTopicManager's
    // currentTopicInfo changing to a fresh non-null topic). No-op unless
    // settings.dialogueSwitchOnNpcLine is set. Same candidate scoping as
    // every other switch path.
    void OnNpcLineChanged();

    // Per-frame driver for the time-based behaviour: the auto-switch
    // interval. Self-gates on the Dialogue Menu being open; cheap no-op
    // otherwise. a_dt is real seconds.
    void Tick(float a_dt);

    // True while the NPC is delivering a line — MenuTopicManager's
    // currentTopicInfo, which the engine only populates while the speaker is
    // actually talking. The inverse (menu open, no current topic) is the
    // player's half of the conversation: the topic list is up and they are
    // choosing what to say.
    [[nodiscard]] bool IsNpcDeliveringLine();

    // True while the PLAYER's own voice line is playing. Only ever true with
    // a player-voice mod installed — vanilla never gives the player a voiced
    // line — and detected from the engine's own subtitle/voice state rather
    // than from any particular mod's signals, so it works with whichever one
    // is installed and costs nothing when none is.
    [[nodiscard]] bool IsPlayerVoiceLinePlaying();

    // True when the dialogue camera should be aimed at the PLAYER's face
    // rather than the speaker's: the "Look At The Player" toggle is on, we're
    // in third person, and it's the player's half of the conversation.
    [[nodiscard]] bool ShouldLookAtPlayer();

    // The active dialogue partner as a plain reference. NOT an Actor: the
    // Statue of Mara and other shrines are talking ACTIVATORS, so anything
    // that narrows to Actor loses them. Falls back to lastSpeaker for the
    // transient null-speaker windows mid-conversation, the same way the
    // face-lock's own gate does.
    [[nodiscard]] RE::TESObjectREFR* GetActiveSpeakerRef();
}
