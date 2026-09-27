#pragma once

namespace DietDrCamera::DBVOIntegration
{
    // Called after SKSE has finished loading every plugin. Optional: no DBVO 2
    // means ordinary DDC dialogue, with no new dependency or preset setting.
    void Initialize();
    void Reset();
    void Tick();
    [[nodiscard]] bool ShouldLookAtPlayer();
    [[nodiscard]] bool IsPlayerVoiceLinePlaying();
}
