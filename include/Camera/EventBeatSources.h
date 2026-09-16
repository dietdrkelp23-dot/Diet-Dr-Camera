#pragma once

// Queue for the table-driven event beats.
//
// This used to be a signal-intake hub owning furniture / activate / combat /
// souls-trapped / ward-hit sinks, an imagespace-modifier vfunc hook and a
// dragon-soul ActorValue poll. Every beat those fed (coffin ambushes, priest
// and mound risings, lurkers, soul traps, ward breaks, dragon-soul absorption,
// standing stones, word walls, scrolls, black books, explosions, roars) has
// since been removed, and the one beat left — werewolf Feeding — is armed by
// CameraNoiseController's own magic-effect sink from the kWerewolfFeed
// archetype. So all that remains is the queue those producers wrote into.
//
// Threading: the producer may fire off the main camera path, so the queue is a
// small mutex-guarded ring; Drain() runs on the camera path.

#include "Camera/EventBeatDefs.h"

namespace DietDrCamera::EventBeatSources
{
    struct BeatEvent
    {
        BeatId       id;
        RE::NiPoint3 pos{};
        bool         hasPos = false;
        float        scale  = 1.0f;  // per-arm amp scale
        bool         playerOwned = false;
    };

    // Arms/disarms the queue. Called per frame with "is any beat enabled".
    void SetActive(bool a_active);

    // Queue a beat. a_pos may be null (player-centric beat).
    void Enqueue(BeatId a_id, const RE::NiPoint3* a_pos, float a_scale = 1.0f,
                 bool a_playerOwned = false);

    // Drain up to a_max pending events into a_out; returns the count.
    std::size_t Drain(BeatEvent* a_out, std::size_t a_max);
}
