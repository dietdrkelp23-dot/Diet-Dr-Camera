#pragma once

#include "Core/HitShake.h"

namespace DietDrCamera::HitShakeController
{
    // Called on the camera path. Event callbacks only enqueue scalar values.
    HitShake::Mixer::Rotation Update(bool cameraAvailable);
    // Quick Tune follows the same actual attack and striking-hand overrides.
    HitShake::Tuning* CurrentTuning(bool firstPerson);
    void Reset();
}
