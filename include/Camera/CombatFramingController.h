#pragma once
#include "Core/CombatFraming.h"

namespace DietDrCamera::CombatFramingController
{
    CombatFraming::Adjustment Update(float dt, bool allowed);
    void Reset();
}
