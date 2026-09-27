#pragma once
#include "Camera/GameplayCameraInput.h"

namespace RE { class TESCamera; class TESCameraState; class NiQuaternion; class InputEvent; }
namespace DietDrCamera::BleedoutCameraOverride
{
    void Install();
    void Tick(RE::TESCamera* camera);
    void BeginEvent(bool dead);
    void ObserveInput(const RE::InputEvent* event, GameplayCameraInput::Route route);
    void ApplyGameplayRotation(RE::TESCameraState* state, RE::NiQuaternion& rotation);
    void UpdateEffects(RE::TESCamera* camera);
    void RequestSlowMotionFade();
    bool IsSlowMotionActive();
    bool IsEventActive(bool dead);
    void Reset();
    bool IsAvailable();
}
