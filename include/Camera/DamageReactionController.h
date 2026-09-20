#pragma once

#include "Core/DamageReaction.h"
namespace RE { class Projectile; class TESObjectREFR; class hkbClipGenerator; class hkbCharacter; }

namespace DietDrCamera::DamageReactionController
{
    void Install();
    void Reset();
    DamageReaction::Rotation Update(bool cameraAvailable);
    void ObserveProjectile(RE::Projectile* projectile, RE::TESObjectREFR* target);
    void ObserveClip(RE::hkbClipGenerator* clip, const RE::hkbCharacter* character, bool active, bool refresh = false);
}
