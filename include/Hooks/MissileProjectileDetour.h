#pragma once

namespace DietDrCamera
{
    // Sister hook to ArrowPathDetour for spell-launched projectiles.
    // Patches VTABLE_MissileProjectile[0] slot 0xAB (Projectile::UpdateImpl).
    // FlameProjectile / BeamProjectile / ConeProjectile / GrenadeProjectile
    // each have their own vtables and are NOT touched, so streaming spells
    // (Flames, Sparks, Frostbite) are auto-excluded.
    //
    // On the first tick after spawn, gates on:
    //   1. shooter == player
    //   2. camera state is third-person / mount / dragon
    //   3. runtime.spell exists and has at least one effect with the
    //      Hostile flag set (Soul Trap and utility spells skip this).
    //   4. SettingsManager::spellTracingEnabled is true.
    //
    // When all gates pass, rewrites the projectile's data.angle.x/z AND
    // linearVelocity to point at the camera-aim target published by
    // ArrowPathDetour::PublishCameraSnapshot. This is the same true-aim
    // fix the arrow detour uses, applied to spell projectiles.
    class MissileProjectileDetour
    {
    public:
        // Idempotent. Patches the MissileProjectile vtable. Call from
        // kPostLoad after ArrowPathDetour::Install (so SmoothCam can
        // still claim the camera-snapshot publisher slot).
        static void Install();

        // Last per-shot velocity-scaling factor observed for spell
        // projectiles. For arrows this was s2 * speedMult; for spells
        // there's no draw strength, so this is just runtime.speedMult
        // (or 1.0 if no shot observed yet). Keyed by the spell's
        // projectile FormID so the predictor can match per-spell.
        // Returns false if no shot has been observed yet for this
        // projectile FormID.
        static bool GetLastVelMult(float& outVelMult, std::uint32_t& outProjFormID);
        static float GetSpeedMultiplier(std::uint32_t projectileFormID);
        static std::uint32_t GetCollisionFilter(const RE::BGSProjectile* projectile);

        struct FireEvent
        {
            RE::NiPoint3 startWorld;
            RE::NiPoint3 targetWorld;
            // Camera-forward at the moment of fire. Captured by the
            // detour so the consumer can evaluate close-quarters
            // fallback using the SAME camera direction the projectile
            // actually used — without this, a fast spin between fire
            // and consume rotates the consumer's camFwd far enough that
            // its close-quarters check falsely fires and the trace
            // ends up pointing where the user is looking NOW instead
            // of where the spell actually went.
            RE::NiPoint3 fireCamFwd;
            // Wall-clock (steady) seconds stamped at PUBLISH. The consumer's
            // dedup must compare EVENT times, not consumption times: two
            // real casts drained in the same Tick batch used to compare at
            // zero gap and collapse to one trail ("shooting back to back
            // extremely fast, only 1 projectile is traced"). Same-cast
            // multi-projectile events share one stamp; different casts are
            // separated by real time no matter when they're drained.
            double fireSec = 0.0;
            float        projSpeed = 0.0f;
            // Casting source captured at fire via nearest-magic-node match.
            // NOTE: this is the detour's OWN index (0=Right, 1=Left,
            // 2=Other, 3=Instant) — NOT the RE::MagicSystem::CastingSource
            // enum, whose hands are the REVERSE (0=Left, 1=Right).
            // Consumers converting to the enum must swap the hands.
            // -1 if unknown.
            int          castingSource = -1;
            // Ballistic snapshot from the actual projectile, never a mod-name
            // lookup. First-person uses its native launch velocity/direction.
            RE::NiPoint3 launchVelocity{};
            RE::NiPoint3 acceleration{};
            float range = 8000.0f;
            std::uint32_t projectileFormID{};
            float integrationStep = 1.0f/60.0f;
            std::uint32_t collisionFilter = static_cast<std::uint32_t>(RE::COL_LAYER::kProjectile);
            RE::ObjectRefHandle projectile;
            float launchAge = 0;
        };

        struct ImpactEvent {
            RE::ObjectRefHandle projectile;
            RE::NiPoint3 position{};
            float age = 0;
            double time = 0;
        };
        // The existing native contact observer supplies real collision points.
        static void ObserveImpact(RE::Projectile* projectile, const RE::NiPoint3& position);
        static bool FindImpact(RE::ObjectRefHandle projectile, double firedAt, ImpactEvent& out);

        // Drain all fire events with sequence > inOutLastSeen. Updates
        // inOutLastSeen to the latest sequence on return. Sequence-
        // numbered ring buffer (not last-write-wins) so dual-cast
        // (left+right hand both firing in the same engine tick)
        // publishes two events that both get consumed — the prior
        // single-slot publish dropped one of the two.
        static void DrainFireEvents(std::uint32_t& inOutLastSeen,
                                    std::vector<FireEvent>& outEvents);

        // Release-time camera capture. When the user releases the cast
        // button, CrosshairManager::Tick detects the falling edge and
        // publishes the niCam state at THAT moment via these. The
        // detour then reads the captured state at its first-tick (which
        // can fire 1-2 frames later due to spawn-animation timing).
        // Without this, the projectile fires at whatever direction the
        // camera happens to be in by the time the engine spawns it —
        // off by K frames of camera spin from where the user actually
        // aimed at release. With the capture, projectile direction =
        // release-time direction = user's click intent.
        static void PublishReleaseCapture(const RE::NiPoint3& forward,
                                          const RE::NiPoint3& up,
                                          const RE::NiPoint3& camPos,
                                          const RE::NiPoint3& target);
        // Returns true if a release capture is "fresh" — i.e., happened
        // within ~500ms. Reads the captured values out via the refs.
        static bool ReadReleaseCapture(RE::NiPoint3& outForward,
                                       RE::NiPoint3& outUp,
                                       RE::NiPoint3& outCamPos,
                                       RE::NiPoint3& outTarget);
        // Invalidate the capture once consumed so a stale capture
        // doesn't get applied to a later, unrelated projectile.
        static void InvalidateReleaseCapture();
    };
}
