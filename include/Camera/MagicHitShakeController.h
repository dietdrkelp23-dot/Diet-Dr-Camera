#pragma once

#include "Core/HitShake.h"

namespace RE { class PlayerCharacter; class PlayerCamera; }

namespace DietDrCamera::MagicHitShakeController
{
    void Install();
    void Reset();
    bool Available();
    HitShake::Tuning* CurrentTuning(bool firstPerson);
    void Update(RE::PlayerCharacter* player, RE::PlayerCamera* camera, bool firstPerson,
                double now, HitShake::Mixer& mixer);
}
