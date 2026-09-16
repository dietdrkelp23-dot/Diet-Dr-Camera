#pragma once

namespace DietDrCamera::Defaults
{
    struct SliderRange
    {
        float min;
        float max;
        float step;
        float vanilla;
    };

    constexpr SliderRange SideOffset  { -150.0f,  150.0f, 1.0f,   30.0f };
    // Height extends both ways and Zoom extends its max for more dramatic
    // framing. Same on-screen slider width — the wider range just maps more
    // value across the track (use the right-click fine-drag for precision).
    constexpr SliderRange Height      { -200.0f,  200.0f, 1.0f,  -10.0f };
    constexpr SliderRange Zoom        {   0.0f,   200.0f, 0.1f,    0.0f };
    constexpr SliderRange FOV         {   50.0f,  140.0f, 1.0f,   90.0f };
    constexpr SliderRange Rotation    { -180.0f,  180.0f, 1.0f,    0.0f };
    constexpr SliderRange PitchOffset { -100.0f,  100.0f, 0.1f,    0.0f };

    constexpr SliderRange FirstPersonFOV { 40.0f, 140.0f, 1.0f, 80.0f };

    // Dragon riding needs much wider tolerances — the dragon's body is huge
    // and the player rides far above terrain, so reasonable framings sit
    // well outside the human-scale ranges. Rotation reuses the normal range
    // (yaw orbit is a percentage of a circle either way).
    constexpr SliderRange DragonSideOffset  { -600.0f,  600.0f, 2.0f,   30.0f };
    constexpr SliderRange DragonHeight      { -400.0f,  400.0f, 2.0f,  -10.0f };
    constexpr SliderRange DragonZoom        { -200.0f,  400.0f, 0.5f,    0.0f };
    constexpr SliderRange DragonFOV         {   20.0f,  150.0f, 1.0f,   90.0f };
    constexpr SliderRange DragonPitchOffset { -400.0f,  400.0f, 0.5f,    0.0f };

    // Horseback needs the Zoom floor opened, but only just: the orbit centres
    // on the MOUNT, so vanilla-closest sits a little further back than it does
    // on foot and a floor of 0 left no room to close that gap. A horse is NOT
    // a dragon — the user sized this directly ("at most, it just needed to be
    // able to go to like -20 zoom"), so the range stays human-scale in every
    // other respect: same max and same 0.1 step as Zoom, so mount rows keep
    // the normal tuning precision. Side/height/FOV are untouched — the rider
    // is still a human-sized subject.
    constexpr SliderRange MountZoom   {  -25.0f,  200.0f, 0.1f,    0.0f };

    // --- Shared camera-tuning constants -------------------------------
    // Behavioural constants (not slider ranges) that are consumed in one
    // place and inverted/reused in another. Single-sourced here so a
    // consumer and its inverse can never silently drift apart — exactly
    // what happened when the SmoothCam importer hand-copied these.

    // Minimum per-frame position-follow rate at max Looseness. Consumed as
    // baseRate = pow(LooseRateMin, sqrt(looseness)) in HookManager; the
    // SmoothCam importer inverts the same formula to recover Looseness.
    constexpr float LooseRateMin = 0.01f;

    // 1 zoom-slider unit in engine game units. sliderZoom * 0.01 spans
    // 443.6 game units per engine targetZoomOffset unit => 4.436. Used by
    // CameraController (slider->game) and the importer (game->slider).
    constexpr float GameUnitsPerSliderZoom = 4.436f;

    // Camera distance from the head (game units) at zoom slider 0 — the
    // engine's pinned closest (zoomBase -0.2). Absolute DDC distance is
    // VanillaCamDistance + slider * GameUnitsPerSliderZoom. Used by
    // CameraController and the SmoothCam importer's distance conversion.
    constexpr float VanillaCamDistance = 120.0f;
}
