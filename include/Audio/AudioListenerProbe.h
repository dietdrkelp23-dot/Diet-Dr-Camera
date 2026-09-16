#pragma once

namespace DietDrCamera::AudioListenerProbe
{
    // READ-ONLY reconnaissance for the "audio goes quiet at far zoom" problem.
    //
    // Skyrim puts its 3D audio listener at the CAMERA, not at the player, so
    // attenuation is computed from the camera. At DDC's far framings the camera
    // sits ~1000 game units out — about 1.5x past anything vanilla can reach —
    // which is far enough that sounds fall to roughly a tenth of their near
    // amplitude AND drop out of the propagation range their Sound Output Model
    // declares. Past that range they don't fade, they stop, which is why it
    // reads as "the audio isn't working" rather than "the audio is quiet".
    //
    // Fixing it means writing the listener position, and nothing public defines
    // where that lives: CommonLibSSE-NG has no BSAudioListener class at all,
    // only a vtable and an RTTI id, and BSAudioManager is a 0x190-byte block of
    // unknown fields. Guessing an offset and writing to it is how you get a
    // crash dump (see the ForEachReferenceInRange post-mortem), so this pass
    // only LOOKS. It identifies the listener by vtable IDENTITY — never by
    // guessing a layout — then reports which floats inside it track the camera
    // across several samples taken at genuinely different camera positions, so
    // a field that merely holds a constant can't masquerade as a match.
    //
    // One play session with some camera movement produces a complete answer in
    // the log under [AUDIO]. Bounded: it stops sampling on its own and costs
    // nothing afterwards.
    //
    // Safe to call every frame; it rate-limits and self-terminates.
    void Update();
}
