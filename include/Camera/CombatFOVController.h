#pragma once

namespace DietDrCamera::CombatFOVController
{
    void Reset();
    // The caller supplies the current profile FOV, before additive effects.
    float Apply(float normalFOV, bool gameplayCamera = true);
}
