#pragma once

#include <RE/Skyrim.h>

namespace DietDrCamera::VanityCamera
{
    // Runs before the engine camera update, including while framework UI is open.
    void Tick();
    // Called when the shared third-person springs finish the vanity exit.
    void FinishReturn();
    void Reset();
    void OnInput(RE::InputEvent* events);
    [[nodiscard]] bool IsActive();
}
