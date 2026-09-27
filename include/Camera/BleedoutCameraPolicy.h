#pragma once

namespace DietDrCamera::BleedoutCameraPolicy
{
    enum class Action { PassThrough, KeepCurrent, ReturnToGameplay };

    // The player singleton exists before its base actor is loaded. Actor-value
    // queries at that point dereference an absent TESNPC during startup.
    template <class Player, class ActorValue>
    bool RealDeath(Player* player, ActorValue health)
    {
        if (!player || !player->GetActorBase()) return false;
        if (player->IsDead()) return true;
        const auto* values = player->AsActorValueOwner();
        return values && values->GetActorValue(health) <= 0.0f;
    }

    constexpr bool Disabled(bool death, bool disableDeath, bool disableRagdoll, bool suspended)
    {
        return !suspended && (death ? disableDeath : disableRagdoll);
    }

    constexpr Action Resolve(bool playerCamera, bool playerAvailable, bool requestedBleedout,
        bool disabled, bool currentGameplay, bool menuOwnsCamera)
    {
        if (!playerCamera || !playerAvailable || !requestedBleedout || !disabled)
            return Action::PassThrough;
        return currentGameplay || menuOwnsCamera ? Action::KeepCurrent : Action::ReturnToGameplay;
    }

    // Tracks only a camera request that we actually suppressed. Recovery clears
    // it; disabling the option during that same event restores native handling.
    struct Session
    {
        bool active = false;
        void Blocked() { active = true; }
        void Reset() { active = false; }
        bool ResumeNative(bool dead, bool knockedDown, bool disabled)
        {
            if (!dead && !knockedDown) { Reset(); return false; }
            if (active && !disabled) return true;
            return false;
        }
    };
}
