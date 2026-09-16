// Replace only engine side effects. The production settings codec, validation,
// reset, profile registry and filesystem preset manager run unchanged in tests.
#include "PCH.h"
#include "Settings/SettingsManager.h"
#include "Locations/LocationDetector.h"
#include "Camera/AnimationCameraController.h"
#include "Camera/CameraController.h"
#include "Camera/StateResolver.h"
#include "Camera/CameraEffectClock.h"
#include "Settings/EquippedItemBinding.h"

namespace DietDrCamera
{
    void SettingsManager::ApplyEngineSettings() {}
    LocationDetector& LocationDetector::GetSingleton() { static LocationDetector instance; return instance; }
    AnimationCameraController& AnimationCameraController::GetSingleton() { static AnimationCameraController instance; return instance; }
    void AnimationCameraController::RebuildMatchIndex() {}
    CameraController& CameraController::GetSingleton() { static CameraController instance; return instance; }
    void CameraController::ResetTransitionState() {}
    void CameraController::InvalidateProfileReferences() {}
    StateResolver& StateResolver::GetSingleton() { throw std::runtime_error("Unexpected engine state query in preset codec"); }
    std::chrono::steady_clock::time_point CameraEffectClock::Now() { throw std::runtime_error("Unexpected engine clock query in preset codec"); }
    ItemBindings::EquippedItem ItemBindings::DescribeEquipped(RE::Actor*, bool, bool) { throw std::runtime_error("Unexpected equipment query in preset codec"); }
}
