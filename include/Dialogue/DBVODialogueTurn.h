#pragma once

#include <cstdint>

namespace DietDrCamera
{
    // SKSE packs the plugin's major version into the high byte. DBVO 1 uses a
    // different, script-driven playback path; this integration targets DBVO 2.
    [[nodiscard]] constexpr bool IsDBVO2Version(std::uint32_t version) noexcept
    {
        return (version >> 24) == 2;
    }

    class DBVODialogueTurn
    {
    public:
        static constexpr std::uint32_t kNoSound = 0xFFFFFFFFu;

        struct Frame
        {
            bool active = false;
            bool paused = false;
            bool npcSpeaking = false;
            std::uint32_t soundID = kNoSound;
            bool soundValid = false;
            bool soundPlaying = false;
            bool soundQueued = false;
        };

        void Reset() noexcept { *this = {}; }

        void Update(const Frame& frame) noexcept
        {
            if (!frame.active) {
                Reset();
                return;
            }
            // A paused audio device can report not-playing without completing
            // the line. Menu close/disable above still takes effect immediately.
            if (frame.paused) return;

            if (frame.soundID != m_soundID) {
                m_soundID = frame.soundID;
                m_heardSound = false;
            }
            if (frame.soundValid && frame.soundPlaying) {
                m_heardSound = true;
                m_playerVoice = true;
            } else {
                // Hold through asynchronous sound startup. Once playback was
                // observed, only the audio system can keep the hold alive:
                // DBVO's greeting flag / duration may outlive a skipped line.
                m_playerVoice = frame.soundValid && frame.soundQueued && !m_heardSound;
            }

            // DBVO may release the NPC response early (negative response delay).
            // Actual player audio always wins, even if the NPC topic changes.
            // Between speech and a delayed NPC response, keep the existing shot.
            m_lookAtPlayer = m_playerVoice || !frame.npcSpeaking;
        }

        [[nodiscard]] bool ShouldLookAtPlayer(bool thirdPerson, bool cameraSwitching = true) const noexcept
        {
            return cameraSwitching && thirdPerson && m_lookAtPlayer;
        }
        [[nodiscard]] bool IsPlayerVoiceActive() const noexcept { return m_playerVoice; }

    private:
        std::uint32_t m_soundID = kNoSound;
        bool m_heardSound = false;
        bool m_playerVoice = false;
        bool m_lookAtPlayer = false;
    };
}
