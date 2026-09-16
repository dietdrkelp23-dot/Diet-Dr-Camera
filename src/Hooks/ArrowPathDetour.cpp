#include "PCH.h"
#include "Hooks/ArrowPathDetour.h"
#include "Camera/CameraNoiseController.h"
#include "Camera/NpcArchery.h"

#include <Windows.h>
#include <atomic>
#include <cmath>

namespace
{
    // Camera-aim snapshot, written from the main thread every game
    // frame and read by the per-tick hook below. Lock-free float
    // atomics on x86-64 — no torn reads. The version-counter sandwich
    // (read pre, components, read post; bail on mismatch) guards
    // against partial publishes.
    std::atomic<float>    sCamFx{0.0f}, sCamFy{1.0f}, sCamFz{0.0f};
    std::atomic<float>    sCamUx{0.0f}, sCamUy{0.0f}, sCamUz{1.0f};
    std::atomic<float>    sCamPx{0.0f}, sCamPy{0.0f}, sCamPz{0.0f};
    std::atomic<float>    sCamTx{0.0f}, sCamTy{1.0f}, sCamTz{0.0f};  // target point
    std::atomic<uint32_t> sCamVersion{0};
    std::atomic<bool>     sCamValid{false};

    // Last observed velocity-scaling product (s2 * power * speedMult)
    // keyed by ammo FormID. Stored from the per-tick hook on the
    // first tick after spawn, read by the prediction. Lets the
    // prediction match the actual arrow speed without mirroring the
    // engine S2 globals (which differ between game versions).
    std::atomic<float>    sLastVelMult{1.0f};
    std::atomic<uint32_t> sLastAmmoFormID{0};
    std::atomic<bool>     sLastVelMultValid{false};

    // Arrow-fire event for CrosshairManager. Publishes start (arrow
    // spawn world position), target (camera-aim raycast world point),
    // and approximate fire speed so the post-fire visualizer can draw
    // a straight line from start to target and clock travel time as
    // distance / speed. Same version-sandwich pattern as the camera
    // snapshot — readers bail on torn reads.
    std::atomic<uint32_t> sFireVersion{0};
    std::atomic<bool>     sFireValid{false};
    std::atomic<float>    sFireStartX{0.0f}, sFireStartY{0.0f}, sFireStartZ{0.0f};
    std::atomic<float>    sFireTargetX{0.0f}, sFireTargetY{0.0f}, sFireTargetZ{0.0f};
    std::atomic<float>    sFireSpeed{3000.0f};


    // Original `Projectile::UpdateImpl(float)` — the per-tick update
    // entry point in the projectile vtable (slot 0xAB). Populated by
    // the vtable patch installed by `Install()`. Null until then.
    using UpdateImplFn = void(*)(RE::Projectile*, float);
    UpdateImplFn _originalUpdateImpl = nullptr;
    bool sInstalled = false;

    // Engine's S2 (weapon-speed scalar) and Power (draw-strength
    // 0..1) helpers, called as `__thiscall` on the projectile.
    using ProjectileFloatGetter = float(*)(RE::Projectile*);

    // Lock-free read of the main-thread camera snapshot. Forwards to
    // the public static so MissileProjectileDetour reads from the
    // same single source of truth.
    bool ReadCameraSnapshot(RE::NiPoint3& outForward, RE::NiPoint3& outUp,
                            RE::NiPoint3& outPos, RE::NiPoint3& outTarget)
    {
        return DietDrCamera::ArrowPathDetour::ReadCameraSnapshot(
            outForward, outUp, outPos, outTarget);
    }

    // Per-tick hook on `ArrowProjectile::UpdateImpl(float)`. Because
    // we patch ONLY `VTABLE_ArrowProjectile[0]` slot 0xAB, this fires
    // exclusively for ArrowProjectile instances. MissileProjectile
    // (firebolt), FlameProjectile, BeamProjectile, BarrierProjectile
    // etc. all keep their own vtables and never enter here — which
    // is what lets us rewrite arrow direction without crashing the
    // game's spell pipeline (the previous function-entry trampoline
    // on `Projectile::UpdateFlightPath` fired for every projectile
    // and was the firebolt-crash trigger).
    //
    // Approach: on the first tick after spawn, set the projectile's
    // stored angle (`data.angle.x` pitch / `data.angle.z` yaw) to
    // the camera's aim direction. The engine's UpdateFlightPath
    // (called inside the original UpdateImpl) computes velocity from
    // those angles, so the arrow flies along camera aim instead of
    // body aim. Subsequent ticks pass through unmodified — the
    // engine continues from the velocity we steered.
    void HookedUpdateImpl(RE::Projectile* a_proj, float a_delta)
    {
        if (!_originalUpdateImpl) return;
        if (!a_proj) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        auto& runtime = a_proj->GetProjectileRuntimeData();

        // Player-only.
        auto handlePtr = runtime.shooter.get();
        if (!handlePtr || handlePtr.get() != ply) {
            const bool firstTick = (runtime.flags.underlying() & 0x80000000u) == 0;
            const bool archery = runtime.weaponSource && (runtime.weaponSource->IsBow() || runtime.weaponSource->IsCrossbow());
            if (handlePtr && DietDrCamera::NpcNoise::IsNewNpcArcheryShot(firstTick, false, archery, runtime.ammoSource != nullptr))
                DietDrCamera::CameraNoiseController::NotifyNpcArcheryShot(
                    runtime.shooter, runtime.weaponSource->IsCrossbow(), runtime.weaponSource->GetFormID(),
                    a_proj->GetFormID(), a_proj->GetPosition());
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Need weapon + ammo source — ArrowProjectile generally has
        // these but defensive-check before any field access on them.
        if (!runtime.weaponSource || !runtime.ammoSource) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Only the camera states that benefit. 1p / dialogue / vanity
        // pass through unmodified — they were never the problem.
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->currentState) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }
        const auto stateId = cam->currentState->id;
        const bool eligible =
            stateId == RE::CameraState::kThirdPerson ||
            stateId == RE::CameraState::kMount       ||
            stateId == RE::CameraState::kDragon;
        if (!eligible) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Bit 31 of `flags` is the engine's "already-flying" marker.
        // Only override on the first tick post-spawn — afterward the
        // engine maintains its own flight state from velocity.
        const bool firstTickAfterSpawn = (((runtime.flags.underlying() >> 0x1F) & 1u) == 0u);

        if (firstTickAfterSpawn) {
            RE::NiPoint3 forward, up, camPos, target;
            if (ReadCameraSnapshot(forward, up, camPos, target)) {
                // True-aim: aim toward the actual world point the
                // camera-forward ray hits (computed by the main
                // thread via raycast and published in the snapshot).
                // This makes the arrow LAND on the crosshair regardless
                // of the camera↔bow lateral offset, fixing the
                // "shifted to the right" miss that the parallel-aim
                // approach couldn't fully correct.
                const RE::NiPoint3 arrowPos = a_proj->GetPosition();
                RE::NiPoint3 aimDir{
                    target.x - arrowPos.x,
                    target.y - arrowPos.y,
                    target.z - arrowPos.z,
                };
                // Sneak / close-quarters guard: if the raycast target
                // ended up near (or behind) the projectile spawn along
                // camera-forward, true-aim collapses — the lateral
                // bow↔camera offset dominates and the arrow flies
                // off-axis. Below kMinForwardDist (~25 ft) the parallax
                // error from camera-forward parallel aim is small
                // enough to ignore, while true-aim is catastrophic.
                constexpr float kMinForwardDist = 500.0f;
                const float fwdDist = aimDir.x * forward.x +
                                      aimDir.y * forward.y +
                                      aimDir.z * forward.z;
                if (fwdDist < kMinForwardDist) {
                    aimDir = forward;
                } else {
                    const float aimLen = std::sqrt(aimDir.x*aimDir.x +
                                                   aimDir.y*aimDir.y +
                                                   aimDir.z*aimDir.z);
                    if (aimLen > 0.001f) {
                        aimDir.x /= aimLen;
                        aimDir.y /= aimLen;
                        aimDir.z /= aimLen;
                    } else {
                        aimDir = forward;
                    }
                }

                // Build a basis around aimDir using Gram-Schmidt against
                // camera-right (= forward × up). The earlier worldUp-cross
                // approach degenerates when aiming near-vertical — the
                // cross product collapses to zero, the tilt vanishes, and
                // the arrow flies bare-aim instead of with the f3PArrow-
                // TiltUpAngle compensation. camera-right is always
                // perpendicular to camera-forward and gives a stable
                // side axis regardless of aim pitch.
                RE::NiPoint3 right{
                    forward.y * up.z - forward.z * up.y,
                    forward.z * up.x - forward.x * up.z,
                    forward.x * up.y - forward.y * up.x,
                };
                {
                    const float dot = right.x * aimDir.x + right.y * aimDir.y + right.z * aimDir.z;
                    right.x -= aimDir.x * dot;
                    right.y -= aimDir.y * dot;
                    right.z -= aimDir.z * dot;
                    const float rLen = std::sqrt(right.x*right.x + right.y*right.y + right.z*right.z);
                    if (rLen > 0.001f) {
                        right.x /= rLen;
                        right.y /= rLen;
                        right.z /= rLen;
                    } else {
                        right = RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
                    }
                }
                const RE::NiPoint3 aimUp{
                    right.y * aimDir.z - right.z * aimDir.y,
                    right.z * aimDir.x - right.x * aimDir.z,
                    right.x * aimDir.y - right.y * aimDir.x,
                };

                float tiltDeg = 2.5f;
                if (auto* iniColl = RE::INISettingCollection::GetSingleton()) {
                    if (auto* s = iniColl->GetSetting("f3PArrowTiltUpAngle:Combat"))
                        tiltDeg = s->GetFloat();
                }
                constexpr float kDeg2Rad = 0.0174532925f;
                const float c = std::cos(tiltDeg * kDeg2Rad);
                const float s = std::sin(tiltDeg * kDeg2Rad);
                const RE::NiPoint3 fireDir{
                    aimDir.x * c + aimUp.x * s,
                    aimDir.y * c + aimUp.y * s,
                    aimDir.z * c + aimUp.z * s
                };

                // Decompose unit fireDir to engine pitch/yaw.
                const float fz = std::clamp(fireDir.z, -1.0f, 1.0f);
                const float pitch = -std::asin(fz);
                const float yaw   = std::atan2(fireDir.x, fireDir.y);

                a_proj->data.angle.x = pitch;
                a_proj->data.angle.z = yaw;

                // Also write linearVelocity directly (SmoothCam's
                // approach). data.angle alone may be insufficient if
                // the engine has already cached velocity from the
                // body-aim spawn. Both writes together: data.angle
                // covers per-tick recomputation, linearVelocity
                // covers the first integration step.
                static REL::Relocation<ProjectileFloatGetter> sS2Fn{ RELOCATION_ID(42537, 43700) };
                static REL::Relocation<ProjectileFloatGetter> sPowerFn{ RELOCATION_ID(42536, 43699) };
                const float s2    = sS2Fn(a_proj);
                const float power = sPowerFn(a_proj);
                const auto* projForm = a_proj->GetBaseObject() ? a_proj->GetBaseObject()->As<RE::BGSProjectile>() : nullptr;
                if (projForm) {
                    const float velScalar = s2 * projForm->data.speed * power * runtime.speedMult;
                    runtime.linearVelocity.x = fireDir.x * velScalar;
                    runtime.linearVelocity.y = fireDir.y * velScalar;
                    runtime.linearVelocity.z = fireDir.z * velScalar;
                }
                // Cache full-draw velMult (s2 * speedMult, no power)
                // so the next prediction always shows the trajectory
                // for a fully drawn shot. Including `power` here meant
                // tap-fires (low power) cached a tiny velMult, then
                // the next draw's prediction inherited that short
                // trajectory until another full-draw shot landed.
                // The actual fire-time velocity below still uses
                // power for the live shot's ballistics — only the
                // cached predict-next-shot scalar drops it.
                const float velMult = s2 * runtime.speedMult;
                sLastVelMult.store(velMult, std::memory_order_relaxed);

                std::uint32_t ammoFid = 0;
                if (auto* ammo = runtime.ammoSource) ammoFid = ammo->GetFormID();
                sLastAmmoFormID.store(ammoFid, std::memory_order_relaxed);
                sLastVelMultValid.store(true, std::memory_order_release);

                const float ammoSpdFire = projForm ? projForm->data.speed : 0.0f;
                const float velScalarFire = ammoSpdFire * s2 * power * runtime.speedMult;
                // Publish fire event for CrosshairManager. Start =
                // arrow's actual spawn position; target = the camera
                // raycast point we just aimed at; speed = the launch
                // velocity magnitude (s2 * ammoSpeed * power * speedMult).
                {
                    const uint32_t v = sFireVersion.fetch_add(1, std::memory_order_acq_rel);
                    sFireStartX.store(arrowPos.x, std::memory_order_relaxed);
                    sFireStartY.store(arrowPos.y, std::memory_order_relaxed);
                    sFireStartZ.store(arrowPos.z, std::memory_order_relaxed);
                    sFireTargetX.store(target.x,  std::memory_order_relaxed);
                    sFireTargetY.store(target.y,  std::memory_order_relaxed);
                    sFireTargetZ.store(target.z,  std::memory_order_relaxed);
                    sFireSpeed.store(velScalarFire > 100.0f ? velScalarFire : 3000.0f,
                                     std::memory_order_relaxed);
                    sFireVersion.store(v + 2, std::memory_order_release);
                    sFireValid.store(true, std::memory_order_release);
                }

                spdlog::info(
                    "[ArrowPathDetour] FIRE s2={:.3f} power={:.3f} ammoSpeed={:.1f} "
                    "speedMult={:.3f} velScalar={:.1f} pitch={:.3f} yaw={:.3f}",
                    s2, power, ammoSpdFire, runtime.speedMult, velScalarFire,
                    pitch, yaw);
            }
        }

        // Original UpdateImpl reads `data.angle` (which we may have
        // just rewritten) on the first-tick branch of UpdateFlightPath,
        // computes linearVelocity from it, then integrates position +
        // does the rest of the engine's per-tick projectile work.
        _originalUpdateImpl(a_proj, a_delta);
    }
}

namespace DietDrCamera
{
    bool ArrowPathDetour::GetLastVelMult(float& outVelMult, std::uint32_t& outAmmoFormID)
    {
        if (!sLastVelMultValid.load(std::memory_order_acquire)) return false;
        outVelMult     = sLastVelMult.load(std::memory_order_relaxed);
        outAmmoFormID  = sLastAmmoFormID.load(std::memory_order_relaxed);
        return true;
    }

    bool ArrowPathDetour::GetLastFireEvent(std::uint32_t& outVersion,
                                           RE::NiPoint3& outStartWorld,
                                           RE::NiPoint3& outTargetWorld,
                                           float& outSpeed)
    {
        if (!sFireValid.load(std::memory_order_acquire)) return false;
        const uint32_t v0 = sFireVersion.load(std::memory_order_acquire);
        const float sx = sFireStartX.load(std::memory_order_relaxed);
        const float sy = sFireStartY.load(std::memory_order_relaxed);
        const float sz = sFireStartZ.load(std::memory_order_relaxed);
        const float tx = sFireTargetX.load(std::memory_order_relaxed);
        const float ty = sFireTargetY.load(std::memory_order_relaxed);
        const float tz = sFireTargetZ.load(std::memory_order_relaxed);
        const float sp = sFireSpeed.load(std::memory_order_relaxed);
        const uint32_t v1 = sFireVersion.load(std::memory_order_acquire);
        if (v0 != v1) return false;
        outVersion     = v0;
        outStartWorld  = RE::NiPoint3{ sx, sy, sz };
        outTargetWorld = RE::NiPoint3{ tx, ty, tz };
        outSpeed       = sp;
        return true;
    }



    void ArrowPathDetour::PublishCameraSnapshot(float fx, float fy, float fz,
                                                float ux, float uy, float uz,
                                                float px, float py, float pz,
                                                float tx, float ty, float tz)
    {
        const uint32_t v = sCamVersion.fetch_add(1, std::memory_order_acq_rel);
        sCamFx.store(fx, std::memory_order_relaxed);
        sCamFy.store(fy, std::memory_order_relaxed);
        sCamFz.store(fz, std::memory_order_relaxed);
        sCamUx.store(ux, std::memory_order_relaxed);
        sCamUy.store(uy, std::memory_order_relaxed);
        sCamUz.store(uz, std::memory_order_relaxed);
        sCamPx.store(px, std::memory_order_relaxed);
        sCamPy.store(py, std::memory_order_relaxed);
        sCamPz.store(pz, std::memory_order_relaxed);
        sCamTx.store(tx, std::memory_order_relaxed);
        sCamTy.store(ty, std::memory_order_relaxed);
        sCamTz.store(tz, std::memory_order_relaxed);
        sCamVersion.store(v + 2, std::memory_order_release);
        sCamValid.store(true, std::memory_order_release);
    }

    void ArrowPathDetour::InvalidateCameraSnapshot()
    {
        sCamValid.store(false, std::memory_order_release);
    }

    bool ArrowPathDetour::ReadCameraSnapshot(RE::NiPoint3& outForward,
                                             RE::NiPoint3& outUp,
                                             RE::NiPoint3& outPos,
                                             RE::NiPoint3& outTarget)
    {
        if (!sCamValid.load(std::memory_order_acquire)) return false;
        const uint32_t v0 = sCamVersion.load(std::memory_order_acquire);
        const float fx = sCamFx.load(std::memory_order_relaxed);
        const float fy = sCamFy.load(std::memory_order_relaxed);
        const float fz = sCamFz.load(std::memory_order_relaxed);
        const float ux = sCamUx.load(std::memory_order_relaxed);
        const float uy = sCamUy.load(std::memory_order_relaxed);
        const float uz = sCamUz.load(std::memory_order_relaxed);
        const float px = sCamPx.load(std::memory_order_relaxed);
        const float py = sCamPy.load(std::memory_order_relaxed);
        const float pz = sCamPz.load(std::memory_order_relaxed);
        const float tx = sCamTx.load(std::memory_order_relaxed);
        const float ty = sCamTy.load(std::memory_order_relaxed);
        const float tz = sCamTz.load(std::memory_order_relaxed);
        const uint32_t v1 = sCamVersion.load(std::memory_order_acquire);
        if (v0 != v1) return false;
        outForward = RE::NiPoint3{ fx, fy, fz };
        outUp      = RE::NiPoint3{ ux, uy, uz };
        outPos     = RE::NiPoint3{ px, py, pz };
        outTarget  = RE::NiPoint3{ tx, ty, tz };
        return true;
    }

    void ArrowPathDetour::Install()
    {
        if (sInstalled) return;

        // SmoothCam patches the engine flight-path function via MS
        // Detours — different mechanism, no vtable conflict — but
        // we still defer to it so the load order's intent is honored.
        if (GetModuleHandleA("SmoothCam.dll")) {
            spdlog::info("[ArrowPathDetour] SmoothCam.dll detected — skipping vtable patch");
            sInstalled = true;
            return;
        }

        sInstalled = true;

        // Patch ONLY the ArrowProjectile vtable (slot 0xAB =
        // UpdateImpl). MissileProjectile, FlameProjectile, etc. each
        // have their own vtables and are never affected by this.
        // This is the key difference from the previous
        // function-entry trampoline on Projectile::UpdateFlightPath,
        // which fired for every projectile and was crashing firebolt.
        REL::Relocation<std::uintptr_t> arrowVtbl{ RE::VTABLE_ArrowProjectile[0] };
        _originalUpdateImpl = reinterpret_cast<UpdateImplFn>(
            arrowVtbl.write_vfunc(0xAB, &HookedUpdateImpl));
        spdlog::info("[ArrowPathDetour] vtable patched at 0x{:x} slot 0xAB",
                     arrowVtbl.address());
    }
}
