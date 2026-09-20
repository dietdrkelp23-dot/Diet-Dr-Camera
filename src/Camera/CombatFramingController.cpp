#include "PCH.h"
#include "Camera/CombatFramingController.h"
#include "Camera/StateResolver.h"
#include "Camera/WorldCameraRay.h"
#include "Settings/SettingsManager.h"
#include <array>

namespace DietDrCamera::CombatFramingController
{
    namespace {
        CombatFraming::Envelope envelope;
        float scanDelay = 0, pressure = 0;
    }
    void Reset() { envelope = {}; scanDelay = pressure = 0; }
    CombatFraming::Adjustment Update(float dt, bool allowed)
    {
        // Physical glide state owns the exclusion, including when the user
        // has disabled/tuned away DDC's dedicated paraglider camera profile.
        // Clear the old encounter too so it cannot return on landing.
        if (StateResolver::GetSingleton().IsParagliding()) { Reset(); return {}; }
        const auto tuning = CombatFraming::Sanitize(SettingsManager::GetSingleton().combatFraming);
        if (tuning.zoomIntensity == 0 && tuning.fovIntensity == 0) { Reset(); return {}; }
        auto* player = RE::PlayerCharacter::GetSingleton();
        allowed = allowed && player && player->IsInCombat() && !player->IsDead() && !player->IsInKillMove();
        if (!allowed) { pressure = 0; scanDelay = 0; }
        else if ((scanDelay -= dt) <= 0) {
            scanDelay = .15f;
            std::array<float, CombatFraming::kMaxSubjects> distances{};
            std::size_t count = 0;
            if (auto* processes = RE::ProcessLists::GetSingleton()) {
                for (const auto& handle : processes->highActorHandles) {
                    const auto actor = handle.get();
                    if (!actor || actor.get() == player || !actor->Is3DLoaded() || actor->IsDead() ||
                        !actor->IsInCombat() || actor->IsPlayerTeammate()) continue;
                    const float distance = actor->GetPosition().GetDistance(player->GetPosition());
                    if (distance >= 1800 || !actor->IsHostileToActor(player)) continue;
                    const auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
                    if (!target || (target.get() != player && !target->IsPlayerTeammate())) continue;
                    const auto hit = TraceCameraRay(player,player->GetPosition()+RE::NiPoint3{0,0,60},
                        actor->GetPosition()+RE::NiPoint3{0,0,60},true);
                    if (!hit.valid || (hit.hit && hit.fraction < .98f)) continue;
                    distances[count++] = distance;
                    if (count == distances.size()) break;
                }
            }
            pressure = CombatFraming::Pressure({distances.data(), count});
        }
        envelope.Step(pressure, dt);
        return CombatFraming::Evaluate(tuning,envelope.value);
    }
}
