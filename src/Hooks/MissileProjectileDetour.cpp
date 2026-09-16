#include "PCH.h"
#include "Hooks/MissileProjectileDetour.h"
#include "Hooks/ArrowPathDetour.h"
#include "Settings/SettingsManager.h"
#include "Camera/StateResolver.h"
#include "Camera/CameraNoiseController.h"
#include "Camera/SpellTrajectory.h"
#include "LockOn/TDMIntegration.h"
#include "RE/B/bhkSimpleShapePhantom.h"
#include "RE/H/hkpSimpleShapePhantom.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    // Per-spell velocity-scaling cache. Spells don't have weapon S2 or
    // draw power — runtime.speedMult is the only multiplier the engine
    // adds on top of the BGSProjectile's base speed. Cached after the
    // first observed shot so the predictor can match flight speed
    // before any tracing-relevant frame.
    std::atomic<float>    sLastVelMult{1.0f};
    std::atomic<uint32_t> sLastProjFormID{0};
    std::atomic<bool>     sLastVelMultValid{false};

    // Release-time camera capture. CrosshairManager::Tick publishes
    // here on the magic release falling edge; the detour reads at its
    // first-tick to use the release-moment direction instead of the
    // spawn-moment direction (which is K frames later due to spawn-
    // animation timing — visible as projectile firing where the camera
    // WAS when engine spawned it, not where camera was at click).
    std::atomic<float>    sRelFx{0.0f}, sRelFy{1.0f}, sRelFz{0.0f};

    // rendered = clean * applied  =>  clean = rendered * appliedᵀ. Same
    // helper as CrosshairManager's (not exported from there — duplicated
    // rather than shared across a module boundary for one 3x3 multiply).
    RE::NiMatrix3 MulByTranspose(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
    {
        RE::NiMatrix3 r;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.entry[i][j] = a.entry[i][0] * b.entry[j][0] +
                                a.entry[i][1] * b.entry[j][1] +
                                a.entry[i][2] * b.entry[j][2];
            }
        }
        return r;
    }
    std::atomic<float>    sRelUx{0.0f}, sRelUy{0.0f}, sRelUz{1.0f};
    std::atomic<float>    sRelPx{0.0f}, sRelPy{0.0f}, sRelPz{0.0f};
    std::atomic<float>    sRelTx{0.0f}, sRelTy{1.0f}, sRelTz{0.0f};
    std::atomic<uint32_t> sRelVersion{0};
    std::atomic<bool>     sRelValid{false};
    std::atomic<float>    sRelWallSec{0.0f};

    // Spell-fire event ring. Sequence-numbered so consumers can drain
    // multiple events accumulated between polls — critical for dual-
    // cast where both hands publish in the same engine tick. Single-
    // slot publish dropped one of the two. Ring depth 8 is well above
    // the realistic max-in-flight-per-frame count (2 hands * 1 fire-
    // first-tick = 2).
    constexpr std::size_t                       kFireRingSize = 8;
    struct FireSlot
    {
        std::uint32_t sequence    = 0;
        RE::NiPoint3  startWorld  {};
        RE::NiPoint3  targetWorld {};
        RE::NiPoint3  fireCamFwd  {};
        double        fireSec     = 0.0;   // steady seconds at publish (see FireEvent)
        float         projSpeed   = 0.0f;
        int           castingSource = -1;
        RE::NiPoint3 launchVelocity{}, acceleration{};
        float range = 8000.0f;
        std::uint32_t projectileFormID{};
        float integrationStep = 1.0f/60.0f;
        std::uint32_t collisionFilter = static_cast<std::uint32_t>(RE::COL_LAYER::kProjectile);
    };
    std::array<FireSlot, kFireRingSize> sFireRing;
    std::mutex                          sFireMutex;
    std::unordered_map<std::uint32_t, float> sSpeedMultipliers;
    std::unordered_map<std::uint32_t, std::uint32_t> sCollisionLayers;
    std::uint32_t                       sFireSeq = 0;

    using UpdateImplFn = void(*)(RE::Projectile*, float);
    UpdateImplFn _originalUpdateImpl = nullptr;
    UpdateImplFn _originalConeUpdateImpl = nullptr;
    bool sInstalled = false;

    // Bounded flight capture for the reported preview/actual-height mismatch.
    // Record native movement as well as the visible node; launch parameters
    // alone cannot establish where an initialized projectile actually flies.
    struct MissileFlightProbe
    {
        RE::Projectile* projectile;
        float delta;
        RE::NiPoint3 position{}, velocity{};
        std::uint32_t flags{};
        int tick = -1;

        MissileFlightProbe(RE::Projectile* p, float dt) : projectile(p), delta(dt)
        {
            struct Track { std::uint32_t id{}; int ticks{}; };
            static std::array<Track, 4> tracks;
            static std::mutex mutex;
            std::lock_guard lock(mutex);
            const auto id = p->GetFormID();
            for (auto& track : tracks) {
                if (track.id != 0 && track.id != id) continue;
                track.id = id;
                const int n = track.ticks++;
                if (n < 4 || n == 7 || n == 11 || n == 15 || n == 23 || n == 31 || n == 47) tick = n;
                break;
            }
            if (tick < 0) return;
            const auto& runtime = p->GetProjectileRuntimeData();
            position = p->GetPosition();
            velocity = runtime.linearVelocity;
            flags = runtime.flags.underlying();
        }

        ~MissileFlightProbe()
        {
            if (tick < 0) return;
            const auto& runtime = projectile->GetProjectileRuntimeData();
            const auto after = projectile->GetPosition();
            const auto v = runtime.linearVelocity;
            const auto* node = projectile->Get3D();
            const auto visual = node ? node->world.translate : after;
            spdlog::info("[SpellFlight] ref={:08X} tick={} dt={:.5f} life={:.5f} flags={:08X}->{:08X} "
                "pos=({:.3f},{:.3f},{:.3f})->({:.3f},{:.3f},{:.3f}) "
                "vel=({:.3f},{:.3f},{:.3f})->({:.3f},{:.3f},{:.3f}) visual=({:.3f},{:.3f},{:.3f})",
                projectile->GetFormID(), tick, delta, runtime.livingTime, flags, runtime.flags.underlying(),
                position.x, position.y, position.z, after.x, after.y, after.z,
                velocity.x, velocity.y, velocity.z, v.x, v.y, v.z, visual.x, visual.y, visual.z);
        }
    };

    // Does any of the spell's effects have the Hostile flag set?
    // This is the gate that distinguishes Firebolt / Ice Spike /
    // Lightning Bolt / Fury / Calm / Turn Undead (all hostile-flagged)
    // from Soul Trap and other utility missile spells (not hostile).
    bool SpellHasHostileEffect(RE::MagicItem* a_spell)
    {
        if (!a_spell) return false;
        for (auto* effect : a_spell->effects) {
            if (!effect) continue;
            auto* base = effect->baseEffect;
            if (base && base->IsHostile()) return true;
        }
        return false;
    }

    void HookedUpdateImpl(RE::Projectile* a_proj, float a_delta)
    {
        if (!_originalUpdateImpl) return;
        if (!a_proj) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Master kill switch — if the user disables spell tracing,
        // we want zero behavior change (projectiles fly with vanilla
        // body-aim direction). The check is cheap; do it before any
        // field access on the projectile.
        if (!DietDrCamera::SettingsManager::GetSingleton().spellTracingEnabled) {
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
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Source spell must be set and hostile. Soul Trap, Bound Bow
        // (Self delivery — no missile spawn anyway), Healing Hands etc.
        // skip the override here.
        if (!SpellHasHostileEffect(runtime.spell)) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }

        // Shouts USED to be excluded here — projectile shouts (Unrelenting
        // Force, Ice Form, Fire Breath) carry hostile effects so they pass the
        // gate above, and the original ask was for them to keep vanilla aim.
        // Vanilla aim for a shout is the BODY's facing, which in a third-person
        // camera with any side offset or free-look pitch is not where the
        // player is looking — "shouts don't care where I'm aiming, they just
        // fire straight forward". So they now take the same camera-aim path as
        // every other player projectile. `spellTracingEnabled` remains the
        // single kill switch for all of it.
        //
        // Same eligible-camera-state gate as ArrowPathDetour, PLUS first
        // person. Dialogue / vanity pass through. 1p is publish-only: the
        // engine already fires along the look ray there, so the aim/velocity
        // steering below is skipped — but the FIRE event must still publish,
        // because spells have no eager-push fallback (bows do): with the old
        // 3p-only gate a 1p cast never created a shot, so the trace vanished
        // at release ("1p traces don't stay after firing", 2026-08-16).
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->currentState) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }
        const auto stateId = cam->currentState->id;
        const bool eligible =
            stateId == RE::CameraState::kThirdPerson ||
            stateId == RE::CameraState::kMount       ||
            stateId == RE::CameraState::kDragon      ||
            stateId == RE::CameraState::kFirstPerson;
        if (!eligible) {
            _originalUpdateImpl(a_proj, a_delta);
            return;
        }
        const bool steerAim = stateId != RE::CameraState::kFirstPerson;
        MissileFlightProbe flightProbe(a_proj, a_delta);

        // Bit 31 of `flags` is the engine's "already flying" marker —
        // same as arrow path. Only override on the first tick post-
        // spawn; after that the engine integrates from velocity.
        const bool firstTickAfterSpawn = (((runtime.flags.underlying() >> 0x1F) & 1u) == 0u);

        if (firstTickAfterSpawn) {
            RE::NiPoint3 forward, up, camPos, target;
            bool haveCam = false;
            // Priority 1: release-time capture published by
            // HookedThirdPersonUpdate on the release falling edge.
            // Do NOT invalidate on first read — the engine spawns
            // multiple projectile-update calls per fire (the same
            // spawn ticks UpdateImpl twice in quick succession with
            // firstTickAfterSpawn=true both times), and BOTH must
            // use the same captured direction. Without this, the
            // second tick falls back to live niCam which has drifted
            // a frame's worth — visible at fast spin as the second
            // projectile firing off-axis from the trigger-pull
            // direction. The 500ms freshness check in
            // ReadReleaseCapture handles expiry so a stale capture
            // can't carry into the next fire cycle.
            if (DietDrCamera::MissileProjectileDetour::ReadReleaseCapture(
                    forward, up, camPos, target))
            {
                haveCam = true;
            }
            // Priority 2: live niCam read. Same data the snapshot
            // publisher would use, just at detour time.
            if (!haveCam)
            if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
                auto* asNode = pc->cameraRoot->AsNode();
                if (asNode && !asNode->GetChildren().empty()) {
                    if (auto* niCam = skyrim_cast<RE::NiCamera*>(asNode->GetChildren()[0].get())) {
                        auto cm = niCam->world.rotate;
                        camPos = niCam->world.translate;
                        if (steerAim) {
                            RE::NiMatrix3 noiseRotation;
                            RE::NiPoint3 noiseTranslation;
                            if (DietDrCamera::CameraNoiseController::GetAppliedCameraOffset3p(noiseRotation, noiseTranslation)) {
                                cm = MulByTranspose(cm, noiseRotation);
                                camPos = camPos - noiseTranslation;
                            }
                        }
                        forward = RE::NiPoint3{ cm.entry[0][0], cm.entry[1][0], cm.entry[2][0] };
                        up      = RE::NiPoint3{ cm.entry[0][1], cm.entry[1][1], cm.entry[2][1] };
                        // Fresh raycast from camera along live forward
                        // to derive target — same ray the snapshot
                        // publisher uses, but at fire time.
                        constexpr float kAimRayLen = 8000.0f;
                        target = RE::NiPoint3{
                            camPos.x + forward.x * kAimRayLen,
                            camPos.y + forward.y * kAimRayLen,
                            camPos.z + forward.z * kAimRayLen,
                        };
                        if (auto* cell = ply->GetParentCell()) {
                            if (auto* bhkW = cell->GetbhkWorld()) {
                                if (auto* hkW = bhkW->GetWorld1()) {
                                    const float ws = RE::bhkWorld::GetWorldScale();
                                    RE::hkpWorldRayCastInput  in;
                                    RE::hkpWorldRayCastOutput out;
                                    in.from.quad = _mm_setr_ps(camPos.x*ws, camPos.y*ws, camPos.z*ws, 0.0f);
                                    in.to.quad   = _mm_setr_ps(target.x*ws, target.y*ws, target.z*ws, 0.0f);
                                    in.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kCameraSphere);
                                    in.enableShapeCollectionFilter = false;
                                    hkW->CastRay(in, out);
                                    if (out.HasHit()) {
                                        target = RE::NiPoint3{
                                            camPos.x + forward.x * (kAimRayLen * out.hitFraction),
                                            camPos.y + forward.y * (kAimRayLen * out.hitFraction),
                                            camPos.z + forward.z * (kAimRayLen * out.hitFraction),
                                        };
                                    }
                                }
                            }
                        }
                        haveCam = true;
                    }
                }
            }
            // Fall back to published snapshot if live read failed
            // (e.g., transient camera-pointer null during state swap).
            if (!haveCam && !DietDrCamera::ArrowPathDetour::ReadCameraSnapshot(forward, up, camPos, target)) {
                _originalUpdateImpl(a_proj, a_delta);
                return;
            }
            // TARGET LOCK: fly at the LOCKED enemy, not at the crosshair
            // ray. The lock camera composes rotOffset + lockAimYaw + the
            // aim-bias / For-Honor framing, so screen center deliberately
            // is NOT on the target — steering spells down the center ray
            // missed by exactly that framing ("projectile spells aren't
            // accurate when target locking", user report 2026-08-15).
            bool lockedAim = false;
            if (DietDrCamera::StateResolver::GetSingleton().IsTargetLocked()) {
                if (auto h = DietDrCamera::TDMIntegration::GetSingleton().GetCurrentTarget()) {
                    if (auto tp = h.get()) {
                        RE::NiPoint3 tpos = tp->GetPosition();
                        // Aim at the torso, not the feet.
                        tpos.z += tp->GetHeight() * 0.55f;
                        target    = tpos;
                        lockedAim = true;
                    }
                }
            }
            {
                const RE::NiPoint3 projPos = a_proj->GetPosition();
                RE::NiPoint3 aimDir{
                    target.x - projPos.x,
                    target.y - projPos.y,
                    target.z - projPos.z,
                };
                // Sneak / close-quarters guard: see ArrowPathDetour
                // for the explanation. Below kMinForwardDist the
                // hand↔camera lateral offset dominates true-aim and
                // sends the projectile sideways. Falling back to
                // camera-forward parallel aim avoids the inversion.
                // A locked target is a REAL point (not a ray endpoint),
                // so lock aim skips the guard — close-range locks are
                // exactly where the direct aim matters most.
                constexpr float kMinForwardDist = 500.0f;
                const float fwdDist = aimDir.x * forward.x +
                                      aimDir.y * forward.y +
                                      aimDir.z * forward.z;
                if (!lockedAim && fwdDist < kMinForwardDist) {
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

                // Preserve the launch direction. Gravity is simulated by the
                // engine and reflected by the tracer, not compensated in aim.
                const RE::NiPoint3 fireDir = aimDir;

                // Decompose unit fireDir to engine pitch/yaw.
                const float fz = std::clamp(fireDir.z, -1.0f, 1.0f);
                const float pitch = -std::asin(fz);
                const float yaw   = std::atan2(fireDir.x, fireDir.y);

                if (steerAim) {
                    a_proj->data.angle.x = pitch;
                    a_proj->data.angle.z = yaw;
                }

                // Velocity scalar = projForm->data.speed * runtime.speedMult.
                // No s2, no power — spells fire at fixed power on release;
                // the engine's actor-skill-based speedMult is the only
                // variable factor.
                const auto* projForm = a_proj->GetBaseObject()
                    ? a_proj->GetBaseObject()->As<RE::BGSProjectile>() : nullptr;
                if (projForm) {
                    const float velScalar = projForm->data.speed * runtime.speedMult;
                    // PROJECTILE_RUNTIME_DATA has TWO velocity fields:
                    // velocity (0x0F0) and linearVelocity (0x0FC). Arrows
                    // only need linearVelocity. Missiles appear to read
                    // velocity in their first-tick path. Write BOTH so we
                    // cover whichever the engine actually integrates from.
                    if (steerAim) {
                        runtime.velocity.x       = fireDir.x * velScalar;
                        runtime.velocity.y       = fireDir.y * velScalar;
                        runtime.velocity.z       = fireDir.z * velScalar;
                        runtime.linearVelocity.x = fireDir.x * velScalar;
                        runtime.linearVelocity.y = fireDir.y * velScalar;
                        runtime.linearVelocity.z = fireDir.z * velScalar;
                    }

                    // Cache speedMult keyed by projForm FormID so the
                    // next-fire predictor matches the engine's actual
                    // launch velocity.
                    sLastVelMult.store(runtime.speedMult, std::memory_order_relaxed);
                    sLastProjFormID.store(projForm->GetFormID(), std::memory_order_relaxed);
                    sLastVelMultValid.store(true, std::memory_order_release);

                    // Identify which hand fired by matching projPos
                    // against the player's hand magic nodes. The
                    // consumer needs this to re-anchor the trail's
                    // polyline to the CURRENT magic-node position each
                    // render frame (so the trail tracks live camera
                    // direction from the right hand).
                    int castSrcIdx = -1;
                    {
                        float bestD2 = 1e9f;
                        const std::array sources = {
                            std::pair{ RE::MagicSystem::CastingSource::kRightHand, 0 },
                            std::pair{ RE::MagicSystem::CastingSource::kLeftHand,  1 },
                            std::pair{ RE::MagicSystem::CastingSource::kOther,     2 },
                            std::pair{ RE::MagicSystem::CastingSource::kInstant,   3 },
                        };
                        for (auto [src, idx] : sources) {
                            auto* c = ply->GetMagicCaster(src);
                            if (!c) continue;
                            auto* n = c->GetMagicNode();
                            if (!n) continue;
                            const auto& p = n->world.translate;
                            const float ddx = projPos.x - p.x;
                            const float ddy = projPos.y - p.y;
                            const float ddz = projPos.z - p.z;
                            const float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
                            if (d2 < bestD2) {
                                bestD2 = d2;
                                castSrcIdx = idx;
                            }
                        }
                    }

                    // The engine records the casting source directly. Prefer it
                    // over nearest-node geometry, especially with two different
                    // spells charged together or crossed-hand animations.
                    const int nativeSource = static_cast<int>(runtime.castingSource);
                    if (nativeSource >= 0 && nativeSource < 4)
                        castSrcIdx = nativeSource == 0 ? 1 : nativeSource == 1 ? 0 : nativeSource;

                    // Snapshot the real launch before the engine integrates it.
                    // First-person stays native; third-person uses our existing
                    // camera-aim direction. No gravity or velocity edits added.
                    RE::NiPoint3 launchVelocity = runtime.linearVelocity;
                    if (DietDrCamera::SpellTrajectory::Length(launchVelocity) < 0.001f)
                        launchVelocity = runtime.velocity;
                    if (!steerAim && DietDrCamera::SpellTrajectory::Length(launchVelocity) < 0.001f) {
                        const float p = a_proj->data.angle.x, y = a_proj->data.angle.z;
                        launchVelocity = RE::NiPoint3{std::sin(y)*std::cos(p)*velScalar,
                            std::cos(y)*std::cos(p)*velScalar, -std::sin(p)*velScalar};
                    }
                    const bool hitscan = projForm->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kHitScan);
                    const RE::NiPoint3 acceleration = DietDrCamera::SpellTrajectory::Acceleration<RE::NiPoint3>(
                        hitscan ? 0.0f : a_proj->GetGravity());
                    const float flightRange = runtime.range > 0 ? runtime.range : projForm->data.range;

                    spdlog::info(
                        "[MissileProjectileDetour] FIRE spellFid=0x{:08x} projSpeed={:.1f} "
                        "speedMult={:.3f} velScalar={:.1f} pitch={:.3f} yaw={:.3f} "
                        "projectile=0x{:08x} accelZ={:.3f} step={:.5f}",
                        runtime.spell ? runtime.spell->GetFormID() : 0u,
                        projForm->data.speed, runtime.speedMult, velScalar,
                        pitch, yaw, projForm->GetFormID(), acceleration.z, a_delta);

                    // Replay the velocity writes AFTER _originalUpdateImpl
                    // in case the engine's missile-specific first-tick path
                    // computes velocity from internal state and clobbers
                    // what we wrote above. We have to call original first
                    // (it does scene-graph setup, sound triggers, etc.)
                    // then reassert direction if flight hasn't begun. Once
                    // the integrator ran, retain the gravity it just applied
                    // to the next frame's velocity. In 1p (publish-only)
                    // the engine's own launch is left completely untouched.
                    _originalUpdateImpl(a_proj, a_delta);
                    if (steerAim && (runtime.flags.underlying() & 0x80000000u) == 0) {
                        runtime.velocity.x       = fireDir.x * velScalar;
                        runtime.velocity.y       = fireDir.y * velScalar;
                        runtime.velocity.z       = fireDir.z * velScalar;
                        runtime.linearVelocity.x = fireDir.x * velScalar;
                        runtime.linearVelocity.y = fireDir.y * velScalar;
                        runtime.linearVelocity.z = fireDir.z * velScalar;
                    }
                    // Initialization can run more than once and can relocate
                    // the projectile. Publish the first actual flight step,
                    // not an earlier setup pose that the consumer then dedups
                    // against the real launch.
                    if ((runtime.flags.underlying() & 0x80000000u) == 0) return;
                    // Read back the initialized projectile's actual collision
                    // filter and flight velocity. Other launch hooks can change
                    // these while original UpdateImpl is running.
                    std::uint32_t collisionFilter = DietDrCamera::MissileProjectileDetour::GetCollisionFilter(projForm);
                    if (runtime.unk0E0 && runtime.unk0E0->phantom) {
                        collisionFilter = runtime.unk0E0->phantom->collidable.broadPhaseHandle.collisionFilterInfo.filter;
                    }
                    const auto postVelocity = runtime.linearVelocity;
                    if (DietDrCamera::SpellTrajectory::Finite(postVelocity) &&
                        DietDrCamera::SpellTrajectory::Length(postVelocity) > 0.001f && a_delta > 0) {
                        launchVelocity = postVelocity - acceleration*a_delta;
                    }
                    // Publish into the ring buffer. Two dual-cast events
                    // arriving in the same engine tick both get a unique
                    // sequence and survive until the consumer drains.
                    {
                        std::lock_guard<std::mutex> lock(sFireMutex);
                        ++sFireSeq;
                        auto& slot      = sFireRing[sFireSeq % kFireRingSize];
                        slot.sequence   = sFireSeq;
                        slot.startWorld = projPos;
                        slot.targetWorld = target;
                        slot.fireCamFwd  = forward;
                        slot.fireSec    = std::chrono::duration<double>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        slot.castingSource = castSrcIdx;
                        slot.projSpeed = velScalar;
                        slot.launchVelocity = launchVelocity;
                        slot.acceleration = acceleration;
                        slot.range = flightRange;
                        slot.projectileFormID = projForm->GetFormID();
                        slot.integrationStep = DietDrCamera::SpellTrajectory::IntegrationStep(a_delta);
                        slot.collisionFilter = collisionFilter;
                        if (sSpeedMultipliers.size() >= 256) sSpeedMultipliers.clear();
                        sSpeedMultipliers[projForm->GetFormID()] = runtime.speedMult;
                        if (sCollisionLayers.size() >= 256) sCollisionLayers.clear();
                        sCollisionLayers[projForm->GetFormID()] = collisionFilter & 0xFFFFu;
                    }

                    return;
                }
            }
        }

        _originalUpdateImpl(a_proj, a_delta);
    }
}

namespace DietDrCamera
{
    bool MissileProjectileDetour::GetLastVelMult(float& outVelMult, std::uint32_t& outProjFormID)
    {
        if (!sLastVelMultValid.load(std::memory_order_acquire)) return false;
        outVelMult    = sLastVelMult.load(std::memory_order_relaxed);
        outProjFormID = sLastProjFormID.load(std::memory_order_relaxed);
        return true;
    }

    float MissileProjectileDetour::GetSpeedMultiplier(std::uint32_t projectileFormID)
    {
        std::lock_guard<std::mutex> lock(sFireMutex);
        const auto it = sSpeedMultipliers.find(projectileFormID);
        return it != sSpeedMultipliers.end() && std::isfinite(it->second) && it->second > 0
            ? it->second : 1.0f;
    }

    std::uint32_t MissileProjectileDetour::GetCollisionFilter(const RE::BGSProjectile* projectile)
    {
        std::uint32_t filter = static_cast<std::uint32_t>(RE::COL_LAYER::kProjectile);
        if (projectile) {
            if (projectile->data.collisionLayer) filter = projectile->data.collisionLayer->collisionIdx;
            std::lock_guard lock(sFireMutex);
            if (const auto it = sCollisionLayers.find(projectile->GetFormID()); it != sCollisionLayers.end())
                filter = it->second;
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            RE::CFilter actorFilter{};
            player->GetCollisionFilterInfo(actorFilter);
            filter |= actorFilter.filter & 0xFFFF0000u;
        }
        return filter;
    }

    void MissileProjectileDetour::DrainFireEvents(std::uint32_t& inOutLastSeen,
                                                  std::vector<FireEvent>& outEvents)
    {
        std::lock_guard<std::mutex> lock(sFireMutex);
        if (sFireSeq <= inOutLastSeen) return;
        // If more than kFireRingSize fires accumulated between polls,
        // older entries have been overwritten — start from the oldest
        // sequence still resident in the ring.
        const std::uint32_t oldestResident = (sFireSeq > kFireRingSize)
            ? sFireSeq - static_cast<std::uint32_t>(kFireRingSize) + 1u
            : 1u;
        std::uint32_t startSeq = inOutLastSeen + 1u;
        if (startSeq < oldestResident) startSeq = oldestResident;
        for (std::uint32_t s = startSeq; s <= sFireSeq; ++s) {
            const auto& slot = sFireRing[s % kFireRingSize];
            if (slot.sequence != s) continue;
            outEvents.push_back(FireEvent{
                slot.startWorld, slot.targetWorld, slot.fireCamFwd,
                slot.fireSec, slot.projSpeed, slot.castingSource,
                slot.launchVelocity, slot.acceleration, slot.range, slot.projectileFormID, slot.integrationStep,
                slot.collisionFilter });
        }
        inOutLastSeen = sFireSeq;
    }

    void MissileProjectileDetour::PublishReleaseCapture(const RE::NiPoint3& forward,
                                                        const RE::NiPoint3& up,
                                                        const RE::NiPoint3& camPos,
                                                        const RE::NiPoint3& target)
    {
        const uint32_t v = sRelVersion.fetch_add(1, std::memory_order_acq_rel);
        sRelFx.store(forward.x, std::memory_order_relaxed);
        sRelFy.store(forward.y, std::memory_order_relaxed);
        sRelFz.store(forward.z, std::memory_order_relaxed);
        sRelUx.store(up.x, std::memory_order_relaxed);
        sRelUy.store(up.y, std::memory_order_relaxed);
        sRelUz.store(up.z, std::memory_order_relaxed);
        sRelPx.store(camPos.x, std::memory_order_relaxed);
        sRelPy.store(camPos.y, std::memory_order_relaxed);
        sRelPz.store(camPos.z, std::memory_order_relaxed);
        sRelTx.store(target.x, std::memory_order_relaxed);
        sRelTy.store(target.y, std::memory_order_relaxed);
        sRelTz.store(target.z, std::memory_order_relaxed);
        const float nowSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        sRelWallSec.store(nowSec, std::memory_order_relaxed);
        sRelVersion.store(v + 2, std::memory_order_release);
        sRelValid.store(true, std::memory_order_release);
    }

    bool MissileProjectileDetour::ReadReleaseCapture(RE::NiPoint3& outForward,
                                                     RE::NiPoint3& outUp,
                                                     RE::NiPoint3& outCamPos,
                                                     RE::NiPoint3& outTarget)
    {
        if (!sRelValid.load(std::memory_order_acquire)) return false;
        // Stale check: anything older than 500ms is from a previous
        // cast cycle, ignore.
        const float capSec = sRelWallSec.load(std::memory_order_relaxed);
        const float nowSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if ((nowSec - capSec) > 0.5f) {
            sRelValid.store(false, std::memory_order_release);
            return false;
        }
        const uint32_t v0 = sRelVersion.load(std::memory_order_acquire);
        const float fx = sRelFx.load(std::memory_order_relaxed);
        const float fy = sRelFy.load(std::memory_order_relaxed);
        const float fz = sRelFz.load(std::memory_order_relaxed);
        const float ux = sRelUx.load(std::memory_order_relaxed);
        const float uy = sRelUy.load(std::memory_order_relaxed);
        const float uz = sRelUz.load(std::memory_order_relaxed);
        const float px = sRelPx.load(std::memory_order_relaxed);
        const float py = sRelPy.load(std::memory_order_relaxed);
        const float pz = sRelPz.load(std::memory_order_relaxed);
        const float tx = sRelTx.load(std::memory_order_relaxed);
        const float ty = sRelTy.load(std::memory_order_relaxed);
        const float tz = sRelTz.load(std::memory_order_relaxed);
        const uint32_t v1 = sRelVersion.load(std::memory_order_acquire);
        if (v0 != v1) return false;  // torn read
        outForward = RE::NiPoint3{ fx, fy, fz };
        outUp      = RE::NiPoint3{ ux, uy, uz };
        outCamPos  = RE::NiPoint3{ px, py, pz };
        outTarget  = RE::NiPoint3{ tx, ty, tz };
        return true;
    }

    void MissileProjectileDetour::InvalidateReleaseCapture()
    {
        sRelValid.store(false, std::memory_order_release);
    }

    // ---- Cone projectiles: every shout that leaves the player -------------
    //
    // The missile hook above never sees a single shout. A raw PROJ scan of the
    // shipped ESMs says why: EVERY vanilla shout projectile is type 0x10 Cone
    // (Unrelenting Force, Ice Form, Disarm, Dismay, Dragonrend, Clear Skies,
    // Marked for Death, the frost / fire waves...) or 0x08 Flamethrower (Fire
    // Breath). Not one is a Missile. So merely deleting the shout exclusion
    // from the missile path changed nothing at all, and shouts kept flying
    // along the BODY's facing — which in third person, with a side offset and
    // free-look pitch, is not where the player is looking. Hence "it still
    // doesn't let me shout where I'm looking".
    //
    // Cones get PARALLEL camera aim rather than the missiles' converge-on-the-
    // crosshair-point aim. A cone is a wide advancing wave, not a point that
    // has to arrive somewhere: pointing it exactly along the view is "shout
    // where I am looking", while bending it toward a raycast hit would swing
    // the whole wave sideways at close range for no visible gain.
    //
    // Flamethrower (Fire Breath) is deliberately NOT hooked — it is a sustained
    // stream anchored to the caster that the engine re-aims as the head turns,
    // so it already follows the view and rewriting its velocity would fight it.
    // ---- [CONEDIAG] cone projectile LIFETIME tracking --------------------
    //
    // The one thing that decides the "shouts fire sideways" report, and the
    // thing no previous log captured. The aim write itself is already known
    // GOOD — a 21:52 capture has the hook firing with dir=(-0.352,-0.285,
    // 0.892), i.e. 63 degrees up, and pitch/yaw encoding exactly that. So
    // the write is correct and the question is entirely what happens AFTER:
    // does the engine keep our direction, or re-derive its own (which for a
    // sheathed player whose body is not facing the camera would send the
    // cone off along the BODY axis — precisely "sideways")?
    //
    // Tracks each player cone for its first several ticks and prints, per
    // tick: the projectile's stored angles, its velocity direction, and —
    // decisively — the direction it ACTUALLY MOVED since the previous tick.
    // Alongside it, the player's body yaw and the camera yaw, so "it flew
    // along the body instead of the camera" is a direct read rather than an
    // inference.
    struct ConeTrack
    {
        const RE::Projectile* proj = nullptr;
        RE::NiPoint3          lastPos{};
        int                   ticks  = 0;
        bool                  active = false;
    };
    std::array<ConeTrack, 4> sConeTracks{};
    int  sConeDiagBudget = 400;

    void ConeDiagTick(RE::Projectile* a_proj, const char* a_phase)
    {
        if (sConeDiagBudget <= 0 || !a_proj) return;
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) return;

        // Only the "pre" phase advances the per-projectile clock and the
        // position baseline. The other two phases are extra views of the
        // SAME engine tick, so counting them would burn the tick budget 3x
        // and would report a zero movement delta for two of every three
        // lines (nothing moves between our write and the engine's call).
        const bool advance = (std::strcmp(a_phase, "pre") == 0);

        ConeTrack* slot = nullptr;
        for (auto& t : sConeTracks) {
            if (t.active && t.proj == a_proj) { slot = &t; break; }
        }
        if (!slot) {
            if (!advance) return;   // never seed a slot from a post-phase
            for (auto& t : sConeTracks) {
                if (!t.active) {
                    t = ConeTrack{ a_proj, a_proj->GetPosition(), 0, true };
                    slot = &t;
                    break;
                }
            }
            if (!slot) return;   // all slots busy; drop rather than evict
        }
        if (slot->ticks > 10) return;   // first ~10 ticks tell the whole story
        if (advance) ++slot->ticks;
        --sConeDiagBudget;

        auto& rt = a_proj->GetProjectileRuntimeData();
        const RE::NiPoint3 pos = a_proj->GetPosition();
        const RE::NiPoint3 moved{ pos.x - slot->lastPos.x,
                                  pos.y - slot->lastPos.y,
                                  pos.z - slot->lastPos.z };
        if (advance) slot->lastPos = pos;
        const float movedLen = std::sqrt(moved.x * moved.x + moved.y * moved.y +
                                         moved.z * moved.z);
        // Direction actually travelled, as yaw/pitch, so it can be compared
        // to the camera and body yaw printed beside it in the same units.
        float movedYaw = 0.0f, movedPitch = 0.0f;
        if (movedLen > 0.001f) {
            movedYaw   = std::atan2(moved.x, moved.y);
            movedPitch = std::atan2(-moved.z, std::sqrt(moved.x * moved.x + moved.y * moved.y));
        }
        const float velLen = std::sqrt(rt.velocity.x * rt.velocity.x +
                                       rt.velocity.y * rt.velocity.y +
                                       rt.velocity.z * rt.velocity.z);
        float velYaw = 0.0f, velPitch = 0.0f;
        if (velLen > 0.001f) {
            velYaw   = std::atan2(rt.velocity.x, rt.velocity.y);
            velPitch = std::atan2(-rt.velocity.z,
                                  std::sqrt(rt.velocity.x * rt.velocity.x +
                                            rt.velocity.y * rt.velocity.y));
        }
        // Camera yaw for comparison against body yaw — the whole question is
        // which of the two the cone ends up following.
        float camYaw = 0.0f;
        if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
            if (auto* nd = pc->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* nc = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                    const auto& cmv = nc->world.rotate;
                    camYaw = std::atan2(cmv.entry[0][0], cmv.entry[1][0]);
                }
            }
        }
        // The VISUAL's actual orientation, which is the one thing separating
        // the two remaining explanations. The hitbox provably travels where
        // we aim it (MOVED matches our written angles exactly), so if the
        // player still sees the effect going the wrong way, the mesh is not
        // following data.angle — the engine builds a projectile's 3D
        // orientation when it spawns, and a cone spawns LEVEL (p=-0.132)
        // before we rewrite it to point up. That would show here as a node
        // rotation that stays level while MOVED climbs, and it is worst
        // exactly when aiming up (largest gap between spawn and written
        // angle) — which is the reported condition. Logged for both axis
        // conventions since object meshes and cameras disagree on "forward".
        float n3dYawX = 0.0f, n3dYawY = 0.0f, n3dPitchY = 0.0f;
        bool  have3d  = false;
        if (auto* obj3d = a_proj->Get3D()) {
            const auto& rm = obj3d->world.rotate;
            n3dYawX = std::atan2(rm.entry[0][0], rm.entry[1][0]);
            const RE::NiPoint3 yAxis{ rm.entry[0][1], rm.entry[1][1], rm.entry[2][1] };
            n3dYawY   = std::atan2(yAxis.x, yAxis.y);
            n3dPitchY = std::atan2(-yAxis.z,
                                   std::sqrt(yAxis.x * yAxis.x + yAxis.y * yAxis.y));
            have3d = true;
        }
        const char* projName = "?";
        if (auto* base = a_proj->GetBaseObject()) {
            if (const char* n = base->GetName(); n && n[0]) projName = n;
        }
        float baseSpeed = -1.0f;
        if (auto* base = a_proj->GetBaseObject()) {
            if (const auto* pf = base->As<RE::BGSProjectile>()) baseSpeed = pf->data.speed;
        }
        spdlog::debug(
            "[CONEDIAG] {} t{} \"{}\" firstTick={} | projAng(p={:.3f} y={:.3f}) "
            "vel(len={:.1f} p={:.3f} y={:.3f}) | MOVED(len={:.2f} p={:.3f} y={:.3f}) | "
            "3d={} nodeYawX={:.3f} nodeYawY={:.3f} nodePitchY={:.3f} | "
            "bodyYaw={:.3f} camYaw={:.3f} bodyVsCam={:.3f} "
            "baseSpeed={:.0f} speedMult={:.2f} weaponDrawn={}",
            a_phase, slot->ticks, projName,
            (((rt.flags.underlying() >> 0x1F) & 1u) == 0u) ? 1 : 0,
            a_proj->data.angle.x, a_proj->data.angle.z,
            velLen, velPitch, velYaw,
            movedLen, movedPitch, movedYaw,
            have3d ? 1 : 0, n3dYawX, n3dYawY, n3dPitchY,
            ply->data.angle.z, camYaw,
            // Wrapped body-vs-camera difference. Both logged samples had the
            // body already facing the camera, so the capture could not have
            // shown a mismatch even if one mattered — this makes the
            // divergence a single readable number instead of something that
            // has to be worked out by hand across a 2*pi wrap.
            [&] {
                float d = ply->data.angle.z - camYaw;
                while (d >  3.14159265f) d -= 6.28318530f;
                while (d < -3.14159265f) d += 6.28318530f;
                return d;
            }(),
            baseSpeed, rt.speedMult,
            (ply->AsActorState() && ply->AsActorState()->IsWeaponDrawn()) ? 1 : 0);
    }

    void HookedConeUpdateImpl(RE::Projectile* a_proj, float a_delta)
    {
        if (!_originalConeUpdateImpl) return;
        if (!a_proj) { _originalConeUpdateImpl(a_proj, a_delta); return; }

        if (!DietDrCamera::SettingsManager::GetSingleton().spellTracingEnabled) {
            _originalConeUpdateImpl(a_proj, a_delta);
            return;
        }
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) { _originalConeUpdateImpl(a_proj, a_delta); return; }

        auto& runtime = a_proj->GetProjectileRuntimeData();
        auto  handlePtr = runtime.shooter.get();
        if (!handlePtr || handlePtr.get() != ply) {
            _originalConeUpdateImpl(a_proj, a_delta);
            return;
        }
        // Observe EVERY tick of a player cone, including the ones we hand
        // straight to the engine below — the post-first-tick ticks are
        // exactly where "did the engine keep our aim" gets answered, and
        // they are the ones the old one-shot log never saw.
        ConeDiagTick(a_proj, "pre");
        // Only the first tick after spawn; after that the engine integrates.
        if (((runtime.flags.underlying() >> 0x1F) & 1u) != 0u) {
            _originalConeUpdateImpl(a_proj, a_delta);
            return;
        }
        // Third person only — 1p already points where you look.
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->currentState) { _originalConeUpdateImpl(a_proj, a_delta); return; }
        const auto stateId = cam->currentState->id;
        if (stateId != RE::CameraState::kThirdPerson &&
            stateId != RE::CameraState::kMount &&
            stateId != RE::CameraState::kDragon) {
            _originalConeUpdateImpl(a_proj, a_delta);
            return;
        }

        // Camera forward: the release-time capture when one is fresh (a shout
        // spawns its projectile several frames after the voice fires, so the
        // live matrix has already drifted), else the live NiCamera.
        RE::NiPoint3 forward{}, up{}, camPos{}, target{};
        bool haveCam = DietDrCamera::MissileProjectileDetour::ReadReleaseCapture(
            forward, up, camPos, target);
        if (!haveCam && cam->cameraRoot) {
            if (auto* asNode = cam->cameraRoot->AsNode()) {
                for (auto& child : asNode->GetChildren()) {
                    if (auto* nc = child ? skyrim_cast<RE::NiCamera*>(child.get()) : nullptr) {
                        // Shouts always land here (they never populate the
                        // release capture above), so this is the ONLY basis
                        // a cone ever aims from. It has to be the CLEAN one:
                        // the rendered matrix carries this frame's camera
                        // noise / Repulse kick — the same reason the spell
                        // aim path strips it (see GetAppliedCameraOffset3p's
                        // comment: "the post-shot Repulse pitch kick was
                        // bending every aim read... trouble with verticals
                        // from repulse"). A shout arms its OWN Repulse kick
                        // on release, so without this a shout's cone could
                        // capture its own kick. Near-vertical aim is the
                        // worst case: dir.x/dir.y both shrink toward the
                        // aim ray, so atan2(dir.x,dir.y) below amplifies any
                        // small off-axis kick into a large yaw swing —
                        // "shooting sideways when aiming upward" is exactly
                        // that amplification, not a separate bug.
                        RE::NiMatrix3 cm = nc->world.rotate;
                        RE::NiMatrix3 nRot;
                        RE::NiPoint3  nTr;
                        if (DietDrCamera::CameraNoiseController::GetAppliedCameraOffset3p(nRot, nTr)) {
                            cm = MulByTranspose(cm, nRot);
                        }
                        forward = RE::NiPoint3{ cm.entry[0][0], cm.entry[1][0], cm.entry[2][0] };
                        haveCam = true;
                        break;
                    }
                }
            }
        }
        if (!haveCam) { _originalConeUpdateImpl(a_proj, a_delta); return; }

        const float flen = std::sqrt(forward.x * forward.x + forward.y * forward.y +
                                     forward.z * forward.z);
        if (flen < 0.001f) { _originalConeUpdateImpl(a_proj, a_delta); return; }
        const RE::NiPoint3 dir{ forward.x / flen, forward.y / flen, forward.z / flen };

        const float fz    = std::clamp(dir.z, -1.0f, 1.0f);
        a_proj->data.angle.x = -std::asin(fz);
        a_proj->data.angle.z = std::atan2(dir.x, dir.y);

        if (const auto* projForm = a_proj->GetBaseObject()
                ? a_proj->GetBaseObject()->As<RE::BGSProjectile>() : nullptr) {
            const float velScalar = projForm->data.speed * runtime.speedMult;
            runtime.velocity.x       = dir.x * velScalar;
            runtime.velocity.y       = dir.y * velScalar;
            runtime.velocity.z       = dir.z * velScalar;
            runtime.linearVelocity.x = dir.x * velScalar;
            runtime.linearVelocity.y = dir.y * velScalar;
            runtime.linearVelocity.z = dir.z * velScalar;
        }
        // Capped per-shout log (was one-shot ever, which is useless for
        // diagnosing a report like "shouts go sideways aiming up"). Logs
        // enough to tell whether the strip above actually engaged and
        // whether the resulting direction is plausible.
        static int sConeLogs = 0;
        if (sConeLogs < 40) {
            ++sConeLogs;
            RE::NiMatrix3 dbgRot;
            RE::NiPoint3  dbgTr;
            const bool kicked = DietDrCamera::CameraNoiseController::GetAppliedCameraOffset3p(dbgRot, dbgTr);
            const bool weaponDrawn = ply->AsActorState() && ply->AsActorState()->IsWeaponDrawn();
            spdlog::debug("[SpellTrace] cone aimed pitch={:.3f} yaw={:.3f} dir=({:.3f},{:.3f},{:.3f}) "
                         "weaponDrawn={} kickStripped={}",
                         a_proj->data.angle.x, a_proj->data.angle.z,
                         dir.x, dir.y, dir.z, weaponDrawn, kicked);
        }
        // Turn the VISUAL to match the trajectory — by writing the node
        // matrix directly, NOT via Update3DPosition.
        //
        // Update3DPosition was tried and produced a provably wrong
        // orientation: the 16:25 [CONEDIAG] capture decodes its result as
        // Rx(-p)∘Rz(y) — yaw applied first, then pitch about the WORLD X
        // axis instead of the local right axis (for aim p=-0.845 y=-2.232 it
        // predicts nodeYawY=-2.045 nodePitchY=+0.477; the log measured
        // -2.047/+0.477). So the mesh pointed 27° DOWN and 10° off-yaw while
        // the hitbox climbed at 48° — worse than the level spawn pose it was
        // meant to fix. The engine's own spawn-time build DOES use the
        // convention below (spawn node decoded exactly to the spawn angles),
        // so compose Rz(yaw)·Rx(pitch) ourselves and hand it to the node.
        // Columns are (right, forward, up); forward = (sy·cp, cy·cp, −sp)
        // matches `dir` by construction of angle.x/z above.
        if (auto* proj3d = a_proj->Get3D()) {
            const float p  = a_proj->data.angle.x;
            const float y  = a_proj->data.angle.z;
            const float cp = std::cos(p), sp = std::sin(p);
            const float cy = std::cos(y), sy = std::sin(y);
            RE::NiMatrix3 rot;
            rot.entry[0][0] = cy;   rot.entry[0][1] = sy * cp;  rot.entry[0][2] = sy * sp;
            rot.entry[1][0] = -sy;  rot.entry[1][1] = cy * cp;  rot.entry[1][2] = cy * sp;
            rot.entry[2][0] = 0.0f; rot.entry[2][1] = -sp;      rot.entry[2][2] = cp;
            proj3d->local.rotate = rot;
            RE::NiUpdateData updateData;
            proj3d->Update(updateData);
        }

        // Straight after our write, and again straight after the engine's own
        // update of the same projectile in the same call. If "postEngine"
        // shows a different velocity/angle than "postWrite", the engine is
        // overwriting our aim and the first-tick-only strategy is wrong for
        // cones.
        ConeDiagTick(a_proj, "postWrite");
        _originalConeUpdateImpl(a_proj, a_delta);
        ConeDiagTick(a_proj, "postEngine");
    }

    void MissileProjectileDetour::Install()
    {
        if (sInstalled) return;

        // SmoothCam patches projectile flight via MS Detours on the
        // engine flight-path function — different mechanism than our
        // per-vtable hook, but its semantics for spells overlap with
        // ours. Defer to it if loaded.
        if (GetModuleHandleA("SmoothCam.dll")) {
            spdlog::info("[MissileProjectileDetour] SmoothCam.dll detected — skipping vtable patch");
            sInstalled = true;
            return;
        }

        sInstalled = true;

        // Patch ONLY VTABLE_MissileProjectile[0] slot 0xAB.
        // ArrowProjectile / FlameProjectile / BeamProjectile each have
        // their own vtables and are unaffected. Slot 0xAB is the
        // inherited Projectile::UpdateImpl override.
        REL::Relocation<std::uintptr_t> missileVtbl{ RE::VTABLE_MissileProjectile[0] };
        _originalUpdateImpl = reinterpret_cast<UpdateImplFn>(
            missileVtbl.write_vfunc(0xAB, &HookedUpdateImpl));
        spdlog::info("[MissileProjectileDetour] vtable patched at 0x{:x} slot 0xAB",
                     missileVtbl.address());

        // Cone projectiles — the family every shout actually uses.
        REL::Relocation<std::uintptr_t> coneVtbl{ RE::VTABLE_ConeProjectile[0] };
        _originalConeUpdateImpl = reinterpret_cast<UpdateImplFn>(
            coneVtbl.write_vfunc(0xAB, &HookedConeUpdateImpl));
        spdlog::info("[MissileProjectileDetour] cone vtable patched at 0x{:x} slot 0xAB",
                     coneVtbl.address());
    }
}
