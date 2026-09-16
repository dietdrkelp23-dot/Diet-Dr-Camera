#pragma once

namespace DietDrCamera
{
    // Detours `Projectile::UpdateFlightPath` (Address Library SE 42998 /
    // AE 44184) so player-fired arrows and bolts orient along the camera
    // aim direction instead of the player's body rotation. With our
    // off-shoulder / shoulder-swap camera the body is rotated relative
    // to the camera, so the engine's body-relative aim makes the arrow
    // miss the crosshair. SmoothCam ships the same detour; this is the
    // server-side half of "what you see is what you shoot."
    //
    // The detour runs only for the player and only in 3rd-person /
    // mount / dragon. NPC arrows pass through unmodified. Mid-flight
    // arrows (flag bit 31 set) preserve their stored angles, so the
    // patch only sets the initial direction once per shot.
    class ArrowPathDetour
    {
    public:
        // Idempotent. Allocates trampoline + writes branch on the engine
        // function. Call from kPostLoad.
        static void Install();

        // Main-thread camera snapshot. Walking PlayerCamera->cameraRoot
        // every detour invocation (which fires from the projectile
        // physics tick — possibly off the main thread) was crashing on
        // bow-fire transitions where the engine reseats the camera
        // tree. Instead the main thread publishes camera forward + up
        // each frame; the detour reads the snapshot atomically. Pass
        // a unit-length forward and up vector. fwdValid=false suppresses
        // override and falls back to the original engine flight path.
        // Publishes camera state PLUS the world-space target point
        // the camera-forward ray hits. The detour aims the arrow
        // toward the target point so the arrow lands where the
        // crosshair points (true aim) — without this, the bow's
        // lateral offset from the camera makes arrows miss to the
        // shoulder side. Pass the target as `(tx, ty, tz)`; if no
        // raycast hit, pass a far point along the forward ray.
        static void PublishCameraSnapshot(float fx, float fy, float fz,
                                          float ux, float uy, float uz,
                                          float px, float py, float pz,
                                          float tx, float ty, float tz);

        // Marks the camera snapshot stale so the per-tick detour falls
        // through to the engine's vanilla flight path. Call this from
        // any frame the publisher chooses NOT to publish a fresh
        // snapshot (target locked, archery tracing disabled, ineligible
        // camera state, etc.) — without it, the detour keeps reading
        // the last published snapshot indefinitely, which makes arrows
        // fly toward a stale world-space target point.
        static void InvalidateCameraSnapshot();

        // Lock-free read of the camera snapshot published by
        // PublishCameraSnapshot. Returns false if no snapshot has been
        // published yet, or if the snapshot was marked stale via
        // InvalidateCameraSnapshot, or if a torn read was detected.
        // Exposed for the sibling MissileProjectileDetour so spell
        // projectiles share the same camera-aim source as arrows.
        static bool ReadCameraSnapshot(RE::NiPoint3& outForward,
                                       RE::NiPoint3& outUp,
                                       RE::NiPoint3& outPos,
                                       RE::NiPoint3& outTarget);

        // Last per-shot velocity-scaling product (s2 * power *
        // speedMult) and ammo-projectile FormID the detour observed
        // at fire time. The prediction multiplies this by ammoSpeed
        // (BGSProjectile::data.speed) to get the actual launch
        // velocity:
        //     vel = ammoSpeed * velMult
        // which mirrors the engine's
        //     velScalar = s2 * baseSpeed * power * speedMult.
        // Caching the full product matters because `power` can be
        // 0.7-0.9 when the user releases at less-than-full draw, and
        // omitting it caused predicted arrows to fly ~30-40% faster
        // than the real ones. Returns false if no shot has been
        // observed yet for the current ammo type.
        static bool GetLastVelMult(float& outVelMult, std::uint32_t& outAmmoFormID);

        // Arrow fire event — increments on each player arrow spawn.
        // Publishes start (arrow spawn world position), target
        // (camera-aim raycast point), and approximate launch speed
        // so CrosshairManager can build a straight-line trail and
        // estimate travel time. Returns false if no fire event has
        // been recorded yet, OR if a torn read was detected.
        static bool GetLastFireEvent(std::uint32_t& outVersion,
                                     RE::NiPoint3& outStartWorld,
                                     RE::NiPoint3& outTargetWorld,
                                     float& outSpeed);
    };
}
