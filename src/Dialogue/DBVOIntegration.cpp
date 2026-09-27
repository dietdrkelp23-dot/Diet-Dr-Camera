#include "PCH.h"
#include "Dialogue/DBVOIntegration.h"

#include "Dialogue/DBVODialogueTurn.h"
#include "Dialogue/DialogueLookPicker.h"
#include "Settings/SettingsManager.h"

namespace DietDrCamera::DBVOIntegration
{
    namespace
    {
        bool s_available = false;
        RE::FormID s_speakerID = 0;
        DBVODialogueTurn s_turn;

        bool IsThirdPerson()
        {
            const auto* camera = RE::PlayerCamera::GetSingleton();
            return camera && camera->currentState &&
                (camera->currentState->id == RE::CameraState::kThirdPerson ||
                 camera->currentState->id == RE::CameraState::kMount);
        }
    }

    void Initialize()
    {
        const auto* plugin = SKSE::GetPluginInfo("DBVO");
        s_available = plugin && IsDBVO2Version(plugin->version);
        Reset();
        if (s_available) {
            spdlog::info("DBVO 2 detected: player camera switching is available in Dialogue settings");
        }
    }

    void Reset()
    {
        s_turn.Reset();
        s_speakerID = 0;
    }

    void Tick()
    {
        auto* ui = RE::UI::GetSingleton();
        const auto& settings = SettingsManager::GetSingleton();
        if (!s_available || !settings.dialogueEnabled || !settings.dialogueDBVOCameraSwitching ||
            !ui || !ui->IsMenuOpen("Dialogue Menu")) {
            Reset();
            return;
        }
        const auto* player = RE::PlayerCharacter::GetSingleton();
        const auto* speaker = DialogueLookPicker::GetActiveSpeakerRef();
        const auto* topics = RE::MenuTopicManager::GetSingleton();
        if (!player || !speaker || !topics || topics->forceGoodbye) {
            Reset();
            return;
        }
        if (s_speakerID != speaker->GetFormID()) {
            Reset();
            s_speakerID = speaker->GetFormID();
        }

        DBVODialogueTurn::Frame frame;
        frame.active = true;
        frame.paused = ui->GameIsPaused();
        frame.npcSpeaking = topics->currentTopicInfo != nullptr || topics->isGreetingPlayer;
        if (!frame.paused) {
            if (const auto* high = player->GetHighProcess()) {
                // DBVO 2.0.2.2 calls native Actor::SpeakSound, which stores the
                // dialogue handle in HighProcessData::soundHandles[0]. This is
                // also used for audio-only FUZ files. See DBVO-COMPATIBILITY.md.
                // VOICE_STATE describes shouts, not this dialogue audio; DBVO's
                // custom subtitles are optional and are not completion signals.
                auto sound = high->soundHandles[0];
                frame.soundID = sound.soundID;
                frame.soundValid = sound.soundID != RE::BSSoundHandle::kInvalidID && sound.IsValid();
                // The copied handle's assumed state can stay kPlaying after the
                // audio ends. Query the audio manager, never that cached value.
                sound.assumeSuccess = false;
                frame.soundPlaying = frame.soundValid && sound.IsPlaying();
                frame.soundQueued = high->greetingPlayer && high->soundDelay > 0.0f;
            }
        }
        s_turn.Update(frame);
    }

    bool ShouldLookAtPlayer()
    {
        const auto& settings = SettingsManager::GetSingleton();
        return s_available && settings.dialogueEnabled &&
            s_turn.ShouldLookAtPlayer(IsThirdPerson(), settings.dialogueDBVOCameraSwitching);
    }

    bool IsPlayerVoiceLinePlaying()
    {
        return s_available && SettingsManager::GetSingleton().dialogueDBVOCameraSwitching &&
            s_turn.IsPlayerVoiceActive();
    }
}
