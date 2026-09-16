#include "PCH.h"
#include "Dialogue/DialogueLookPicker.h"

#include "Dialogue/SpeakerClassifier.h"
#include "Settings/SettingsManager.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include <RE/A/Actor.h>
#include <RE/M/MenuTopicManager.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/SubtitleManager.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>

namespace DietDrCamera::DialogueLookPicker
{
    namespace
    {
        // Returns the speaker as a REFR, not an Actor: talking activators
        // (Statue of Mara, the Aspect of Hircine) are real speakers, and the
        // As<Actor> cast used to drop them before classification ever ran
        // (user report 2026-08-20).
        RE::TESObjectREFR* GetCurrentSpeaker()
        {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            if (!mtm) return nullptr;
            if (auto p = mtm->speaker.get(); p) {
                return p.get();
            }
            if (auto p2 = mtm->lastSpeaker.get(); p2) {
                return p2.get();
            }
            return nullptr;
        }

        // The POV the active look was last resolved for, so a manual POV switch
        // mid-dialogue re-picks the look (RefreshForPovChange).
        DialoguePOV s_lastResolvedPov = DialoguePOV::ThirdPerson;

        DialoguePOV GetCurrentPOV()
        {
            // Dialogue uses whatever POV the player is currently in (the force-on-
            // entry toggles were removed).
            auto* pc = RE::PlayerCamera::GetSingleton();
            return (pc && pc->IsInFirstPerson()) ? DialoguePOV::FirstPerson
                                                 : DialoguePOV::ThirdPerson;
        }

        // True if `look` is a SpecificNPC entry bound to `speaker` (resolves
        // local FormID through TESDataHandler so load-order shifts don't
        // break the binding). Mirrors SpeakerClassifier's helper.
        bool LookMatchesSpeaker(RE::TESObjectREFR* speaker, const DialogueLook& look)
        {
            if (!speaker || look.targetFormID == 0) return false;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return false;
            const auto* plug = look.targetPluginName.empty()
                ? "Skyrim.esm"
                : look.targetPluginName.c_str();
            const auto resolved = dh->LookupFormID(look.targetFormID, plug);
            if (resolved == 0) return false;
            auto* base = speaker->GetBaseObject();
            if (!base) return false;
            return base->GetFormID() == resolved;
        }

        // Env axis only applies to Dragons / Creatures / SpecificNPC.
        // Outdoors, Indoors AND Horseback have env baked into the category
        // itself (they always read from env=Outdoor's slot) — Horseback is a
        // third value of the same toggle that picks Outdoors vs Indoors, so it
        // has no further split, exactly like the other two.
        DialogueEnv EnvForCategory(DialogueCategory cat)
        {
            const bool hasEnvAxis =
                cat == DialogueCategory::Dragons   ||
                cat == DialogueCategory::Creatures ||
                cat == DialogueCategory::SpecificNPC;
            if (!hasEnvAxis) return DialogueEnv::Outdoor;
            const auto& s = SettingsManager::GetSingleton();
            return s.indoorMode ? DialogueEnv::Indoor : DialogueEnv::Outdoor;
        }

        // Walk the fallback chain from the matched category outward to the
        // most general. Returns the bucket index that has at least one
        // look, or -1 if every category for this POV is empty.
        int FindNonEmptyBucketIdx(DialogueCategory matched, DialoguePOV pov)
        {
            const auto& s = SettingsManager::GetSingleton();
            // Horseback is only offered when the player is ACTUALLY mounted —
            // otherwise
            // this is a plain no-op and the chain reads exactly as before, so
            // presets with no Horseback looks are unaffected either way.
            const bool mountedNow = [] {
                auto* ply = RE::PlayerCharacter::GetSingleton();
                return ply && ply->IsOnMount();
            }();
            // Mounted framing takes priority even for a bound NPC. If this
            // POV has no Horseback looks, retain the normal speaker fallback.
            std::array<DialogueCategory, 6> chain{};
            std::size_t n = 0;
            if (mountedNow) chain[n++] = DialogueCategory::Horseback;
            chain[n++] = matched;
            chain[n++] = DialogueCategory::Dragons;
            chain[n++] = DialogueCategory::Creatures;
            chain[n++] = DialogueCategory::Indoors;
            if (n < chain.size()) chain[n++] = DialogueCategory::Outdoors;
            const auto chainEnd = chain.begin() + static_cast<std::ptrdiff_t>(n);
            for (auto it = chain.begin(); it != chainEnd; ++it) {
                const auto cat = *it;
                const auto env = EnvForCategory(cat);
                const auto idx = DialogueBucketIndex(cat, pov, env);
                if (!s.dialogueBuckets[idx].looks.empty()) {
                    return static_cast<int>(idx);
                }
                // For env-aware categories, fall back to the other env
                // slot before giving up on this category — a user with
                // presets only in Outdoor still gets a match when indoors.
                if (env != DialogueEnv::Outdoor) {
                    const auto altIdx = DialogueBucketIndex(cat, pov, DialogueEnv::Outdoor);
                    if (!s.dialogueBuckets[altIdx].looks.empty()) {
                        return static_cast<int>(altIdx);
                    }
                }
            }
            return -1;
        }

        // Candidate set for the ACTIVE look: bucket-wide, or scoped to the
        // one NPC when the active look is bound to a specific one. Shared by
        // every mid-conversation switch path so they all agree on what
        // "another preset" means.
        std::vector<int> CandidatesForActive(const DialogueBucket& a_bucket, int a_curIdx)
        {
            std::vector<int> candidates;
            const bool curValid = a_curIdx >= 0 && a_curIdx < static_cast<int>(a_bucket.looks.size());
            const bool scoped   = curValid && a_bucket.looks[a_curIdx].targetFormID != 0;
            if (scoped) {
                const auto& cur = a_bucket.looks[a_curIdx];
                for (int i = 0; i < static_cast<int>(a_bucket.looks.size()); ++i) {
                    const auto& l = a_bucket.looks[i];
                    if (l.targetFormID == cur.targetFormID &&
                        l.targetPluginName == cur.targetPluginName) {
                        candidates.push_back(i);
                    }
                }
            } else {
                candidates.reserve(a_bucket.looks.size());
                for (int i = 0; i < static_cast<int>(a_bucket.looks.size()); ++i)
                    candidates.push_back(i);
            }
            return candidates;
        }

        // When the ACTIVE look last changed, on the steady clock. Every path
        // that actually commits a change stamps it; the automatic paths
        // consult it through HoldWindowBlocks() below. Reset on dialogue
        // open so a stale stamp from the previous conversation can't
        // suppress the new one's first switch.
        std::chrono::steady_clock::time_point s_lastLookChangeTp{};

        void StampLookChange()
        {
            s_lastLookChangeTp = std::chrono::steady_clock::now();
        }

        // Minimum-hold window ("presets never change if the change would be
        // within a set amount of time"). SUPPRESSES the switch — it is not
        // deferred — and the manual cycle keys never consult this: a change
        // the player asked for by hand is always honoured.
        bool HoldWindowBlocks(const char* a_why)
        {
            const auto& s = SettingsManager::GetSingleton();
            if (s.dialogueMinShotSec <= 0.0f) return false;
            if (s_lastLookChangeTp.time_since_epoch().count() == 0) return false;
            const float held = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - s_lastLookChangeTp).count();
            if (held >= s.dialogueMinShotSec) return false;
            spdlog::debug("[DLG-PICK] {} suppressed — preset held only {:.2f}s of {:.2f}s",
                         a_why, held, s.dialogueMinShotSec);
            return true;
        }

        // Roll to a DIFFERENT candidate than the current one and commit it.
        // Returns true if the active look actually changed.
        bool SwitchToAnother(const char* a_why)
        {
            auto& s = SettingsManager::GetSingleton();
            if (s.activeDialogueBucketIdx < 0 ||
                s.activeDialogueBucketIdx >= static_cast<int>(s.dialogueBuckets.size())) return false;
            auto& bucket = s.dialogueBuckets[s.activeDialogueBucketIdx];
            if (bucket.looks.empty()) return false;
            // The minimum-hold window gates every AUTOMATIC switch — this is
            // the shared entry point for all of them (option pick, NPC line,
            // auto timer). It is also what collapses the option-pick →
            // NPC-line DOUBLE switch the 17:30 log showed 50-70ms apart.
            if (HoldWindowBlocks(a_why)) return false;

            const int curIdx = s.activeDialogueLookIdx;
            std::vector<int> candidates = CandidatesForActive(bucket, curIdx);
            if (candidates.size() < 2) {
                // Nothing to move to — unless the one candidate isn't the one
                // we're on, which happens when the active index has gone stale
                // (looks deleted from the bucket mid-conversation). Moving onto
                // the entry that actually exists is a real correction, and
                // without it this returns false forever and the conversation is
                // stuck on the legacy profile until it's reopened.
                if (candidates.size() == 1 && candidates[0] != curIdx) {
                    s.activeDialogueLookIdx = candidates[0];
                    bucket.activeIndex      = candidates[0];
                    StampLookChange();
                    spdlog::debug("[DLG-PICK] {} -> only preset {} ({})",
                                 a_why, candidates[0], bucket.looks[candidates[0]].name);
                    return true;
                }
                return false;
            }

            static thread_local std::mt19937 rng{ std::random_device{}() };
            std::uniform_int_distribution<int> dist(0, static_cast<int>(candidates.size()) - 1);
            int pick = dist(rng);
            if (candidates[pick] == curIdx) {
                pick = (pick + 1) % static_cast<int>(candidates.size());
            }
            const int next = candidates[pick];
            if (next == curIdx) return false;
            s.activeDialogueLookIdx = next;
            bucket.activeIndex      = next;
            StampLookChange();
            spdlog::debug("[DLG-PICK] {} -> look {} ({}) of {} candidate(s)",
                         a_why, next, bucket.looks[next].name, candidates.size());
            return true;
        }

        // Auto-switch interval state. The interval is re-rolled after every
        // fire so a conversation doesn't settle into a metronome.
        float s_autoSwitchTimer    = 0.0f;
        float s_autoSwitchInterval = 0.0f;

        float RollAutoSwitchInterval()
        {
            const auto& s = SettingsManager::GetSingleton();
            const float hi = std::max(0.0f, s.dialogueAutoSwitchMax);
            if (hi <= 0.0f) return 0.0f;
            const float lo = std::clamp(s.dialogueAutoSwitchMin, 0.0f, hi);
            if (hi - lo < 0.01f) return hi;
            static thread_local std::mt19937 rng{ std::random_device{}() };
            std::uniform_real_distribution<float> dist(lo, hi);
            return dist(rng);
        }
    }

    void OnDialogueOpen(bool allowReroll)
    {
        auto& s = SettingsManager::GetSingleton();

        auto* speaker = GetCurrentSpeaker();
        const auto category = SpeakerClassifier::Classify(speaker);
        const auto pov      = GetCurrentPOV();
        // Remember the POV this resolve is for, so RefreshForPovChange can detect a
        // mid-dialogue POV switch and re-pick the matching bucket.
        s_lastResolvedPov = pov;

        const int bucketIdx = FindNonEmptyBucketIdx(category, pov);
        if (bucketIdx < 0) {
            // No bucket has any presets for this POV. Leave active state
            // cleared; CameraController and face-lock fall back to legacy
            // dialogueProfile / dialogueFirstPersonProfile fields.
            s.activeDialogueBucketIdx = -1;
            s.activeDialogueLookIdx   = -1;
            spdlog::debug("[DLG-PICK] no looks in any bucket for pov={}, falling back to legacy profile",
                         static_cast<int>(pov));
            return;
        }

        auto& bucket = s.dialogueBuckets[bucketIdx];

        // For SpecificNPC, the bucket can hold presets for many NPCs.
        // Filter the candidate index set to only the looks that match the
        // current speaker, so cycling and random-pick stay scoped to that
        // NPC. Other categories use every look in the bucket.
        std::vector<int> candidates;
        const auto selectedCategory = static_cast<DialogueCategory>(
            static_cast<std::size_t>(bucketIdx) /
            (static_cast<std::size_t>(DialoguePOV::Count) *
             static_cast<std::size_t>(DialogueEnv::Count)));
        const bool specificNpc = selectedCategory == DialogueCategory::SpecificNPC;
        if (specificNpc) {
            for (int i = 0; i < static_cast<int>(bucket.looks.size()); ++i) {
                if (LookMatchesSpeaker(speaker, bucket.looks[i])) {
                    candidates.push_back(i);
                }
            }
        } else {
            candidates.reserve(bucket.looks.size());
            for (int i = 0; i < static_cast<int>(bucket.looks.size()); ++i) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            // Defensive: SpeakerClassifier promised at least one match, but
            // if the FormID resolved away (plugin missing), fall back to
            // bucket-wide so we still apply *something* sensible.
            candidates.reserve(bucket.looks.size());
            for (int i = 0; i < static_cast<int>(bucket.looks.size()); ++i)
                candidates.push_back(i);
        }

        int lookIdx = bucket.activeIndex;
        const bool currentInCandidates =
            std::find(candidates.begin(), candidates.end(), lookIdx) != candidates.end();

        // The entry-specific flag was folded into the master toggle
        // (2026-08-14): dialogueRandomEveryTime is the one random switch.
        const bool wantRandom = bucket.randomEnabled
            || s.dialogueRandomEveryTime;
        // allowReroll is false for a mid-dialogue POV-switch re-pick: keep the
        // current look, just move to the new POV's bucket (no surprise re-roll).
        if (allowReroll && wantRandom && candidates.size() > 1) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(0, static_cast<int>(candidates.size()) - 1);
            int pick = dist(rng);
            if (currentInCandidates && candidates[pick] == lookIdx) {
                pick = (pick + 1) % static_cast<int>(candidates.size());
            }
            lookIdx = candidates[pick];
        } else if (!currentInCandidates) {
            lookIdx = candidates.front();
        }
        bucket.activeIndex = lookIdx;

        s.activeDialogueBucketIdx = bucketIdx;
        s.activeDialogueLookIdx   = lookIdx;
        // Arm the auto-switch interval for this conversation. Re-armed on
        // every OnDialogueOpen, including the no-reroll POV-switch re-pick —
        // that's a fresh framing either way, so restarting the clock is what
        // keeps the rotation feeling paced rather than arbitrary.
        s_autoSwitchInterval = RollAutoSwitchInterval();
        s_autoSwitchTimer    = 0.0f;
        // The open pick starts the minimum-hold window; a stale stamp from
        // the previous conversation must not suppress this one's switches.
        StampLookChange();
        spdlog::debug("[DLG-PICK] category={} pov={} bucket={} look={} ({}) candidates={}",
                     static_cast<int>(category), static_cast<int>(pov),
                     bucketIdx, lookIdx,
                     bucket.looks[lookIdx].name,
                     candidates.size());
    }

    void OnDialogueClose()
    {
        auto& s = SettingsManager::GetSingleton();
        s.activeDialogueBucketIdx = -1;
        s.activeDialogueLookIdx   = -1;
        s_lastResolvedPov  = DialoguePOV::ThirdPerson;
        s_autoSwitchTimer    = 0.0f;
        s_autoSwitchInterval = 0.0f;
    }

    // Per-frame (called from HookManager). If the camera's POV changed mid-dialogue
    // — e.g. the user manually toggled F / R3 while "Force First/Third Person On
    // Entry" had put them in the other POV — re-pick the look for the NEW POV's
    // bucket, so third person uses third-person presets and vice versa. Self-gates
    // on the Dialogue Menu being open; cheap no-op otherwise.
    void RefreshForPovChange()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen("Dialogue Menu")) return;
        if (GetCurrentPOV() == s_lastResolvedPov) return;
        OnDialogueOpen(/*allowReroll=*/false);
    }

    void OnDialogueOptionSelected()
    {
        auto& s = SettingsManager::GetSingleton();
        // Gate on the per-category random-on-option flag (derived from
        // the active bucket index — 2 POVs per category, so cat = idx/2).
        // Per-bucket randomEnabled is OR'd into OnDialogueOpen but
        // doesn't drive the per-option re-roll — only the per-category
        // setting does.
        if (s.activeDialogueBucketIdx < 0 ||
            s.activeDialogueBucketIdx >= static_cast<int>(s.dialogueBuckets.size())) {
            return;
        }
        // Bucket index layout: ((cat * POVCount) + pov) * EnvCount + env
        const std::size_t catIdx = static_cast<std::size_t>(s.activeDialogueBucketIdx) /
                                   (static_cast<std::size_t>(DialoguePOV::Count) *
                                    static_cast<std::size_t>(DialogueEnv::Count));
        if (catIdx >= s.dialogueRandomOnOption.size()) return;
        if (!s.dialogueRandomOnOption[catIdx]) return;
        // Candidate scoping and the "not the one we're already on" roll are
        // shared with every other switch path.
        (void)SwitchToAnother("random on option select");
        // Every switch restarts the auto-switch clock: the point of the timer
        // is "how long a framing is allowed to sit", and one that has just
        // changed for another reason has not been sitting.
        s_autoSwitchInterval = RollAutoSwitchInterval();
        s_autoSwitchTimer    = 0.0f;
    }

    void OnNpcLineChanged()
    {
        auto& s = SettingsManager::GetSingleton();
        if (!s.dialogueSwitchOnNpcLine) return;
        if (s.activeDialogueBucketIdx < 0 ||
            s.activeDialogueBucketIdx >= static_cast<int>(s.dialogueBuckets.size())) return;
        (void)SwitchToAnother("npc line");
        s_autoSwitchInterval = RollAutoSwitchInterval();
        s_autoSwitchTimer    = 0.0f;
    }

    bool IsNpcDeliveringLine()
    {
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) return false;
        // currentTopicInfo is documented by the engine layout as "only valid
        // when the NPC is talking", and it clears the moment the line ends —
        // which is exactly the window we want. lastTopicInfo deliberately is
        // NOT consulted: it persists after the line finishes, which would
        // make the NPC look like they never stop speaking.
        return mtm->currentTopicInfo != nullptr;
    }

    bool IsPlayerVoiceLinePlaying()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Path 1 — the engine's own voice state on the player. A player-voice
        // mod that speaks through Say() drives this, and it's the most direct
        // statement the engine makes about "this actor is currently
        // vocalising".
        if (auto* high = player->GetHighProcess()) {
            const auto vs = high->voiceState.get();
            if (vs == RE::VOICE_STATE::kStart || vs == RE::VOICE_STATE::kContinue)
                return true;
        }

        // Path 2 — the subtitle currently on screen belongs to the player.
        // Catches mods that route their line through the subtitle system on a
        // different path. Costs one pointer compare.
        if (auto* subs = RE::SubtitleManager::GetSingleton()) {
            if (auto sp = subs->currentSpeaker.get();
                sp && sp.get() == static_cast<RE::TESObjectREFR*>(player)) {
                return true;
            }
        }
        return false;
    }

    RE::TESObjectREFR* GetActiveSpeakerRef()
    {
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) return nullptr;
        if (auto p = mtm->speaker.get(); p) return p.get();
        if (!mtm->forceGoodbye) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen("Dialogue Menu")) {
                if (auto p2 = mtm->lastSpeaker.get(); p2) return p2.get();
            }
        }
        return nullptr;
    }

    bool ShouldLookAtPlayer()
    {
        // Feature REMOVED at user request 2026-08-15 ("remove the switch to
        // the player when choosing a dialogue option toggle as well as the
        // whole feature"). The API stays so consumers need no change; it
        // simply never fires. The old dialogue_lock_on_player_face TOML key
        // is ignored on load.
        return false;
    }

    void Tick(float a_dt)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen("Dialogue Menu")) return;
        auto& s = SettingsManager::GetSingleton();
        if (s.activeDialogueBucketIdx < 0) return;
        if (a_dt <= 0.0f || a_dt > 0.5f) a_dt = 1.0f / 60.0f;

        // --- Automatic preset rotation ---------------------------------
        if (s_autoSwitchInterval <= 0.0f) {
            // Off, or the sliders were only just turned up — pick up the
            // change without needing a new conversation.
            s_autoSwitchInterval = RollAutoSwitchInterval();
            s_autoSwitchTimer    = 0.0f;
            return;
        }
        s_autoSwitchTimer += a_dt;
        if (s_autoSwitchTimer < s_autoSwitchInterval) return;
        s_autoSwitchTimer    = 0.0f;
        s_autoSwitchInterval = RollAutoSwitchInterval();
        (void)SwitchToAnother("auto-switch timer");
    }

    void CycleActiveLook(int direction)
    {
        auto& s = SettingsManager::GetSingleton();
        if (s.activeDialogueBucketIdx < 0 ||
            s.activeDialogueBucketIdx >= static_cast<int>(s.dialogueBuckets.size())) {
            return;
        }
        auto& bucket = s.dialogueBuckets[s.activeDialogueBucketIdx];
        if (bucket.looks.empty()) return;
        if (s.activeDialogueLookIdx < 0 ||
            s.activeDialogueLookIdx >= static_cast<int>(bucket.looks.size())) return;

        // Scope the cycle to the current NPC's presets when the active
        // look is NPC-bound. Other categories cycle bucket-wide.
        const auto& cur = bucket.looks[s.activeDialogueLookIdx];
        const bool scoped = (cur.targetFormID != 0);

        std::vector<int> indices;
        if (scoped) {
            for (int i = 0; i < static_cast<int>(bucket.looks.size()); ++i) {
                const auto& l = bucket.looks[i];
                if (l.targetFormID == cur.targetFormID &&
                    l.targetPluginName == cur.targetPluginName) {
                    indices.push_back(i);
                }
            }
        } else {
            indices.reserve(bucket.looks.size());
            for (int i = 0; i < static_cast<int>(bucket.looks.size()); ++i)
                indices.push_back(i);
        }
        if (indices.size() < 2) return;

        const auto it = std::find(indices.begin(), indices.end(), s.activeDialogueLookIdx);
        const int curPos = (it == indices.end()) ? 0 : static_cast<int>(it - indices.begin());
        const int n = static_cast<int>(indices.size());
        int nextPos = (curPos + direction) % n;
        if (nextPos < 0) nextPos += n;

        const int next = indices[nextPos];
        s.activeDialogueLookIdx = next;
        bucket.activeIndex      = next;
        // Every switch restarts the auto-switch clock (same rule as the
        // option-select and NPC-line paths): a framing the user just picked
        // by hand has not been sitting, so the idle timer must not fire
        // right on top of the manual choice.
        s_autoSwitchInterval = RollAutoSwitchInterval();
        s_autoSwitchTimer    = 0.0f;
        // Manual cycling is EXEMPT from the minimum-hold window but still
        // restarts it — the player's own pick deserves its full hold.
        StampLookChange();
        spdlog::debug("[DLG-PICK] cycle dir={} → look {} ({}) within {} candidate(s)",
                     direction, next, bucket.looks[next].name, indices.size());
    }
}
