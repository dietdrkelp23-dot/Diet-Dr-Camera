#pragma once

// Static definitions for the table-driven one-shot event beats (Cinematic
// Effects). One row here = one tunable entry in the menu, one BeatTuning in
// SettingsManager, one EventBeat slot in the noise controller's loudest-wins
// pool, and one arming path in EventBeatSources. Standalone header: no RE /
// settings dependencies, so every consumer can include it.

#include <array>
#include <cstdint>

namespace DietDrCamera
{
    enum class BeatId : std::uint8_t
    {
        WerewolfFeed = 0,
        kCount
    };
    inline constexpr std::size_t kEventBeatCount = static_cast<std::size_t>(BeatId::kCount);

    // Same field order as SettingsManager::CinematicShakeChar
    // (rotShake, posShake, driftJitter, roughness, fadeDuration) — kept as a
    // plain POD here so this header stays dependency-free. All-zero characters
    // render nothing on an additive layer, so every default is non-zero.
    struct EventBeatCharDef
    {
        float rotShake;
        float posShake;
        float driftJitter;
        float roughness;
        float fadeDuration;
    };

    struct EventBeatDef
    {
        const char*      tomlKey;     // [cinematic.<tomlKey>]
        const char*      label;       // menu row text
        const char*      group;       // menu group header (consecutive rows sharing it merge)
        float            hold;        // envelope hold seconds (clamped 0..3 downstream)
        bool             positional;  // has a Range slider + distance falloff at the latched pos
        // Has a Direction slider. Only meaningful for a beat that happens
        // somewhere ELSE — the bias points the shake at the latched world
        // position, so a beat centred on the player has nothing to point at
        // and the slider is dead controls.
        bool             directional;
        float            defSpeed;
        float            defRange;    // only meaningful when positional
        EventBeatCharDef defChar;
    };

    inline constexpr std::array<EventBeatDef, kEventBeatCount> kEventBeatDefs = { {
        // Renders under the bespoke "Werewolf" group in the menu (row moved
        // beside Transformation/Revert); the TOML key is unchanged.
        // Not positional and not directional: you are the one feeding, so the
        // event is always at the camera.
        { "werewolf_feeding",       "Feeding",                  "Werewolf",   1.50f, false, false, 0.90f,    0.0f, { 1.4f, 0.8f, 0.45f, 0.55f, 0.90f } },
    } };

    [[nodiscard]] inline constexpr const EventBeatDef& BeatDefOf(BeatId a_id)
    {
        return kEventBeatDefs[static_cast<std::size_t>(a_id)];
    }
}
