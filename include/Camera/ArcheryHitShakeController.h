#pragma once

#include "Core/HitShake.h"

namespace RE { class PlayerCharacter; class PlayerCamera; }

namespace DietDrCamera::ArcheryHitShakeController
{
    void Install();
    void Reset();
    HitShake::Tuning* CurrentTuning(bool firstPerson);
    void Update(RE::PlayerCharacter* player, RE::PlayerCamera* camera, bool firstPerson,
                double now, HitShake::Mixer& mixer);
}
