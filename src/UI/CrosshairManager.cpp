#include "PCH.h"
#define NOMINMAX
#include "UI/CrosshairManager.h"
#include "Camera/CameraNoiseController.h"
#include "Camera/SpellTrajectory.h"
#include "Camera/RayHitFilter.h"
#include "Hooks/ArrowPathDetour.h"
#include "Hooks/MissileProjectileDetour.h"
#include "LockOn/TDMIntegration.h"
#include "Settings/SettingsManager.h"
#include "Camera/StateResolver.h"
#include "UI/SKSEMenuFramework.h"
#include "RE/H/hkpClosestRayHitCollector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <toml++/toml.hpp>

namespace DietDrCamera
{
    namespace
    {
        // Release-anchor calibration cache (see CrosshairManager.h).
        constexpr auto kTraceCalibPath = "Data/SKSE/Plugins/DietDrCamera/TraceCalibration.toml";

        double AnchorNowSeconds()
        {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        // Reject self/projectile hits in the collector, without advancing the
        // ray past them. Advancing could skip a nearby wall or exhaust the
        // retry budget inside a projectile and report an obstructed ray clear.
        struct IgnoreAimHit
        {
            RE::TESObjectREFR* shooter{};

            bool operator()(const RE::hkpCdBody& body) const
            {
                const auto* root = &body;
                while (root->parent) root = root->parent;
                const auto* collidable = static_cast<const RE::hkpCollidable*>(root);
                const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*collidable);
                return ref && (ref == shooter || ref->As<RE::Projectile>());
            }
        };

        bool AimRayCastSkipProjectiles(RE::bhkWorld* a_bhkW,
                                       const RE::NiPoint3& a_from,
                                       const RE::NiPoint3& a_to,
                                       float& a_outFraction, bool skipShooter = false,
                                       std::uint32_t filterInfo = static_cast<std::uint32_t>(RE::COL_LAYER::kCameraSphere))
        {
            auto* hkW = a_bhkW ? a_bhkW->GetWorld1() : nullptr;
            if (!hkW) return false;
            const float ws = RE::bhkWorld::GetWorldScale();
            RayHitFilter<RE::hkpClosestRayHitCollector, RE::hkpCdBody,
                RE::hkpShapeRayCastCollectorOutput, IgnoreAimHit> collector(
                    IgnoreAimHit{skipShooter ? RE::PlayerCharacter::GetSingleton() : nullptr});
            RE::bhkPickData pick{};
            pick.rayInput.from.quad = _mm_setr_ps(a_from.x*ws, a_from.y*ws, a_from.z*ws, 0);
            pick.rayInput.to.quad = _mm_setr_ps(a_to.x*ws, a_to.y*ws, a_to.z*ws, 0);
            pick.rayInput.filterInfo.filter = filterInfo;
            pick.rayInput.enableShapeCollectionFilter = true;
            pick.ray.quad = _mm_setzero_ps();
            pick.closestRayHitCollector = &collector;
            a_bhkW->PickObject(pick);
            if (!collector.HasHit()) return false;
            a_outFraction = collector.rayHit.hitFraction;
            return true;
        }

        SpellTrajectory::Path<RE::NiPoint3> TraceSpellPath(RE::TESObjectCELL* cell,
            RE::NiPoint3 origin, RE::NiPoint3 velocity, RE::NiPoint3 acceleration, float range, float integrationStep,
            std::uint32_t collisionFilter)
        {
            auto* world = cell ? cell->GetbhkWorld() : nullptr;
            if (!world || !world->GetWorld1()) return {};
            return SpellTrajectory::Trace(origin, velocity, acceleration, range,
                [world, collisionFilter](RE::NiPoint3 from, RE::NiPoint3 to, float& fraction) {
                    return AimRayCastSkipProjectiles(world, from, to, fraction, true, collisionFilter);
                }, integrationStep);
        }

        RE::BGSProjectile* ChargingProjectile(RE::MagicCaster* caster)
        {
            auto* spell = caster ? caster->currentSpell : nullptr;
            if (!spell) return nullptr;
            const auto projectile = [](RE::Effect* effect) -> RE::BGSProjectile* {
                return effect && effect->baseEffect && effect->baseEffect->IsHostile()
                    ? effect->baseEffect->data.projectileBase : nullptr;
            };
            if (auto* base = projectile(spell->GetCostliestEffectItem())) return base;
            for (auto* effect : spell->effects)
                if (auto* base = projectile(effect)) return base;
            return nullptr;
        }

        // rendered = clean * applied  =>  clean = rendered * appliedᵀ.
        // (Rotation inverse = transpose.) Used to strip the noise/Repulse
        // offsets off camera reads that must match the ENGINE's aim.
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
    }
}

namespace
{
    // SmoothCam raycast layer mask. Verbatim from SmoothCam (raycast.cpp);
    // SmoothCam itself flags this as TODO. Mirroring guarantees identical
    // hit behavior when both mods are present.
    constexpr std::uint32_t kPickFilterInfo = 0x40122716;

    // Layout of PlayerCharacter::unkBA0 — verbatim from SmoothCam's
    // game_state.h (SSA<UnkBowDrawnTimerEntry, 2>). The engine pushes
    // a per-draw-timer entry on this stack; the top entry's
    // `bowDrawTime` field is the LIVE 0..1 draw progress and updates
    // every frame while the bow string is being pulled. CommonLibSSE-NG
    // declares unkBA0 as BSTSmallArray<void*, 4> (wrong type for our
    // purpose), so we reinterpret the raw 0x30 bytes here.
    struct UnkBowDrawnTimerEntry
    {
        float bowDrawTime;
        float unk1, unk2, unk3;
    };
    struct UnkPlayerBA0
    {
        std::uint32_t         capacityAndLocal;
        std::uint32_t         pad04;
        union {
            void*                 heapPtr;
            UnkBowDrawnTimerEntry inlineEntries[2];
        } data;
        std::uint32_t         size;
        std::uint32_t         pad2C;
    };
    static_assert(sizeof(UnkPlayerBA0) == 0x30,
        "PlayerCharacter unkBA0 SSA layout must match SmoothCam's");

    // Returns the live bow draw amount in [0, 1]. Returns 0.0 when the
    // stack is empty — covers the sub-frame race where stringTaut goes
    // true before the engine pushes the new draw entry. The previous
    // 1.0 default (matching SmoothCam) made every chain-fire shot start
    // with a one-frame full-draw flash because PredictArrowImpact runs
    // on the first taut frame; defaulting to 0 means that frame renders
    // no trail instead.
    //
    // In modern CommonLibSSE-NG, unkBA0 lives inside PLAYER_RUNTIME_DATA
    // (accessed via GetPlayerRuntimeData()), NOT at PlayerCharacter+0xBA0.
    // SmoothCam's `&(player->unkBA0)` predates this refactor and reads
    // the wrong memory in our build.
    float GetLiveBowDrawAmountImpl(RE::PlayerCharacter* a_ply)
    {
        if (!a_ply) return 0.0f;
        const auto& runtime = a_ply->GetPlayerRuntimeData();
        const auto* arr = reinterpret_cast<const UnkPlayerBA0*>(
            static_cast<const void*>(&runtime.unkBA0));
        if (arr->size == 0 || arr->size > 32) return 0.0f;
        const std::uint32_t topIdx = arr->size - 1u;

        // Heap vs inline. Bit 31 of capacityAndLocal is the local flag
        // (1 = inline, 0 = heap). After multiple draw/cancel cycles
        // the stack grows past 2 entries and the engine reallocates
        // to heap; reading inline_ at that point returns the heap
        // pointer's bytes interpreted as floats — garbage.
        const bool isLocal = (arr->capacityAndLocal & 0x80000000u) != 0u;
        const UnkBowDrawnTimerEntry* entries = nullptr;
        if (isLocal) {
            if (topIdx >= 2) return 0.0f;  // inline holds at most 2
            entries = arr->data.inlineEntries;
        } else {
            const auto* heap = static_cast<const UnkBowDrawnTimerEntry*>(arr->data.heapPtr);
            if (!heap) return 0.0f;
            entries = heap;
        }
        const float val = entries[topIdx].bowDrawTime;
        if (!std::isfinite(val) || val < 0.0f) return 0.0f;
        return val < 1.0f ? val : 1.0f;
    }

    // Diagnostic helper: returns raw bowDrawTime (no clamp) and stack
    // size. Caller logs them per frame so we can see the actual ramp
    // shape during a tap-fire vs a full draw.
    struct LiveBowDrawSnapshot { float raw = 0.0f; std::uint32_t size = 0; bool valid = false; };
    LiveBowDrawSnapshot GetLiveBowDrawSnapshot(RE::PlayerCharacter* a_ply)
    {
        LiveBowDrawSnapshot s;
        if (!a_ply) return s;
        const auto& runtime = a_ply->GetPlayerRuntimeData();
        const auto* arr = reinterpret_cast<const UnkPlayerBA0*>(
            static_cast<const void*>(&runtime.unkBA0));
        s.size = arr->size;
        if (arr->size == 0 || arr->size > 32) return s;
        const std::uint32_t topIdx = arr->size - 1u;
        const bool isLocal = (arr->capacityAndLocal & 0x80000000u) != 0u;
        const UnkBowDrawnTimerEntry* entries = nullptr;
        if (isLocal) {
            if (topIdx >= 2) return s;
            entries = arr->data.inlineEntries;
        } else {
            entries = static_cast<const UnkBowDrawnTimerEntry*>(arr->data.heapPtr);
            if (!entries) return s;
        }
        s.raw = entries[topIdx].bowDrawTime;
        s.valid = true;
        return s;
    }

    // Diagnostic: dump raw bytes around the runtime-data unkBA0 (the
    // REAL offset, accessed through GetPlayerRuntimeData()). Confirms
    // the SSA layout once we're reading the right memory.
    void LogBowTimerRawBytes(RE::PlayerCharacter* a_ply, std::uint64_t a_frame)
    {
        if (!a_ply) return;
        if ((a_frame % 30u) != 0u) return;
        const auto& runtime = a_ply->GetPlayerRuntimeData();
        const auto* unkBA0Ptr = reinterpret_cast<const std::uint8_t*>(
            static_cast<const void*>(&runtime.unkBA0));
        const auto baseAddr = reinterpret_cast<std::uintptr_t>(a_ply);
        const auto unkAddr = reinterpret_cast<std::uintptr_t>(unkBA0Ptr);
        const auto* w = reinterpret_cast<const std::uint32_t*>(unkBA0Ptr);
        const auto* f = reinterpret_cast<const float*>(unkBA0Ptr);

        spdlog::info(
            "[BowDump] real-offset={:#x} (was reading PlayerChar+0xBA0)",
            unkAddr - baseAddr);
        spdlog::info(
            "[BowDump] +00 {:08X} {:08X} {:08X} {:08X}  "
            "+10 {:08X} {:08X} {:08X} {:08X}  "
            "+20 {:08X} {:08X} {:08X} {:08X}",
            w[0], w[1], w[2], w[3],
            w[4], w[5], w[6], w[7],
            w[8], w[9], w[10], w[11]);
        spdlog::info(
            "[BowDump] floats: +00={:.3f} +04={:.3f} +08={:.3f} +0C={:.3f} "
            "+10={:.3f} +14={:.3f} +18={:.3f} +1C={:.3f}",
            f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }

    // Crosshair-scaling distance band. Linear lerp from kCloseSize at
    // kCloseDist to kFarSize at kFarDist (clamped beyond either end).
    constexpr float  kCrosshairCloseDist = 256.0f;
    constexpr float  kCrosshairFarDist   = 4096.0f;
    constexpr double kCrosshairCloseSize = 1.0;
    constexpr double kCrosshairFarSize   = 0.4;

    // Maximum forward ray length (game units). Past this nothing is hit and
    // the crosshair drops to the "miss" state.
    constexpr float kRayLen = 8000.0f;

    // Minimal SmoothCam V1 interface — vtable-compatible with the real one
    // declared in research_acc/.../api/SmoothCamAPI.h. We only need the
    // GetCrosshairOwner slot. Using a slim local declaration avoids
    // dragging SmoothCam's PCH defines into the rest of the project.
    class IVSmoothCam1Slim
    {
    public:
        virtual ~IVSmoothCam1Slim()                                                                = default;
        virtual unsigned long             GetSmoothCamThreadId() const noexcept                    = 0;
        virtual std::uint8_t              RequestCameraControl(SKSE::PluginHandle h) noexcept      = 0;
        virtual std::uint8_t              RequestCrosshairControl(SKSE::PluginHandle h, bool r)    = 0;
        virtual std::uint8_t              RequestStealthMeterControl(SKSE::PluginHandle h, bool r) = 0;
        virtual SKSE::PluginHandle        GetCameraOwner() const noexcept                         = 0;
        virtual SKSE::PluginHandle        GetCrosshairOwner() const noexcept                      = 0;
        virtual SKSE::PluginHandle        GetStealthMeterOwner() const noexcept                   = 0;
        virtual std::uint8_t              ReleaseCameraControl(SKSE::PluginHandle h) noexcept     = 0;
        virtual std::uint8_t              ReleaseCrosshairControl(SKSE::PluginHandle h) noexcept  = 0;
        virtual std::uint8_t              ReleaseStealthMeterControl(SKSE::PluginHandle h) noexcept = 0;
    };

    constexpr std::uint32_t kSmoothCamCmdHeader      = 0x9007CA50;
    constexpr std::uint8_t  kSmoothCamCmdRequestIfc  = 0;
    constexpr std::uint8_t  kSmoothCamRespIfcProvider = 1;
    constexpr std::uint8_t  kSmoothCamIfcVersionV1    = 0;
    constexpr std::uint8_t  kSmoothCamIfcVersionV3    = 2;

    struct SmoothCamPluginCommand
    {
        std::uint32_t header = kSmoothCamCmdHeader;
        std::uint8_t  type;
        void*         commandStructure = nullptr;
    };

    struct SmoothCamInterfaceRequest
    {
        std::uint8_t interfaceVersion;
    };

    struct SmoothCamPluginResponse
    {
        std::uint8_t type;
        void*        responseData = nullptr;
    };

    struct SmoothCamInterfaceContainer
    {
        void*         interfaceInstance = nullptr;
        std::uint8_t  interfaceVersion;
    };
}

namespace DietDrCamera
{
    // Public wrapper around the file-scope helper above, so other
    // translation units (CameraNoiseController) can read live bow draw.
    float GetLiveBowDrawAmount(RE::PlayerCharacter* a_ply)
    {
        return ::GetLiveBowDrawAmountImpl(a_ply);
    }

    CrosshairManager& CrosshairManager::GetSingleton()
    {
        static CrosshairManager instance;
        return instance;
    }

    void CrosshairManager::Init()
    {
        // Send the SmoothCam interface request and register a listener. If
        // SmoothCam isn't loaded the dispatch returns false / no response,
        // and smoothCamIface_ stays null — the rest of the code falls back
        // to "no SmoothCam" path.
        auto* messaging = SKSE::GetMessagingInterface();
        if (messaging && GetModuleHandleA("SmoothCam.dll")) {
            messaging->RegisterListener("SmoothCam", [](SKSE::MessagingInterface::Message* msg) {
                if (!msg || msg->type != 0) return;
                if (msg->dataLen != sizeof(SmoothCamPluginResponse)) return;
                auto* resp = reinterpret_cast<SmoothCamPluginResponse*>(msg->data);
                if (!resp || resp->type != kSmoothCamRespIfcProvider) return;
                auto* container = reinterpret_cast<SmoothCamInterfaceContainer*>(resp->responseData);
                if (!container || !container->interfaceInstance) return;
                CrosshairManager::GetSingleton().OnSmoothCamInterface(
                    container->interfaceInstance, container->interfaceVersion);
            });

            SmoothCamInterfaceRequest req{};
            req.interfaceVersion = kSmoothCamIfcVersionV3;
            SmoothCamPluginCommand cmd{};
            cmd.type             = kSmoothCamCmdRequestIfc;
            cmd.commandStructure = &req;
            (void)messaging->Dispatch(0, &cmd, sizeof(cmd), "SmoothCam");
            spdlog::info("[CrosshairManager] SmoothCam interface requested");
        }

        // Note: we cannot capture the HUD baseline here — the HUD menu
        // isn't typically loaded yet. EnsureBaseline() is called from
        // Tick() and stores the values on the first frame they're valid.

        // Register a HudElement callback so the predicted-trajectory
        // line gets drawn each frame on top of the HUD overlay. The
        // trampolined ImGui foreground draw list is the cleanest way
        // to render world-space lines without writing a D3D11 hook.
        if (!hudElementRegistered_) {
            SKSEMenuFramework::AddHudElement([]() {
                CrosshairManager::GetSingleton().RenderTrajectoryHud();
            });
            hudElementRegistered_ = true;
        }
    }

    void CrosshairManager::Shutdown()
    {
        RestoreCrosshair();
        RestoreStealthMeter();
    }

    void CrosshairManager::OnSmoothCamInterface(void* a_iface, std::uint8_t a_version)
    {
        smoothCamIface_      = a_iface;
        ownershipCheckDirty_ = true;
        spdlog::info("[CrosshairManager] SmoothCam interface received (v{})",
                     static_cast<int>(a_version));
    }

    bool CrosshairManager::SmoothCamOwnsCrosshair()
    {
        if (!smoothCamIface_) return false;
        // V1 layout exposes GetCrosshairOwner; all later versions inherit it.
        auto* api    = static_cast<IVSmoothCam1Slim*>(smoothCamIface_);
        auto  owner  = api->GetCrosshairOwner();
        const auto self = SKSE::GetPluginHandle();
        if (owner == SKSE::kInvalidPluginHandle) return false;
        if (owner == self)                       return false;
        return true;
    }

    bool CrosshairManager::ResolveCameraNi(RE::NiCamera*& outNiCam) const
    {
        outNiCam       = nullptr;
        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (!playerCam) return false;
        auto* root = playerCam->cameraRoot.get();
        if (!root) return false;
        auto* asNode = root->AsNode();
        if (!asNode || asNode->GetChildren().empty()) return false;
        auto* child = asNode->GetChildren()[0].get();
        if (!child) return false;
        outNiCam = netimmerse_cast<RE::NiCamera*>(child);
        return outNiCam != nullptr;
    }

    bool CrosshairManager::EnsureBaseline()
    {
        if (baselineCaptured_) return true;  // already attempted (success or skin-mod-invalid)

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hud) return false;
        auto* hudMenu = hud.get();
        if (!hudMenu) return false;
        auto& runtime = static_cast<RE::HUDMenu*>(hudMenu)->GetRuntimeData();

        RE::GFxValue crosshair;
        if (!runtime.root.GetMember("Crosshair", &crosshair)) {
            spdlog::warn("[CrosshairManager] HUD root has no 'Crosshair' member yet");
            return false;
        }

        RE::GFxValue::DisplayInfo di;
        if (!crosshair.GetDisplayInfo(&di)) {
            spdlog::warn("[CrosshairManager] Crosshair GetDisplayInfo failed");
            return false;
        }

        base_.x = di.GetX();
        base_.y = di.GetY();

        // Baseline width/height come from member fields (DisplayInfo doesn't
        // carry them). Try _width/_height; if absent, leave 0 (we still get
        // valid x/y for centering).
        RE::GFxValue w, h;
        if (crosshair.GetMember("_width",  &w) && w.IsNumber()) base_.width  = w.GetNumber();
        if (crosshair.GetMember("_height", &h) && h.IsNumber()) base_.height = h.GetNumber();

        // The HUD swf does NOT initialize Crosshair at _xscale=100 — it's
        // typically lower (Skyrim's reticle is small by design). Capturing
        // the swf's startup scale lets us treat that as our 100% reference
        // so writing scale=baseline preserves the vanilla look.
        RE::GFxValue xs, ys;
        if (crosshair.GetMember("_xscale", &xs) && xs.IsNumber()) base_.xScale = xs.GetNumber();
        if (crosshair.GetMember("_yscale", &ys) && ys.IsNumber()) base_.yScale = ys.GetNumber();

        // Capture StealthMeterInstance baseline _x/_y via the SWF
        // GetVariable path — same approach SmoothCam uses
        // (`crosshair.cpp:465-469`). DisplayInfo doesn't expose the
        // member from the parent root walk, so this is the cleanest
        // read. We only relocate when the read succeeds; otherwise
        // a HUD skin mod with a custom layout keeps vanilla behavior.
        if (auto* hudMovie = static_cast<RE::HUDMenu*>(hudMenu)->uiMovie.get()) {
            RE::GFxValue va;
            if (hudMovie->GetVariable(&va, "_root.HUDMovieBaseInstance.StealthMeterInstance._x") &&
                va.IsNumber())
            {
                base_.stealthX = va.GetNumber();
                if (hudMovie->GetVariable(&va, "_root.HUDMovieBaseInstance.StealthMeterInstance._y") &&
                    va.IsNumber())
                {
                    base_.stealthY     = va.GetNumber();
                    base_.stealthValid = std::isfinite(base_.stealthX) &&
                                         std::isfinite(base_.stealthY);
                }
            }
        }

        const bool numericValid = std::isfinite(base_.x) && std::isfinite(base_.y) &&
                                  base_.x != 0.0 && base_.y != 0.0;
        base_.valid       = numericValid;
        baselineCaptured_ = true;

        // Resolution diagnostic. One-time on first valid HUD tick.
        const auto* state = RE::BSGraphics::State::GetSingleton();
        const int   sw    = state ? static_cast<int>(state->screenWidth)  : 0;
        const int   sh    = state ? static_cast<int>(state->screenHeight) : 0;
        spdlog::info(
            "[CrosshairManager] init resolution={}x{} baseline x={:.1f} y={:.1f} w={:.1f} h={:.1f} xs={:.1f} ys={:.1f} stealthX={:.1f} stealthY={:.1f} stealthValid={} valid={} smoothcam_owned={}",
            sw, sh, base_.x, base_.y, base_.width, base_.height,
            base_.xScale, base_.yScale,
            base_.stealthX, base_.stealthY,
            base_.stealthValid ? 1 : 0,
            base_.valid ? 1 : 0, smoothCamOwns_ ? 1 : 0);
        return true;
    }

    void CrosshairManager::RestoreCrosshair()
    {
        if (!baselineCaptured_ || !base_.valid) {
            // Nothing reliable to restore to — drop our deduper so the
            // engine's next visibility write isn't dropped, but don't
            // force visible (engine knows when to show/hide based on
            // sneak/menu/etc.).
            haveLastWritten_ = false;
            return;
        }
        WriteCrosshairScreenPos(base_.x, base_.y);
        // Restore the swf's startup scale, NOT a hard-coded 100, otherwise
        // we'd enlarge a reticle the HUD initialized smaller.
        WriteCrosshairScaleXY(base_.xScale, base_.yScale);

        // Counter the prior `DisplayInfo.SetVisible(true)` that the
        // bow-active path writes every frame — that flag persists on
        // the GFx display object after we stop writing, and the
        // engine's vanilla hide path (used when sneaking with the
        // weapon sheathed) doesn't clear it. Explicitly write
        // visible=false so vanilla "no cursor while sneaking sheathed"
        // takes over; the engine fires its own SetVisible(true) when
        // it wants the cursor back (looking at an activatable, etc.),
        // so this isn't a permanent hide.
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME)) {
                auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
                if (hud) {
                    auto& runtime = hud->GetRuntimeData();
                    RE::GFxValue ch;
                    if (runtime.root.GetMember("Crosshair", &ch)) {
                        RE::GFxValue::DisplayInfo di;
                        di.SetVisible(false);
                        ch.SetDisplayInfo(di);
                    }
                }
            }
        }

        // Drop the visibility-write deduper so the engine's next
        // SetCrosshairEnabled call lands.
        haveLastWritten_ = false;
    }

    CrosshairManager::AimMode CrosshairManager::DetectAimMode()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return AimMode::None;

        // Per-frame game-time accumulation runs unconditionally, BEFORE
        // any mode-resolution returns. The bow path returns AimMode::Bow
        // early (when a bow is drawn) — without ticking here first, any
        // in-flight shot pushed via the renderer's eager-release-edge
        // would never age, freezing the trail + reticle on screen until
        // the user sheaths. Wall-clock delta * QGlobalTimeMultiplier so
        // Slow Time / Time Stop stretch shot timelines correctly.
        // Clamp to 0.2s to swallow long pauses without dumping a huge
        // delta on the first frame back.
        const float nowSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float gameDelta = 0.016f;
        if (lastTickWallSec_ > 0.0f) {
            const float wallDelta = std::clamp(nowSec - lastTickWallSec_, 0.0f, 0.2f);
            const float mul = RE::BSTimer::QGlobalTimeMultiplier();
            gameDelta = wallDelta * std::clamp(mul, 0.01f, 4.0f);
        }
        lastTickWallSec_ = nowSec;

        constexpr float kPostImpactSettle = 1.0f;
        {
            std::lock_guard<std::mutex> lock(firedShotsMutex_);
            for (auto& s : firedShots_) {
                s.elapsedGameTime += gameDelta;
            }
            firedShots_.erase(
                std::remove_if(firedShots_.begin(), firedShots_.end(),
                    [&](const ProjectileShot& s) {
                        return s.elapsedGameTime > (s.travelTime + kPostImpactSettle);
                    }),
                firedShots_.end());
        }

        // Bow / crossbow takes priority — same gate as before.
        auto* actorState = player->AsActorState();
        if (actorState && actorState->IsWeaponDrawn()) {
            auto* right = player->GetEquippedObject(false);
            if (right) {
                if (auto* weap = right->As<RE::TESObjectWEAP>()) {
                    const auto type = weap->GetWeaponType();
                    if (type == RE::WEAPON_TYPE::kBow ||
                        type == RE::WEAPON_TYPE::kCrossbow)
                    {
                        return AimMode::Bow;
                    }
                }
            }
        }

        // Magic: any magic-caster CHARGING a hostile fire-and-forget
        // missile spell (or matching staff enchantment). Excludes:
        //   - non-hostile spells (Soul Trap, Healing Hands, etc.)
        //   - non-FoF spells (Flames concentration stream)
        //   - non-aimed deliveries (self / bound weapon / conjure)
        //   - kCasting state — by then the spell has been released
        //     and the casting hand is dropping; live-tracking would
        //     drag the trail down to the moving hand.
        //   - kUnk07/08/09 (post-fire / interrupt / deselect)
        //
        // Each fired projectile gets its own entry in magicShots_,
        // captured at the detour's fire event. Shots live independently
        // until (fireTime + travelTime + kPostImpactSettle) elapses,
        // then they're pruned (above). Firing N spells in rapid
        // succession shows N simultaneous trails + cursors.
        const bool wantSpell = SettingsManager::GetSingleton().spellTracingEnabled;
        const bool wantArrow = SettingsManager::GetSingleton().archeryTracingEnabled;
        if (!wantSpell && !wantArrow) {
            magicWasCharging_ = false;
            DiscardPendingFireEvents(false);
            std::lock_guard<std::mutex> lock(firedShotsMutex_);
            firedShots_.clear();
            return AimMode::None;
        }
        // Bow tracing on, spell tracing off: the drain below is inside
        // `if (wantSpell)`, so the ring would grow unbounded and then flood
        // the moment spell tracing came back on. Same reasoning as the bails
        // in Tick — see DiscardPendingFireEvents.
        if (!wantSpell) DiscardPendingFireEvents(false);

        auto casterChargingHostileMissile = [](RE::MagicCaster* a_caster) -> bool {
            if (!a_caster) return false;
            using CState = RE::MagicCaster::State;
            const auto state = a_caster->state.get();
            // Only true charging states. kCasting onward = already
            // fired (handled by the detour's fire event, not by
            // live tracking).
            if (state == CState::kNone)    return false;
            if (state == CState::kCasting) return false;
            if (state == CState::kUnk07 || state == CState::kUnk08 || state == CState::kUnk09)
                return false;
            auto* magicItem = a_caster->currentSpell;
            if (!magicItem) return false;
            // Exclude shouts/powers. They cast hostile fire-and-forget
            // projectiles through the voice/instant caster but must never
            // draw the aim trace. This is the timing-independent half of the
            // shout exclusion: the StateResolver shout sub-state lags the
            // caster's charge by 1-2 frames, which showed as a split-second
            // of trace before the sub-state registered. Keying off the spell
            // type kills it at the source the instant the caster charges.
            switch (magicItem->GetSpellType()) {
            case RE::MagicSystem::SpellType::kPower:
            case RE::MagicSystem::SpellType::kLesserPower:
            case RE::MagicSystem::SpellType::kVoicePower:
                return false;
            default:
                break;
            }
            if (magicItem->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget)
                return false;
            for (auto* effect : magicItem->effects) {
                if (effect && effect->baseEffect && effect->baseEffect->IsHostile())
                    return true;
            }
            return false;
        };

        // Shouts cast hostile fire-and-forget projectiles through the voice/
        // instant caster, which would otherwise trip Magic mode and draw the
        // aim trace/reticle. The user wants shouts excluded — suppress Magic
        // mode while the shout sub-state is active. Gate on StateResolver
        // (the camera's authoritative shout signal, confirmed in the log)
        // because the word-of-power spell type isn't reliably Voice.
        // Follows the LIVE shout, not the framing hold: Shout Lag keeps the
        // sub-state alive for the camera, and inheriting it here would keep
        // the aim reticle suppressed for seconds after the shout is over.
        const bool shoutingNow =
            DietDrCamera::StateResolver::GetSingleton().GetSubState()
                    == DietDrCamera::CameraSubState::Shout &&
            !DietDrCamera::StateResolver::GetSingleton().IsShoutLagHolding();

        const bool magicNow = wantSpell && !shoutingNow && (
            casterChargingHostileMissile(player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand))  ||
            casterChargingHostileMissile(player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) ||
            casterChargingHostileMissile(player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther))     ||
            casterChargingHostileMissile(player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)));

        // Spell fire event drain. Multi-slot ring buffer so dual-cast
        // (both hands firing in the same engine tick) yields TWO events
        // — the prior single-slot publish lost one of them. Dedup
        // window: <50ms AND <50 units for the same projectile form and
        // casting source catches same-cast duplicates without conflating
        // separate hands or different projectile effects.
        if (wantSpell) {
            std::vector<MissileProjectileDetour::FireEvent> events;
            // Debounced calibration flush — one tiny file write a few quiet
            // seconds after the last learned fire, so the anchors survive
            // the session and every BOOT starts already calibrated.
            FlushReleaseAnchorsIfDue(AnchorNowSeconds());
            MissileProjectileDetour::DrainFireEvents(lastSpellFireVersion_, events);
            // Shots fired WHILE target locked must never become trails —
            // not while locked (the HUD bails there anyway) and not as a
            // replay after the lock releases (user report 2026-08-15). The
            // events are still drained (sequence advances) and still teach
            // the release anchors; only the trail/reticle creation is
            // skipped.
            const bool tlLockedFire = TDMIntegration::GetSingleton().IsTargetLocked();
            for (const auto& ev : events) {
                // Teach the release anchor with the projectile's TRUE spawn
                // position. The detour indexes hands 0=Right/1=Left; the
                // anchor store uses the RE CastingSource enum (0=Left,
                // 1=Right), so the hands swap on the way in.
                const int srcEnum = ev.castingSource == 0 ? 1 : ev.castingSource == 1 ? 0 : ev.castingSource;
                if (srcEnum >= 0) LearnReleaseAnchor(srcEnum, ev.startWorld);
                if (tlLockedFire) continue;
                bool dup = false;
                {
                    // Same-CAST dedup only. The 300ms window this used to
                    // scan guarded against the spell eager push — removed
                    // 2026-05-21 — so all it did lately was eat REAL shots:
                    // same-hand spam casting fires ~150ms+ apart from nearly
                    // the same spot ("shooting back to back extremely fast,
                    // only 1 projectile is traced"), and alternating dual-
                    // cast hands sit right at the 50u position borderline.
                    // The one duplicate source left is a multi-projectile
                    // spell publishing one event per projectile in the SAME
                    // tick. Compare PUBLISH times, not consumption times:
                    // two real casts drained in one Tick batch share a
                    // consumption time (zero apparent gap — the "still
                    // occasionally only 1" residue) but never a publish
                    // time; same-cast projectiles share both.
                    std::lock_guard<std::mutex> lock(firedShotsMutex_);
                    for (auto it = firedShots_.rbegin(); it != firedShots_.rend(); ++it) {
                        if (it->fireEventSec < -1.0e8) continue;   // non-spell producer
                        if ((ev.fireSec - it->fireEventSec) > 0.05) break;
                        if (it->worldPolyline.empty() || it->projectileFormID != ev.projectileFormID ||
                            it->castingSource != srcEnum) continue;
                        const auto& shotStart = it->worldPolyline.front();
                        const float ddx = ev.startWorld.x - shotStart.x;
                        const float ddy = ev.startWorld.y - shotStart.y;
                        const float ddz = ev.startWorld.z - shotStart.z;
                        if (ddx*ddx + ddy*ddy + ddz*ddz < 50.0f * 50.0f) {
                            dup = true;
                            break;
                        }
                    }
                }
                if (!dup) {
                    if (SpellTrajectory::Finite(ev.acceleration) && SpellTrajectory::Length(ev.acceleration) > 0.001f) {
                        auto path = TraceSpellPath(player->GetParentCell(), ev.startWorld,
                            ev.launchVelocity, ev.acceleration, ev.range, ev.integrationStep, ev.collisionFilter);
                        if (path.points.size() < 2) continue;
                        ProjectileShot shot;
                        shot.worldPolyline = std::move(path.points);
                        shot.travelTime = (std::max)(path.duration, 0.05f);
                        shot.wallClockFireTime = nowSec;
                        shot.fireEventSec = ev.fireSec;
                        shot.projectileFormID = ev.projectileFormID;
                        shot.castingSource = srcEnum;
                        // The actual shot is authoritative immediately. Blending
                        // from the charge pose draws a different, unchecked arc
                        // during precisely the frames the missile is leaving.
                        static unsigned traceLogCount = 0;
                        if (traceLogCount++ < 8) {
                            const auto& end = shot.worldPolyline.back();
                            spdlog::info("[SpellTrace] projectile={:08X} source={} filter={:08X} "
                                "origin=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f}) "
                                "accelZ={:.3f} step={:.5f} duration={:.4f} hit={} points={} end=({:.3f},{:.3f},{:.3f})",
                                ev.projectileFormID, srcEnum, ev.collisionFilter,
                                ev.startWorld.x, ev.startWorld.y, ev.startWorld.z,
                                ev.launchVelocity.x, ev.launchVelocity.y, ev.launchVelocity.z,
                                ev.acceleration.z, ev.integrationStep, path.duration, path.hit,
                                shot.worldPolyline.size(), end.x, end.y, end.z);
                        }
                        spdlog::debug("[SpellArc] fired projectile={:08X} speed={:.1f} accelerationZ={:.1f} travel={:.3f} hit={}",
                            ev.projectileFormID, SpellTrajectory::Length(ev.launchVelocity),
                            ev.acceleration.z, path.duration, path.hit);
                        std::lock_guard<std::mutex> lock(firedShotsMutex_);
                        firedShots_.push_back(std::move(shot));
                        if (firedShots_.size() > 16) firedShots_.erase(firedShots_.begin());
                        continue;
                    }
                    // Re-derive targetWorld via a hand-cast from spawn
                    // when the detour's camera-cast target ended up at
                    // or behind the spawn along camFwd (sneak self-hit
                    // case). Use ev.fireCamFwd (the camera direction
                    // CAPTURED AT FIRE TIME by the detour) — NOT a
                    // fresh read here. Reading camFwd now would track
                    // the user's spin between fire and consume and
                    // fire a "corrective" hand-cast that ends up
                    // pointing where the user is looking NOW, not
                    // where the spell actually went. The trace must
                    // align with the projectile's true direction.
                    RE::NiPoint3 correctedTarget = ev.targetWorld;
                    const RE::NiPoint3 fireCamFwd = ev.fireCamFwd;
                    const float fireCamFwdLen2 =
                        fireCamFwd.x*fireCamFwd.x +
                        fireCamFwd.y*fireCamFwd.y +
                        fireCamFwd.z*fireCamFwd.z;
                    if (fireCamFwdLen2 > 0.5f) {
                        const float fireFwdDist =
                            (ev.targetWorld.x - ev.startWorld.x) * fireCamFwd.x +
                            (ev.targetWorld.y - ev.startWorld.y) * fireCamFwd.y +
                            (ev.targetWorld.z - ev.startWorld.z) * fireCamFwd.z;
                        constexpr float kMinFwdDist = 500.0f;
                        if (fireFwdDist < kMinFwdDist) {
                            constexpr float kRayLen = 8000.0f;
                            correctedTarget = RE::NiPoint3{
                                ev.startWorld.x + fireCamFwd.x * kRayLen,
                                ev.startWorld.y + fireCamFwd.y * kRayLen,
                                ev.startWorld.z + fireCamFwd.z * kRayLen,
                            };
                            if (auto* cell = player->GetParentCell()) {
                                if (auto* bhkW = cell->GetbhkWorld()) {
                                    float frac = 1.0f;
                                    if (AimRayCastSkipProjectiles(bhkW, ev.startWorld, correctedTarget, frac)) {
                                        correctedTarget = RE::NiPoint3{
                                            ev.startWorld.x + fireCamFwd.x * (kRayLen * frac),
                                            ev.startWorld.y + fireCamFwd.y * (kRayLen * frac),
                                            ev.startWorld.z + fireCamFwd.z * (kRayLen * frac),
                                        };
                                    }
                                }
                            }
                        }
                    }

                    // Handoff continuity: inherit the exact endpoint the
                    // live preview last rendered for this hand instead of
                    // using correctedTarget (a separate fire-time raycast
                    // ~1 frame behind the preview). The preview is camera-
                    // locked and re-anchors every render frame, so by the
                    // release frame it sits one frame of rotation ahead of
                    // the fire-time direction — switching to correctedTarget
                    // snaps the trace back by that frame (the residual
                    // flicker). Inheriting the preview endpoint makes the
                    // seam exact. Falls back to correctedTarget if the
                    // cache is stale/absent (e.g. instant cast with no
                    // preview frames). The projectile launched along the
                    // aim the preview showed, so this is also accurate.
                    // Inherit the endpoint the live preview last drew so
                    // the trace doesn't jump at release. Prefer the exact
                    // casting hand; if the detour's hand-id disagrees with
                    // the hand that was actually charging (the preview),
                    // fall back to the freshest fresh cache entry and adopt
                    // ITS hand as the shot's source — otherwise the trail's
                    // near end would snap between hands at the handoff.
                    RE::NiPoint3 traceEnd = correctedTarget;
                    float inheritedCamDist   = -1.0f;
                    bool  inheritedAnchorCam = false;
                    int   inheritedSrc       = srcEnum;
                    {
                        std::lock_guard<std::mutex> lk(previewEndCacheMutex_);
                        int best = -1;
                        if (srcEnum >= 0 && srcEnum < 4) {
                            const auto& pc = previewEndCache_[srcEnum];
                            if (pc.valid && pc.projectileFormID == ev.projectileFormID &&
                                (nowSec - pc.wallSec) < 0.15f) best = srcEnum;
                        }
                        if (best < 0) {
                            for (int i = 0; i < 4; ++i) {
                                const auto& pc = previewEndCache_[i];
                                if (pc.valid && pc.projectileFormID == ev.projectileFormID &&
                                    (nowSec - pc.wallSec) < 0.15f &&
                                    (best < 0 || pc.wallSec > previewEndCache_[best].wallSec))
                                    best = i;
                            }
                        }
                        if (best >= 0) {
                            const auto& pc = previewEndCache_[best];
                            traceEnd           = pc.endpoint;
                            inheritedCamDist   = pc.cachedDistance;
                            inheritedAnchorCam = pc.anchorAtCamera;
                            inheritedSrc       = best;
                        }
                    }

                    ProjectileShot s;
                    s.worldPolyline = { ev.startWorld, traceEnd };
                    s.projectileFormID = ev.projectileFormID;
                    const float dx = traceEnd.x - ev.startWorld.x;
                    const float dy = traceEnd.y - ev.startWorld.y;
                    const float dz = traceEnd.z - ev.startWorld.z;
                    const float dist  = std::sqrt(dx*dx + dy*dy + dz*dz);
                    const float speed = (std::max)(ev.projSpeed, 500.0f);
                    s.travelTime        = std::clamp(dist / speed, 0.05f, 3.0f);
                    s.wallClockFireTime = nowSec;
                    s.fireEventSec      = ev.fireSec;
                    s.elapsedGameTime   = 0.0f;
                    s.cachedDistance    = dist;
                    // Use the hand the preview actually charged on (see the
                    // inherit fallback above), not the detour's possibly-
                    // wrong nearest-node guess, so the blend resolves the
                    // correct magic node for the trail's near end.
                    s.castingSource     = inheritedSrc;
                    // Handoff blend metadata (only when we inherited a
                    // fresh preview endpoint). Lets the renderer reproduce
                    // the preview's camera-locked line for the first few
                    // frames, then ease to this world-locked one — no jump
                    // at release, even while turning. See ProjectileShot.
                    s.camAnchorDist     = inheritedCamDist;
                    s.anchorAtCamera    = inheritedAnchorCam;
                    spdlog::info(
                        "[SpellFire] start=({:.1f},{:.1f},{:.1f}) "
                        "origTarget=({:.1f},{:.1f},{:.1f}) "
                        "corrTarget=({:.1f},{:.1f},{:.1f}) dist={:.0f} travel={:.2f}s",
                        ev.startWorld.x, ev.startWorld.y, ev.startWorld.z,
                        ev.targetWorld.x, ev.targetWorld.y, ev.targetWorld.z,
                        correctedTarget.x, correctedTarget.y, correctedTarget.z,
                        dist, s.travelTime);
                    std::lock_guard<std::mutex> lock(firedShotsMutex_);
                    firedShots_.push_back(std::move(s));
                    if (firedShots_.size() > 16) {
                        firedShots_.erase(firedShots_.begin());
                    }
                }
            }
        }

        // Arrow fire event poll. The release-edge eager push above
        // (in the bow Tick path) usually beats the detour by 1-2
        // frames. If the eager push already added a shot within
        // 250ms, skip this one — otherwise (e.g., eager push failed
        // because lastArrowWorldPath_ was empty), fall back to a
        // straight-line shot here.
        if (wantArrow) {
            std::uint32_t v = 0;
            RE::NiPoint3 startW, targetW;
            float speed = 3000.0f;
            if (ArrowPathDetour::GetLastFireEvent(v, startW, targetW, speed) &&
                v != lastArrowFireVersion_)
            {
                lastArrowFireVersion_ = v;
                const bool eagerCovered = (nowSec - lastBowFireWallSec_) < 0.25f;
                if (!eagerCovered) {
                    std::vector<RE::NiPoint3> path;
                    {
                        std::lock_guard<std::mutex> lock(lastArrowWorldPathMutex_);
                        path = lastArrowWorldPath_;
                    }
                    if (path.size() < 2) {
                        path = { startW, targetW };
                    }
                    ProjectileShot s;
                    s.worldPolyline = std::move(path);
                    float dist = 0.0f;
                    for (std::size_t i = 1; i < s.worldPolyline.size(); ++i) {
                        const auto& a = s.worldPolyline[i - 1];
                        const auto& b = s.worldPolyline[i];
                        const float dx = b.x - a.x;
                        const float dy = b.y - a.y;
                        const float dz = b.z - a.z;
                        dist += std::sqrt(dx*dx + dy*dy + dz*dz);
                    }
                    const float spd = (std::max)(speed, 500.0f);
                    s.travelTime        = std::clamp(dist / spd, 0.05f, 5.0f);
                    s.wallClockFireTime = nowSec;
                    s.elapsedGameTime   = 0.0f;
                    std::lock_guard<std::mutex> lock(firedShotsMutex_);
                    firedShots_.push_back(std::move(s));
                    if (firedShots_.size() > 16) {
                        firedShots_.erase(firedShots_.begin());
                    }
                    lastBowFireWallSec_ = nowSec;
                    spdlog::debug("[ArrowDetourFire] v={} dist={:.0f} (detour-event push)", v, dist);
                } else {
                    spdlog::debug("[ArrowDetourFire] v={} skipped (eager already pushed)", v);
                }
            }
        }

        // Snapshot anyShots after the fire-event polls above (tick/prune
        // already ran near the top of this function so post-impact fade
        // works in every mode, including Bow).
        bool  anyShots      = false;
        float newestFireSec = -1.0f;
        {
            std::lock_guard<std::mutex> lock(firedShotsMutex_);
            anyShots = !firedShots_.empty();
            for (const auto& s : firedShots_)
                newestFireSec = (std::max)(newestFireSec, s.wallClockFireTime);
        }

        // Mode resolution: charging wins (live cursor); else any
        // in-flight shot keeps us in Magic mode for the visualization;
        // else None.
        constexpr float kReleaseBridge = 0.08f;
        if (magicNow) {
            magicWasCharging_   = true;
            magicReleaseBridge_ = false;
            magicLastChargeSec_ = nowSec;
            return AimMode::Magic;
        }
        magicWasCharging_ = false;
        // Has the NEW shot for the charge we just finished appeared yet?
        // A shot fired AFTER the last charge frame has a fire time later
        // than magicLastChargeSec_. This distinguishes the new shot from
        // an OLD shot still fading on screen — during rapid casting
        // anyShots is true the whole time because the previous shot
        // lingers ~1s, which used to preempt the bridge and blank the new
        // charge's preview for a few frames.
        const bool newShotSinceCharge = (newestFireSec > magicLastChargeSec_);
        if ((nowSec - magicLastChargeSec_) < kReleaseBridge && !newShotSinceCharge) {
            // Bridge the charge-end → fired-shot gap so the new charge's
            // preview doesn't blink out — regardless of any OLD shot
            // still fading.
            magicReleaseBridge_ = true;
            return AimMode::Magic;
        }
        // The new shot is on screen (or an old one still fades) — no
        // bridge, and the preview must stop so it doesn't double-draw
        // over the shot.
        magicReleaseBridge_ = false;
        if (anyShots) return AimMode::Magic;
        return AimMode::None;
    }

    bool CrosshairManager::RaycastFromCamera(float& outDist) const
    {
        outDist = kRayLen;
        auto* player    = RE::PlayerCharacter::GetSingleton();
        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (!player || !playerCam) return false;

        auto* cell = player->GetParentCell();
        if (!cell) return false;
        auto* world = cell->GetbhkWorld();
        if (!world) return false;

        // Camera forward derived from cameraNI->world.rotate column 0,
        // matching the rest of the codebase's convention.
        RE::NiCamera* niCam = nullptr;
        if (!ResolveCameraNi(niCam) || !niCam) return false;
        const auto& m = niCam->world.rotate;
        const RE::NiPoint3 forward{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };

        const auto& origin = niCam->world.translate;
        const RE::NiPoint3 endPt{
            origin.x + forward.x * kRayLen,
            origin.y + forward.y * kRayLen,
            origin.z + forward.z * kRayLen,
        };

        RE::bhkPickData pd{};
        const float scale = RE::bhkWorld::GetWorldScale();
        pd.rayInput.from.quad = _mm_setr_ps(origin.x * scale, origin.y * scale, origin.z * scale, 0.0f);
        pd.rayInput.to.quad   = _mm_setr_ps(endPt.x  * scale, endPt.y  * scale, endPt.z  * scale, 0.0f);
        pd.rayInput.filterInfo.filter = kPickFilterInfo;
        pd.ray.quad           = _mm_setr_ps(
            (endPt.x - origin.x) * scale,
            (endPt.y - origin.y) * scale,
            (endPt.z - origin.z) * scale,
            0.0f);

        if (!world->PickObject(pd)) return false;
        const float frac = pd.rayOutput.hitFraction;
        if (frac >= 1.0f || frac <= 0.0f) return false;

        outDist = kRayLen * frac;
        return true;
    }

    // Lazy one-time load of the calibration cache. Caller holds
    // releaseAnchorMutex_. A missing or unparsable file just leaves every
    // anchor invalid — the previews fall back to the live node until the
    // first fire re-teaches them.
    void CrosshairManager::EnsureReleaseAnchorsLoadedLocked()
    {
        if (anchorsLoaded_) return;
        anchorsLoaded_ = true;
        try {
            if (!std::filesystem::exists(kTraceCalibPath)) return;
            const auto tbl = toml::parse_file(kTraceCalibPath);
            // v2 = shoulder-pivot aim-oriented frame. v1 offsets (yaw-only,
            // no pivot) would reconstruct ~110u low — drop them and let one
            // cast recalibrate instead of loading a wrong anchor.
            if (tbl["version"].value_or(1) < 2) {
                spdlog::debug("[TraceCalib] pre-v2 calibration ignored — recalibrating");
                return;
            }
            int loaded = 0;
            for (int s = 0; s < 4; ++s) {
                for (int k = 0; k < 2; ++k) {
                    for (int p = 0; p < 2; ++p) {
                        const std::string key =
                            "a" + std::to_string(s) + std::to_string(k) + std::to_string(p);
                        if (const auto* arr = tbl[key].as_array(); arr && arr->size() == 3) {
                            auto& a = releaseAnchors_[s][k][p];
                            a.offsetLocal.x = static_cast<float>(arr->at(0).value_or(0.0));
                            a.offsetLocal.y = static_cast<float>(arr->at(1).value_or(0.0));
                            a.offsetLocal.z = static_cast<float>(arr->at(2).value_or(0.0));
                            // Learned pitch gain (absent in pre-gain files →
                            // 1.0, the old rigid-pivot behavior).
                            a.pitchGain = static_cast<float>(
                                tbl["g" + std::to_string(s) + std::to_string(k) +
                                    std::to_string(p)].value_or(1.0));
                            a.valid = true;
                            ++loaded;
                        }
                    }
                }
            }
            if (loaded > 0)
                spdlog::debug("[TraceCalib] loaded {} release anchors", loaded);
        } catch (...) {
            spdlog::warn("[TraceCalib] failed to parse {} — recalibrating from live fires",
                         kTraceCalibPath);
        }
    }

    // Debounced auto-save: a few quiet seconds after the last learn, write
    // the whole (tiny) anchor set. Called once per Tick from the fire-event
    // drain path, so a burst of casts costs one file write, not one per cast.
    void CrosshairManager::FlushReleaseAnchorsIfDue(double a_nowSec)
    {
        ReleaseAnchor snap[4][2][2];
        {
            std::lock_guard<std::mutex> lk(releaseAnchorMutex_);
            if (!anchorsDirty_ || (a_nowSec - anchorsLastLearnSec_) < 3.0) return;
            anchorsDirty_ = false;
            std::memcpy(snap, releaseAnchors_, sizeof(snap));
        }
        toml::table t;
        t.insert("version", 2);
        for (int s = 0; s < 4; ++s) {
            for (int k = 0; k < 2; ++k) {
                for (int p = 0; p < 2; ++p) {
                    const auto& a = snap[s][k][p];
                    if (!a.valid) continue;
                    const std::string key =
                        "a" + std::to_string(s) + std::to_string(k) + std::to_string(p);
                    t.insert(key, toml::array{
                        static_cast<double>(a.offsetLocal.x),
                        static_cast<double>(a.offsetLocal.y),
                        static_cast<double>(a.offsetLocal.z) });
                    t.insert("g" + std::to_string(s) + std::to_string(k) + std::to_string(p),
                             static_cast<double>(a.pitchGain));
                }
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(kTraceCalibPath).parent_path(), ec);
        std::ofstream f(kTraceCalibPath);
        if (f) {
            f << t;
            spdlog::debug("[TraceCalib] release anchors saved");
        }
    }

    // Learn where spells ACTUALLY leave from: called with the detour's true
    // projectile spawn position at every player fire. Stored player-yaw-local
    // (x = right, y = forward, z = up relative to the body heading) so the
    // reconstructed origin rides walking, strafing and turning smoothly
    // instead of wobbling with the hand's charge animation. EMA (0.35) so a
    // changed animation set or stance re-converges within a few casts.
    // Drain the detour's fire-event ring WITHOUT creating trails.
    //
    // THE BUG THIS EXISTS FOR (user report 2026-08-16): "when killing an
    // enemy with a projectile, there are projectile traces that appear after
    // the lock off happens upon their death." DrainFireEvents is called from
    // exactly one place — DetectAimMode — and Tick() RETURNS BEFORE
    // DetectAimMode while TDM is locked. So every spell fired during a lock
    // sat undrained in the ring, and the frame the enemy died and TDM locked
    // off, Tick ran through, drained the whole backlog, and read
    // IsTargetLocked() as FALSE (the lock is already gone) — so the
    // "fired while locked, don't trail it" test passed everything and the
    // entire lock's worth of shots became trails at once.
    //
    // The existing guard in DetectAimMode already SAYS this is handled ("the
    // events are still drained (sequence advances) and still teach the
    // release anchors; only the trail/reticle creation is skipped") — that
    // was simply never true on the path that returns early. Draining here
    // makes the comment honest: no backlog can survive a bail, so nothing
    // can flood out of one.
    //
    // LESSON: a "we skip X while condition C" test is only sound if the code
    // that evaluates it actually RUNS during C. Test the flag where the
    // event is produced or consumed, not where the consumer happens to be
    // reachable.
    void CrosshairManager::DiscardPendingFireEvents(bool a_learnAnchors)
    {
        std::vector<MissileProjectileDetour::FireEvent> events;
        MissileProjectileDetour::DrainFireEvents(lastSpellFireVersion_, events);
        if (!a_learnAnchors) return;
        for (const auto& ev : events) {
            // Same hand swap as the main drain: the detour indexes
            // 0=Right/1=Left, the anchor store uses the RE CastingSource
            // enum (0=Left, 1=Right). A shot fired under lock is still a
            // real measurement of where the spell leaves the hand, so the
            // calibration keeps learning from it.
            if (ev.castingSource >= 0) {
                const int srcEnum = (ev.castingSource == 0) ? 1
                                  : (ev.castingSource == 1) ? 0
                                  : ev.castingSource;
                LearnReleaseAnchor(srcEnum, ev.startWorld);
            }
        }
        // The main drain flushes the debounced calibration write; a long
        // lock would otherwise hold newly-learned anchors in memory until
        // it ended.
        FlushReleaseAnchorsIfDue(AnchorNowSeconds());
    }

    void CrosshairManager::LearnReleaseAnchor(int a_castingSourceEnum,
                                              const RE::NiPoint3& a_worldPos)
    {
        if (a_castingSourceEnum < 0 || a_castingSourceEnum >= 4) return;
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) return;
        // Shoulder-pivot, aim-oriented frame: yaw AND pitch. The arm tracks
        // the aim, so a level-learned anchor must rotate DOWN about the
        // shoulder when the player aims down — the earlier yaw-only frame
        // left the origin at level height, which is what made the trace
        // drift under "vertical camera movement while casting".
        constexpr float kShoulderZ = 110.0f;
        RE::NiPoint3 d = a_worldPos - ply->GetPosition();
        d.z -= kShoulderZ;
        // IMPOSSIBLE-TEACHER GATE (log-adjudicated 2026-08-13): the session
        // log carried spawn samples 457u / 551u / 1805u from the player —
        // no hand release happens from there. Those are target-anchored or
        // scripted projectiles, or the player displaced mid-drain (Whirlwind
        // Sprint). They can never teach a release POSE; before this gate
        // they at least squatted in the pending-snap slot for its 20s TTL.
        {
            const float off2 = d.x * d.x + d.y * d.y + d.z * d.z;
            constexpr float kMaxArmReach = 150.0f;
            if (off2 > kMaxArmReach * kMaxArmReach) {
                spdlog::debug("[TraceCalib] teacher ignored (|off|={:.0f}u — not a hand release)",
                             std::sqrt(off2));
                return;
            }
        }
        const float yaw = ply->data.angle.z;
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        // fwd = (sin yaw, cos yaw), right = (cos yaw, -sin yaw).
        const RE::NiPoint3 dy{
            d.x * c - d.y * s,   // right component
            d.x * s + d.y * c,   // forward component
            d.z,
        };
        const float pitch = ply->data.angle.x;
        const int sneak = ply->IsSneaking() ? 1 : 0;
        int pov = 0;
        if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFirstPerson()) pov = 1;
        bool grounded = true;
        if (auto* cc = ply->GetCharController())
            grounded = cc->context.currentState ==
                       RE::hkpCharacterStateType::kOnGround;
        const double nowSec = AnchorNowSeconds();
        std::lock_guard<std::mutex> lk(releaseAnchorMutex_);
        EnsureReleaseAnchorsLoadedLocked();
        auto& a = releaseAnchors_[a_castingSourceEnum][sneak][pov];
        // One lesson per CAST: dual-cast attribution and multi-projectile
        // spells deliver several fire events in the same instant with
        // scattered spawn points; only the first teaches.
        constexpr double kTeachRefractory = 0.30;
        if (a.valid && (nowSec - a.lastTeachSec) < kTeachRefractory) return;
        a.lastTeachSec = nowSec;
        // LEARNED PITCH GAIN (2026-08-14). The rigid model divided out the
        // FULL aim pitch about the pivot; the 16:25 residual log showed the
        // real release only follows a fraction of it (up≈-11u at both -23°
        // and -45°: predicted origin consistently too high aiming up). So
        // estimate, per pitched grounded cast, what fraction of the aim
        // pitch the release elevation actually moved — measured against the
        // stored base's elevation — and EMA it. Must run BEFORE this sample
        // is folded into the base, and before the sample is converted into
        // the (gain-divided) local frame below. Airborne poses are excluded
        // (they release from a different pose entirely — see the AIRBORNE
        // residual note), and near-level casts carry no signal.
        if (a.valid && grounded && std::fabs(pitch) > 0.15f) {
            const float baseMag = std::sqrt(a.offsetLocal.y * a.offsetLocal.y +
                                            a.offsetLocal.z * a.offsetLocal.z);
            if (baseMag > 10.0f) {
                const float phi0 = std::atan2(a.offsetLocal.z, a.offsetLocal.y);
                const float phiS = std::atan2(dy.z, dy.y);
                const float kSample =
                    std::clamp((phi0 - phiS) / pitch, 0.0f, 1.2f);
                a.pitchGain += (kSample - a.pitchGain) * 0.35f;
            }
        }
        // Aim-pitch frame (engine sign: + = aiming down), scaled by the
        // learned gain. aimFwd = (0, cp, -sp), aimUp = (0, sp, cp) in the
        // yaw frame. gain 1.0 (fresh slot / pre-gain calibration file)
        // reproduces the old rigid division exactly.
        const float eff = a.pitchGain * pitch;
        const float cp = std::cos(eff);
        const float sp = std::sin(eff);
        const RE::NiPoint3 local{
            dy.x,
            dy.y * cp - dy.z * sp,
            dy.y * sp + dy.z * cp,
        };
        if (a.valid) {
            // Animation-change detection, TWO-SAMPLE confirmed. Normal
            // variance (gait phase, aim-pitch torso twist) stays near the
            // anchor -> EMA. A far sample alone proves nothing: rapid-fire
            // chains release mid-blend from scattered spots, and snapping on
            // one sample thrashed the anchor during spam casting ("issues
            // when spells are cast too close together"). A REAL animation
            // change produces consistent new positions, so: far sample ->
            // held as pending; a second far sample that AGREES with the
            // pending (within a tight radius) confirms the new pose and
            // snaps; a disagreeing far sample replaces the pending (chain
            // scatter never agrees with itself, so spam can't move the
            // anchor at all). A near sample clears the pending.
            const float ddx = local.x - a.offsetLocal.x;
            const float ddy = local.y - a.offsetLocal.y;
            const float ddz = local.z - a.offsetLocal.z;
            // Residual diagnostic ("still isn't accurate with vertical
            // movement", 2026-08-13). The rigid shoulder-pivot model assumes
            // the release node follows the FULL aim pitch about z=110; if the
            // torso-twist additive only carries a fraction of it, residuals
            // grow with |pitch| — and if aerial casts release from a
            // different pose, residuals spike while airborne. One line per
            // lesson (already refractory-limited to one per cast), residual
            // in the anchor's own frame (x=right, y=aim-fwd, z=aim-up), so
            // the next vertical-aim session states which model is wrong
            // instead of us guessing a third time.
            {
                const float rmag = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                if (rmag > 6.0f) {
                    // Name the spell: the 17:42 log had a 57u two-sample
                    // pose change that could equally be a spell swap or a
                    // pitch-model error — without the spell on the line the
                    // two are indistinguishable.
                    const char* spellName = "?";
                    if (auto* caster = ply->GetMagicCaster(
                            static_cast<RE::MagicSystem::CastingSource>(a_castingSourceEnum))) {
                        if (caster->currentSpell && caster->currentSpell->GetName())
                            spellName = caster->currentSpell->GetName();
                    }
                    spdlog::debug(
                        "[TraceCalib] residual src={} sneak={} pov={} "
                        "d(right,fwd,up)=({:.1f},{:.1f},{:.1f}) |d|={:.1f} "
                        "pitchDeg={:.1f} gain={:.2f} {} spell=\"{}\"",
                        a_castingSourceEnum, sneak, pov, ddx, ddy, ddz, rmag,
                        pitch * 57.29578f, a.pitchGain,
                        grounded ? "grounded" : "AIRBORNE",
                        spellName);
                }
            }
            constexpr float  kSnapDist2    = 25.0f * 25.0f;
            constexpr float  kAgreeDist2   = 15.0f * 15.0f;
            constexpr double kPendingTtl   = 20.0;
            if (ddx * ddx + ddy * ddy + ddz * ddz > kSnapDist2) {
                const bool pendingFresh =
                    a.pendingValid && (nowSec - a.pendingSec) < kPendingTtl;
                const float pdx = local.x - a.pendingSnap.x;
                const float pdy = local.y - a.pendingSnap.y;
                const float pdz = local.z - a.pendingSnap.z;
                if (pendingFresh &&
                    (pdx * pdx + pdy * pdy + pdz * pdz) < kAgreeDist2) {
                    a.offsetLocal  = local;
                    a.pendingValid = false;
                    spdlog::debug("[TraceCalib] release pose changed (src={} sneak={} pov={}) — re-anchored",
                                 a_castingSourceEnum, sneak, pov);
                } else {
                    a.pendingSnap  = local;
                    a.pendingValid = true;
                    a.pendingSec   = nowSec;
                }
            } else {
                a.pendingValid = false;
                constexpr float kEma = 0.35f;
                a.offsetLocal.x += ddx * kEma;
                a.offsetLocal.y += ddy * kEma;
                a.offsetLocal.z += ddz * kEma;
            }
        } else {
            a.offsetLocal = local;
            a.valid       = true;
        }
        anchorsDirty_        = true;
        anchorsLastLearnSec_ = nowSec;
    }

    // Reconstruct the expected release origin for a casting source at the
    // player's CURRENT position/heading/stance. False until that source has
    // fired at least once in this stance/POV — callers fall back to the live
    // node (the pre-anchor behavior). Residual error: the offset is yaw-local
    // only, so steep aim pitch (torso twist) can shift the true origin a few
    // units vertically — still far closer than the mid-charge hand position.
    bool CrosshairManager::PredictedReleaseOrigin(int a_castingSourceEnum,
                                                  RE::NiPoint3& a_out)
    {
        if (a_castingSourceEnum < 0 || a_castingSourceEnum >= 4) return false;
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) return false;
        const int sneak = ply->IsSneaking() ? 1 : 0;
        int pov = 0;
        if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFirstPerson()) pov = 1;
        RE::NiPoint3 local{};
        float gain = 1.0f;
        {
            std::lock_guard<std::mutex> lk(releaseAnchorMutex_);
            EnsureReleaseAnchorsLoadedLocked();
            const auto& a = releaseAnchors_[a_castingSourceEnum][sneak][pov];
            if (!a.valid) return false;
            local = a.offsetLocal;
            gain  = a.pitchGain;
        }
        // Inverse of the learn transform: aim-pitch frame -> yaw frame ->
        // world, about the shoulder pivot at the player's LIVE heading and
        // aim pitch — so the predicted origin nods with the aim, by the
        // LEARNED fraction of the aim pitch (see LearnReleaseAnchor).
        constexpr float kShoulderZ = 110.0f;
        const float pitch = gain * ply->data.angle.x;
        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);
        const RE::NiPoint3 dy{
            local.x,
            local.y * cp + local.z * sp,
            -local.y * sp + local.z * cp,
        };
        const RE::NiPoint3 p = ply->GetPosition();
        const float yaw = ply->data.angle.z;
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        a_out = RE::NiPoint3{
            p.x + c * dy.x + s * dy.y,
            p.y - s * dy.x + c * dy.y,
            p.z + kShoulderZ + dy.z,
        };
        return true;
    }

    bool CrosshairManager::RaycastFromMagicNode(double& outScreenX, double& outScreenY,
                                                float& outDist) const
    {
        outDist     = kRayLen;
        outScreenX  = base_.x;
        outScreenY  = base_.y;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* cell = player->GetParentCell();
        if (!cell) return false;
        auto* world = cell->GetbhkWorld();
        if (!world) return false;

        // Pick the first non-null magic node from R / L / Voice slots.
        RE::NiNode* magicNode = nullptr;
        for (auto src : { RE::MagicSystem::CastingSource::kRightHand,
                          RE::MagicSystem::CastingSource::kLeftHand,
                          RE::MagicSystem::CastingSource::kOther })
        {
            if (auto* caster = player->GetMagicCaster(src)) {
                auto* n = caster->GetMagicNode();
                if (n) {
                    magicNode = n;
                    break;
                }
            }
        }
        if (!magicNode) return false;

        RE::NiCamera* niCam = nullptr;
        if (!ResolveCameraNi(niCam) || !niCam) return false;
        const auto& m       = niCam->world.rotate;
        const RE::NiPoint3 fwd{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };

        const RE::NiPoint3 origin = magicNode->world.translate;
        const RE::NiPoint3 endPt{
            origin.x + fwd.x * kRayLen,
            origin.y + fwd.y * kRayLen,
            origin.z + fwd.z * kRayLen,
        };

        RE::bhkPickData pd{};
        const float scale = RE::bhkWorld::GetWorldScale();
        pd.rayInput.from.quad = _mm_setr_ps(origin.x * scale, origin.y * scale, origin.z * scale, 0.0f);
        pd.rayInput.to.quad   = _mm_setr_ps(endPt.x  * scale, endPt.y  * scale, endPt.z  * scale, 0.0f);
        pd.rayInput.filterInfo.filter = kPickFilterInfo;
        pd.ray.quad           = _mm_setr_ps(
            (endPt.x - origin.x) * scale,
            (endPt.y - origin.y) * scale,
            (endPt.z - origin.z) * scale,
            0.0f);

        RE::NiPoint3 hitPos{};
        bool hit = world->PickObject(pd);
        const float frac = pd.rayOutput.hitFraction;
        if (hit && frac > 0.0f && frac < 1.0f) {
            hitPos = RE::NiPoint3{
                origin.x + fwd.x * (kRayLen * frac),
                origin.y + fwd.y * (kRayLen * frac),
                origin.z + fwd.z * (kRayLen * frac),
            };
            outDist = kRayLen * frac;
        } else {
            // No hit — project the ray endpoint at "infinity" so the
            // crosshair tracks the player's actual aim line.
            hitPos  = endPt;
            outDist = kRayLen;
            hit     = false;
        }

        // Project to screen using the HUD movie's visible frame rect as
        // the port — same pattern as SmoothCam (`crosshair.cpp:404-414`).
        // The output coords land in [rect.left, rect.right] etc., which
        // we then convert to HUDMovieBaseInstance-local coords (origin
        // at center, +y down) by subtracting the rect midpoint and
        // adding the swf-startup baseline offset.
        return ProjectWorldPointToHUD(hitPos, outScreenX, outScreenY);
    }

    bool CrosshairManager::ProjectWorldPointToHUD(const RE::NiPoint3& worldPt,
                                                  double& outHudX, double& outHudY) const
    {
        outHudX = base_.x;
        outHudY = base_.y;

        RE::NiCamera* niCam = nullptr;
        if (!ResolveCameraNi(niCam) || !niCam) return false;

        // Use the HUD's visible frame rect as the port. The output
        // sx/sy then sit in [rect.left, rect.right] x [rect.top,
        // rect.bottom] directly — Scaleform stage units, no normalize.
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return false;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return false;
        const auto frame = hud->uiMovie->GetVisibleFrameRect();
        const RE::NiRect<float> port{ frame.left, frame.right, frame.top, frame.bottom };

        const auto& runtime = niCam->GetRuntimeData();
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!RE::NiCamera::WorldPtToScreenPt3(
                runtime.worldToCam, port, worldPt, sx, sy, sz, 1e-5f)) {
            return false;
        }
        if (sz <= 0.0f) return false;  // behind camera

        // Convert from frame-rect coords to HUD-local coords (origin at
        // rect midpoint) and add the swf's captured offset, mirroring
        // SmoothCam `crosshair.cpp:494-501`.
        const double midX = (static_cast<double>(frame.right) + static_cast<double>(frame.left))  * 0.5;
        const double midY = (static_cast<double>(frame.bottom) + static_cast<double>(frame.top)) * 0.5;
        outHudX = (static_cast<double>(sx) - midX) + base_.x;
        outHudY = (static_cast<double>(sy) - midY) + base_.y;
        return true;
    }

    void CrosshairManager::WriteCrosshairScreenPos(double sx, double sy, bool forceVisible)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return;
        auto& runtime = hud->GetRuntimeData();

        RE::GFxValue crosshair;
        if (!runtime.root.GetMember("Crosshair", &crosshair)) return;

        // forceVisible=true: per-frame DisplayInfo.SetVisible(true). This
        // bypasses the SetVisibility AS-invoke deduper and lands directly
        // on the same SWF state the engine writes when it hides the
        // crosshair on sneak transitions etc., so we win every frame
        // prediction is active. forceVisible=false: position-only,
        // engine-owned visibility (used by RestoreCrosshair so vanilla
        // hide-on-sneak with sheathed/melee weapons is preserved).
        RE::GFxValue::DisplayInfo di;
        di.SetPosition(sx, sy);
        if (forceVisible) {
            di.SetVisible(true);
        }
        crosshair.SetDisplayInfo(di);

        // ALSO write to the parent CrosshairInstance via SetVariable.
        // The engine appears to reset CrosshairInstance position on
        // sneak transition (one-frame flicker symptom). The two are
        // separate Scaleform symbols — Crosshair is the inner widget
        // we move, CrosshairInstance is its parent group. Writing
        // both keeps the entire group locked.
        RE::GFxValue val;
        val.SetNumber(sx);
        hud->uiMovie->SetVariable(
            "_root.HUDMovieBaseInstance.CrosshairInstance._x", val);
        val.SetNumber(sy);
        hud->uiMovie->SetVariable(
            "_root.HUDMovieBaseInstance.CrosshairInstance._y", val);
    }

    void CrosshairManager::WriteCrosshairScale(double scalePercent)
    {
        WriteCrosshairScaleXY(scalePercent, scalePercent);
    }

    void CrosshairManager::WriteCrosshairScaleXY(double sx, double sy)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud) return;
        auto& runtime = hud->GetRuntimeData();

        RE::GFxValue crosshair;
        if (!runtime.root.GetMember("Crosshair", &crosshair)) return;

        RE::GFxValue::DisplayInfo di;
        di.SetScale(sx, sy);
        crosshair.SetDisplayInfo(di);
    }

    void CrosshairManager::WriteStealthMeterPosition(double sx, double sy)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return;

        // Position writes (offset). Both SetDisplayInfo and direct
        // SetVariable so any reset from the engine is overridden.
        RE::GFxValue var;
        if (hud->uiMovie->GetVariable(
                &var, "_root.HUDMovieBaseInstance.StealthMeterInstance") &&
            var.IsDisplayObject())
        {
            RE::GFxValue::DisplayInfo di;
            di.SetPosition(sx, sy);
            var.SetDisplayInfo(di);
        }
        RE::GFxValue val;
        val.SetNumber(sx);
        hud->uiMovie->SetVariable(
            "_root.HUDMovieBaseInstance.StealthMeterInstance._x", val);
        val.SetNumber(sy);
        hud->uiMovie->SetVariable(
            "_root.HUDMovieBaseInstance.StealthMeterInstance._y", val);
    }

    void CrosshairManager::WriteStealthMeterAlpha(double alpha)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return;

        RE::GFxValue val;
        val.SetNumber(alpha);
        hud->uiMovie->SetVariable(
            "_root.HUDMovieBaseInstance.StealthMeterInstance._alpha", val);
    }

    void CrosshairManager::RestoreStealthMeter()
    {
        if (!stealthMeterRelocated_) return;
        if (base_.stealthValid) {
            WriteStealthMeterPosition(base_.stealthX, base_.stealthY);
        }
        stealthMeterRelocated_ = false;
    }

    void CrosshairManager::SetVisibility(bool visible)
    {
        if (haveLastWritten_ && lastVisible_ == visible) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return;
        const RE::GFxValue arg{ visible };
        hud->uiMovie->Invoke("HUDMovieBaseInstance.SetCrosshairEnabled", nullptr, &arg, 1);
        lastVisible_     = visible;
        haveLastWritten_ = true;
    }

    namespace
    {
        // Look up the player's nocked-arrow node — this is the world
        // position the arrow actually fires from. SmoothCam maps these
        // strings in `crosshair.h:184-188`.
        RE::NiAVObject* FindArrowNode(RE::PlayerCharacter* a_ply, bool a_crossbow)
        {
            if (!a_ply || !a_ply->Get3D()) return nullptr;
            auto* root = a_ply->Get3D();
            const RE::BSFixedString kWeapon{ "WEAPON" };
            const RE::BSFixedString kArrowName{ "Arrow:0" };
            const RE::BSFixedString kRMag{ "NPC R MagicNode [RMag]" };
            const RE::BSFixedString kLMag{ "NPC L MagicNode [LMag]" };

            if (a_crossbow) {
                if (auto* rmag = root->GetObjectByName(kRMag)) {
                    if (auto* arrow = rmag->GetObjectByName(kArrowName)) return arrow;
                    return rmag;  // horseback fallback (Arrow:0 sometimes missing)
                }
            }

            if (auto* hand = root->GetObjectByName(kWeapon)) {
                if (auto* arrow = hand->GetObjectByName(kArrowName)) return arrow;
            }
            if (auto* hand = root->GetObjectByName(kRMag)) {
                if (auto* arrow = hand->GetObjectByName(kArrowName)) return arrow;
            }
            if (auto* hand = root->GetObjectByName(kLMag)) {
                if (auto* arrow = hand->GetObjectByName(kArrowName)) return arrow;
            }
            return nullptr;
        }

        // The body-vs-camera yaw delta SmoothCam corrects for at
        // `crosshair.cpp:275` — when the camera is offset off the
        // player's shoulder, the arrow node's world position is rotated
        // around the player's Z axis to align with where the player
        // visually points. Without this the fire-origin sits on the
        // wrong side of the body for off-center cameras.
        RE::NiPoint3 TranslateFirePos(RE::PlayerCharacter* a_ply,
                                      float a_camYaw,
                                      const RE::NiPoint3& a_firePos)
        {
            if (!a_ply || !a_ply->Get3D()) return a_firePos;
            const RE::NiPoint3 plyPos{
                a_ply->Get3D()->world.translate.x,
                a_ply->Get3D()->world.translate.y,
                a_ply->Get3D()->world.translate.z
            };
            const RE::NiPoint3 local{
                a_firePos.x - plyPos.x,
                a_firePos.y - plyPos.y,
                a_firePos.z - plyPos.z
            };
            const float yawDelta = a_ply->data.angle.z - a_camYaw;
            const float c = std::cos(yawDelta);
            const float s = std::sin(yawDelta);
            return RE::NiPoint3{
                plyPos.x + (local.x * c - local.y * s),
                plyPos.y + (local.x * s + local.y * c),
                plyPos.z + local.z
            };
        }

        // SmoothCam Verlet step at `crosshair.cpp:64`. Uses the iron-arrow
        // .nif damping (0.099609) and the engine's odd (0,0,59) gravity
        // scaler. Kept verbatim — these constants are derived from the
        // shipped arrow asset and don't change between weapon types.
        void TickPath(RE::NiPoint3& pos, RE::NiPoint3& vel,
                      const RE::NiPoint3& gravity, float gravityScale, float dt)
        {
            constexpr float linDamp      = 0.099609f;
            constexpr float gravityFactor = 1.0f;
            const RE::NiPoint3 magic{ 0.0f, 0.0f, 59.0f };
            const RE::NiPoint3 gv{
                gravity.x * gravityFactor * gravityScale * magic.x,
                gravity.y * gravityFactor * gravityScale * magic.y,
                gravity.z * gravityFactor * gravityScale * magic.z
            };
            const RE::NiPoint3 step{
                gv.x - vel.x * linDamp,
                gv.y - vel.y * linDamp,
                gv.z - vel.z * linDamp
            };
            vel.x += step.x * dt;
            vel.y += step.y * dt;
            vel.z += step.z * dt;
            pos.x += vel.x * dt;
            pos.y += vel.y * dt;
            pos.z += vel.z * dt;
        }
    }

    bool CrosshairManager::PredictArrowImpact(double& outScreenX, double& outScreenY,
                                              float& outDist)
    {
        // Always start with a sane fallback. If we early-bail anywhere,
        // the caller still gets centered baseline coords — never (0,0)
        // which would put the reticle off-screen at the top-left.
        outScreenX = base_.x;
        outScreenY = base_.y;
        outDist    = kRayLen;

        // Clear stale world-space path data up front. Every call leaves
        // lastArrowWorldPath_ either freshly repopulated (success path
        // at the end of this function) OR empty (any early-bail return).
        // Without this, an early bail — e.g. Arrow:0 node not yet
        // attached on the first frame of a new draw — leaves the live-
        // preview AND the eager-release-edge push both reading the
        // PRIOR shot's polyline. The next shot's trail + reticle then
        // appear at the previous shot's location until the in-flight
        // entry fades out 1-2s later (the "ghost shot" bug).
        {
            std::lock_guard<std::mutex> lock(lastArrowWorldPathMutex_);
            lastArrowWorldPath_.clear();
        }

        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) return false;
        auto* ammo = ply->GetCurrentAmmo();
        // Most aimable bow shots have ammo; fall back to a reasonable
        // default speed so users with zero arrows still see a reticle.
        const float ammoSpeed = (ammo && ammo->data.projectile)
            ? ammo->data.projectile->data.speed : 5000.0f;
        const float ammoGravity = (ammo && ammo->data.projectile)
            ? ammo->data.projectile->data.gravity : 1.0f;

        // Detect crossbow vs bow for the arrow-node lookup.
        bool isCrossbow = false;
        if (auto* right = ply->GetEquippedObject(false)) {
            if (auto* weap = right->As<RE::TESObjectWEAP>()) {
                isCrossbow = (weap->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow);
            }
        }

        // Arrow:0 may not exist (e.g. zero ammo, draw not yet started).
        // Fall back to the WEAPON / RMag transform itself — close enough
        // for the prediction's start point, and crucially keeps the
        // crosshair visible while drawing.
        auto* arrowNode = FindArrowNode(ply, isCrossbow);
        if (!arrowNode) {
            const RE::BSFixedString kWeapon{ "WEAPON" };
            const RE::BSFixedString kRMag{ "NPC R MagicNode [RMag]" };
            if (auto* root = ply->Get3D()) {
                arrowNode = isCrossbow
                    ? root->GetObjectByName(kRMag)
                    : root->GetObjectByName(kWeapon);
                if (!arrowNode) arrowNode = root->GetObjectByName(kRMag);
            }
        }
        if (!arrowNode) return false;

        // Camera forward + up direct from the NiCamera matrix — same
        // source as ArrowPathDetour. Avoids the 3p body-pitch trap
        // (data.angle.x is body pitch, which is ~0 even when the
        // camera is looking up/down).
        RE::NiCamera* niCamForAim = nullptr;
        if (!ResolveCameraNi(niCamForAim) || !niCamForAim) return false;
        const auto& cm      = niCamForAim->world.rotate;
        const RE::NiPoint3 forward{ cm.entry[0][0], cm.entry[1][0], cm.entry[2][0] };
        const RE::NiPoint3 up     { cm.entry[0][1], cm.entry[1][1], cm.entry[2][1] };

        // True-aim direction for the prediction. The detour aims the
        // arrow toward whatever world point the camera-forward ray
        // hits (so arrows land on the crosshair, not parallel-shifted
        // off-axis from the bow). The prediction must use the SAME
        // aim direction or it diverges from where arrows actually go.
        // Compute the target by raycasting from the camera here too.
        const RE::NiPoint3 camPos{
            niCamForAim->world.translate.x,
            niCamForAim->world.translate.y,
            niCamForAim->world.translate.z
        };
        constexpr float kAimRayLen = 8000.0f;
        RE::NiPoint3 aimTarget{
            camPos.x + forward.x * kAimRayLen,
            camPos.y + forward.y * kAimRayLen,
            camPos.z + forward.z * kAimRayLen,
        };
        if (auto* aimCell = ply->GetParentCell()) {
            if (auto* aimBhk = aimCell->GetbhkWorld()) {
                if (auto* aimW = aimBhk->GetWorld1()) {
                    const float ws = RE::bhkWorld::GetWorldScale();
                    RE::hkpWorldRayCastInput  in;
                    RE::hkpWorldRayCastOutput out;
                    in.from.quad = _mm_setr_ps(camPos.x*ws, camPos.y*ws, camPos.z*ws, 0.0f);
                    in.to.quad   = _mm_setr_ps(aimTarget.x*ws, aimTarget.y*ws, aimTarget.z*ws, 0.0f);
                    in.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kCameraSphere);
                    in.enableShapeCollectionFilter = false;
                    aimW->CastRay(in, out);
                    if (out.HasHit()) {
                        aimTarget = RE::NiPoint3{
                            camPos.x + forward.x * (kAimRayLen * out.hitFraction),
                            camPos.y + forward.y * (kAimRayLen * out.hitFraction),
                            camPos.z + forward.z * (kAimRayLen * out.hitFraction),
                        };
                    }
                }
            }
        }

        // Aim direction = (target - arrow_node), normalized. f3PArrow-
        // TiltUpAngle:Combat tilt is rotated around the corrected
        // direction's right axis (using world Z as up reference) so
        // the tilt remains "upward" relative to the actual flight.
        RE::NiPoint3 aimDirRaw{
            aimTarget.x - arrowNode->world.translate.x,
            aimTarget.y - arrowNode->world.translate.y,
            aimTarget.z - arrowNode->world.translate.z,
        };
        {
            // Sneak / close-quarters guard — same logic as the
            // ArrowPathDetour fire path. When the camera-forward
            // raycast hits within ~25 ft of the bow node along the
            // camera's forward axis, true-aim becomes catastrophic
            // (the hand↔camera lateral offset dominates and the
            // predicted arc diverges sideways from where the user is
            // aiming). Falling back to camera-forward keeps the
            // prediction matching the fired-arrow path.
            constexpr float kMinForwardDist = 500.0f;
            const float fwdDist = aimDirRaw.x * forward.x +
                                  aimDirRaw.y * forward.y +
                                  aimDirRaw.z * forward.z;
            if (fwdDist < kMinForwardDist) {
                aimDirRaw = forward;
            } else {
                const float aLen = std::sqrt(aimDirRaw.x*aimDirRaw.x +
                                             aimDirRaw.y*aimDirRaw.y +
                                             aimDirRaw.z*aimDirRaw.z);
                if (aLen > 0.001f) {
                    aimDirRaw.x /= aLen;
                    aimDirRaw.y /= aLen;
                    aimDirRaw.z /= aLen;
                } else {
                    aimDirRaw = forward;
                }
            }
        }
        // Gram-Schmidt against camera-right. The previous implementation
        // crossed aimDirRaw with world-up, which is degenerate when
        // aiming near-vertical (cross product collapses to zero, leaving
        // aimRight=0 and aimUp=0 — fireDir loses its tilt term and the
        // trail / reticle / arrow all go haywire when looking at the
        // sky). Camera-right is always perpendicular to camera-forward,
        // so projecting it onto aimDirRaw gives a stable side vector
        // regardless of aim pitch.
        const RE::NiPoint3 camRight{ cm.entry[0][2], cm.entry[1][2], cm.entry[2][2] };
        RE::NiPoint3 aimRight = camRight;
        {
            const float dot = aimRight.x * aimDirRaw.x +
                              aimRight.y * aimDirRaw.y +
                              aimRight.z * aimDirRaw.z;
            aimRight.x -= aimDirRaw.x * dot;
            aimRight.y -= aimDirRaw.y * dot;
            aimRight.z -= aimDirRaw.z * dot;
            const float rLen = std::sqrt(aimRight.x*aimRight.x +
                                         aimRight.y*aimRight.y +
                                         aimRight.z*aimRight.z);
            if (rLen > 0.001f) {
                aimRight.x /= rLen;
                aimRight.y /= rLen;
                aimRight.z /= rLen;
            } else {
                aimRight = RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
            }
        }
        const RE::NiPoint3 aimUp{
            aimRight.y * aimDirRaw.z - aimRight.z * aimDirRaw.y,
            aimRight.z * aimDirRaw.x - aimRight.x * aimDirRaw.z,
            aimRight.x * aimDirRaw.y - aimRight.y * aimDirRaw.x,
        };

        float tiltDeg = 2.5f;
        if (auto* iniColl = RE::INISettingCollection::GetSingleton()) {
            if (auto* s = iniColl->GetSetting("f3PArrowTiltUpAngle:Combat"))
                tiltDeg = s->GetFloat();
        }
        constexpr float kDeg2Rad = 0.0174532925f;
        const float tc = std::cos(tiltDeg * kDeg2Rad);
        const float ts = std::sin(tiltDeg * kDeg2Rad);
        const RE::NiPoint3 fireDir{
            aimDirRaw.x * tc + aimUp.x * ts,
            aimDirRaw.y * tc + aimUp.y * ts,
            aimDirRaw.z * tc + aimUp.z * ts
        };

        // Initial velocity. The detour's actual fire-time write is
        //     velScalar = s2 * baseSpeed * power * speedMult
        // where `power` is the 0..1 draw-strength at release. We cache
        // s2*speedMult per ammo FormID (without `power`) and apply
        // CURRENT draw progress here so the predicted trajectory
        // matches what would actually fire if released right now —
        // tap-fires show short arcs, full draws show full arcs, mid-
        // draws show interpolated arcs.
        //
        // Estimate currentPower from the engine's attack state +
        // draw timer (same model as the size taper):
        //   kBowDrawn        → 1.0 (full draw confirmed by engine)
        //   kBowDraw/Attached → linear ramp from 0 over fullDrawSeconds
        //   else             → 1.0 (idle, nothing to scale)
        // velMult = s2 * speedMult — the engine's actual fire computes
        //   velScalar = s2 * data.speed * power * speedMult
        // s2 and speedMult require a Projectile* (no projectile exists
        // before fire), so we can't compute them directly during draw.
        // Default to 0.7 — the empirically observed s2*speedMult for
        // vanilla bows + iron arrows. The detour caches the actual
        // value after each fire; subsequent shots use the per-ammo
        // cached value if the ammo type matches. This makes the FIRST
        // shot of a session land where the trail predicts for any
        // vanilla setup, and self-corrects to exact for modded ones.
        constexpr float kDefaultVelMult = 0.7f;
        float velMult = kDefaultVelMult;
        std::uint32_t cachedAmmoFid = 0;
        if (ArrowPathDetour::GetLastVelMult(velMult, cachedAmmoFid)) {
            const std::uint32_t curAmmoFid =
                (ammo ? ammo->GetFormID() : 0u);
            if (cachedAmmoFid == 0 || curAmmoFid != cachedAmmoFid) {
                velMult = kDefaultVelMult;
            }
        }

        // Read the engine's LIVE bow-draw timer from PlayerCharacter::unkBA0
        // (top of SSA stack). This is the value the engine itself feeds into
        // its fire-power formula — sampling it directly is the only way to
        // match the actual fired arrow's velocity. Time-based estimation
        // (drawElapsed / fullDrawSeconds) never matched the engine's curve
        // and was producing near-full-power trails on tap-fires regardless
        // of how short the actual draw was.
        // Live engine bowDrawTime, clamped to [0,1].
        const float drawTime = GetLiveBowDrawAmount(ply);
        LogBowTimerRawBytes(ply, frameCounter_);

        // Trail-visibility gate. The engine fires arrows at partial power
        // any time the player releases before bowDrawTime hits 1.0 — and
        // there is NO per-frame readable value that mirrors the engine's
        // partial-draw fire power (no animation-graph variable, no engine
        // field other than unkBA0 itself, which lags reality at fire by
        // an animation-pack-dependent amount). Rather than predict
        // inaccurately, hide the trail until the bow is fully drawn —
        // at which point the engine clamps power to 1.0 and our full-
        // power prediction is exact across every bow, perk, and animation
        // pack. Tap-fires and partial draws simply show no trail.
        constexpr float kFullDrawThreshold = 1.0f;
        // ALSO require the engine's own fully-drawn ATTACK STATE. Some
        // animation packs push a draw-amount entry that reads 1.0 before
        // the draw animation actually completes, which showed the trail
        // early with a full-power prediction the impending tap-fire could
        // not honor ("draws the trace too soon... thinks it is fully
        // charged"). kBowDrawn only sets when the graph finishes the draw,
        // so the pair is honest across packs.
        const bool fullyDrawnState =
            ply->AsActorState()->actorState1.meleeAttackState ==
            RE::ATTACK_STATE_ENUM::kBowDrawn;
        if (!fullyDrawnState || drawTime < kFullDrawThreshold) {
            // Not fully drawn — clear the published trajectory, hide the
            // trail, and reset the smoothed reticle so we don't carry
            // stale full-draw state into the next full-draw event.
            {
                std::lock_guard<std::mutex> lock(trajectoryMutex_);
                trajectoryHud_.clear();
            }
            showTrajectory_ = false;
            crosshairOverrideActive_.store(false, std::memory_order_release);
            smoothedValid_ = false;
            return false;
        }

        // We only render the trail at full draw (drawTime >= 1.0), where the
        // engine clamps fire power to 1.0. No formula needed — the engine is
        // guaranteed to fire at full power if it fires at all from this state,
        // so the predicted velocity is exact regardless of bow type, perks,
        // or animation pack.
        constexpr float currentPower = 1.0f;
        const float chargeRatio = drawTime;  // legacy alias for size-taper code below
        lastCurrentPower_ = currentPower;
        const float velScalar = ammoSpeed * velMult * currentPower;

        // Per-frame diagnostic. Only fires at full draw (we early-returned
        // before this point if drawTime < 1.0). currentPower is always 1.0.
        {
            const auto attackStateDiag = ply
                ? ply->AsActorState()->actorState1.meleeAttackState
                : RE::ATTACK_STATE_ENUM::kNone;
            const auto snap = GetLiveBowDrawSnapshot(ply);
            spdlog::info(
                "[Predict] frame={} attackState={} stackSize={} rawBowTime={:.3f} "
                "drawTime={:.3f} velScalar={:.1f}",
                frameCounter_, static_cast<int>(attackStateDiag),
                snap.size, snap.raw, drawTime, velScalar);
        }
        RE::NiPoint3 vel{
            fireDir.x * velScalar,
            fireDir.y * velScalar,
            fireDir.z * velScalar
        };

        // Fire origin = arrow-node world position rotated to the
        // camera's yaw. The arrow node is parented to the bow, which
        // rides the player body — so its world position reflects body
        // yaw, not camera yaw. When the camera and body yaw diverge
        // (free-rotation off, body still mid-turn animation, off-
        // shoulder offsets) the body-relative bow node is on the
        // wrong side relative to the camera, and predictions issued
        // from there miss laterally. SmoothCam compensates with the
        // same Z-axis rotation around the player's pivot
        // (`crosshair.cpp:275-291`).
        //
        // We then nudge ~50 units forward along the firing direction
        // so the first raycast clears the player's collision capsule.
        // Without our own shooter-filtered RayCollector this is the
        // simplest way to avoid spurious self-hits; skipping early
        // segments instead caused close-range targets (~120-700 units)
        // to be missed entirely, since each segment at draw speed
        // covers ~125 units.
        // Start the integrator from the RAW arrow node position — same as
        // a_proj->GetPosition() in the detour at fire time. Previously we
        // translated this around the player's center to camera-yaw and
        // nudged forward 50 units, but both introduced a constant offset
        // between predicted endpoint and actual arrow landing. Skipping
        // the first raycast in the loop below keeps the player's collision
        // capsule from generating false hits without distorting the arc.
        const RE::NiPoint3 firePos{
            arrowNode->world.translate.x,
            arrowNode->world.translate.y,
            arrowNode->world.translate.z
        };

        // Cell gravity. Defaults to the standard engine value (-9.8 in
        // havok units; engine pre-scales internally).
        RE::NiPoint3 gravity{ 0.0f, 0.0f, -9.8f };
        if (auto* cell = ply->GetParentCell()) {
            if (auto* world = cell->GetbhkWorld()) {
                if (auto* w1 = world->GetWorld1()) {
                    gravity.x = w1->gravity.quad.m128_f32[0];
                    gravity.y = w1->gravity.quad.m128_f32[1];
                    gravity.z = w1->gravity.quad.m128_f32[2];
                }
            }
        }
        const float gravityScale = ammoGravity;

        // Integrate. dt = 1/20s per SmoothCam — coarse but enough for a
        // visually faithful arc, and the per-segment raycast catches
        // hits between sample points.
        constexpr int kSegCount = 128;
        constexpr float kDt       = 1.0f / 20.0f;
        constexpr float kMaxRange = 8000.0f;

        auto* cell = ply->GetParentCell();
        if (!cell) return false;
        auto* bhkW = cell->GetbhkWorld();
        if (!bhkW) return false;
        auto* world = bhkW->GetWorld1();  // hkpWorld* — exposes CastRay()
        if (!world) return false;
        const float worldScale = RE::bhkWorld::GetWorldScale();

        RE::NiPoint3 prevPos = firePos;
        RE::NiPoint3 curPos  = firePos;
        bool         hit     = false;
        RE::NiPoint3 hitPos{};
        // World-space polyline mirror of the HUD trajectory. Captured
        // every other segment (same density as HUD output) so the
        // arrow-fire event handler can snapshot the integrated curve
        // and re-project it during the post-fire hold window.
        std::vector<RE::NiPoint3> worldPath;
        worldPath.reserve(64);
        worldPath.push_back(firePos);

        // Build the trajectory polyline into a local buffer; we'll
        // publish it atomically under the mutex at the end so the
        // render thread never sees a half-written vector. Keeps Tick
        // and RenderTrajectoryHud safe on different threads.
        std::vector<float> localTrajectory;
        localTrajectory.reserve(kSegCount);

        for (int i = 0; i < kSegCount; ++i) {
            TickPath(curPos, vel, gravity, gravityScale, kDt);

            // Skip the first segment's raycast — starting at the bow node
            // means prevPos is inside the player's collision capsule, so
            // the very first ray would always self-hit. After one step
            // the arrow has cleared the capsule (~125 units forward) and
            // raycasts work normally.
            bool segmentHit = false;
            if (i > 0) {
                RE::hkpWorldRayCastInput  input;
                RE::hkpWorldRayCastOutput output;
                input.from.quad = _mm_setr_ps(prevPos.x * worldScale,
                                              prevPos.y * worldScale,
                                              prevPos.z * worldScale, 0.0f);
                input.to.quad   = _mm_setr_ps(curPos.x  * worldScale,
                                              curPos.y  * worldScale,
                                              curPos.z  * worldScale, 0.0f);
                input.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kCameraSphere);
                input.enableShapeCollectionFilter = false;
                world->CastRay(input, output);
                segmentHit = output.HasHit();
                if (segmentHit) {
                    const float frac = output.hitFraction;
                    hitPos = RE::NiPoint3{
                        prevPos.x + (curPos.x - prevPos.x) * frac,
                        prevPos.y + (curPos.y - prevPos.y) * frac,
                        prevPos.z + (curPos.z - prevPos.z) * frac
                    };
                }
            }

            if (segmentHit) {
                hit = true;
                {
                    double hx = 0.0, hy = 0.0;
                    if (ProjectWorldPointToHUD(prevPos, hx, hy)) {
                        localTrajectory.push_back(static_cast<float>(hx));
                        localTrajectory.push_back(static_cast<float>(hy));
                    }
                    if (ProjectWorldPointToHUD(hitPos, hx, hy)) {
                        localTrajectory.push_back(static_cast<float>(hx));
                        localTrajectory.push_back(static_cast<float>(hy));
                    }
                }
                worldPath.push_back(hitPos);
                break;
            }

            // Capture this segment's endpoint for the HUD line. Skip
            // every other segment to keep the line count modest (~64).
            if ((i & 1) == 0) {
                double hx = 0.0, hy = 0.0;
                if (ProjectWorldPointToHUD(curPos, hx, hy)) {
                    localTrajectory.push_back(static_cast<float>(hx));
                    localTrajectory.push_back(static_cast<float>(hy));
                }
                worldPath.push_back(curPos);
            }

            // Bail if the arrow has dropped well below the player's
            // foot level. At low draw power the integrator's vertical
            // drop dominates and the polyline would otherwise extend
            // off-screen toward the bottom — projecting nearby
            // below-camera world points produces a long vertical line
            // that visually looks like a full-arc trail. Capping at
            // foot level matches what the actual arrow does (plops
            // on the ground a few feet ahead).
            const float footZ = ply->GetPositionZ();
            if (curPos.z < footZ - 5.0f) break;

            // Bail if we've flown past the max prediction range —
            // segments beyond are wasted work.
            const float dx = curPos.x - firePos.x;
            const float dy = curPos.y - firePos.y;
            const float dz = curPos.z - firePos.z;
            const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > kMaxRange) break;

            prevPos = curPos;
        }

        // Visually truncate by currentPower so the rendered polyline
        // length scales linearly with draw amount. Without this, the
        // perspective projection of low-power-but-long-vertical-drop
        // trajectories produces a long visual line that LOOKS like a
        // full arc on screen. Truncating here gives the user the
        // visual progressive growth they expect.
        // No additional visual truncation. velScalar above already
        // applied currentPower, so the integrator produces an arc whose
        // length matches the actual fired velocity. Truncating again by
        // currentPower was double-counting and shrinking the trail to
        // ~38% of an already-half-length arc at half draw.
        const std::size_t fullPairs = localTrajectory.size() / 2;
        spdlog::info(
            "[Predict] -> pairs={} hit={} dist={:.1f}",
            fullPairs, hit ? 1 : 0,
            hit ? std::sqrt((hitPos.x - firePos.x) * (hitPos.x - firePos.x) +
                            (hitPos.y - firePos.y) * (hitPos.y - firePos.y) +
                            (hitPos.z - firePos.z) * (hitPos.z - firePos.z)) : 0.0f);
        {
            std::lock_guard<std::mutex> lock(trajectoryMutex_);
            trajectoryHud_ = std::move(localTrajectory);
        }
        // Publish the world-space mirror so the arrow-fire event can
        // snapshot the integrated curve.
        {
            std::lock_guard<std::mutex> lock(lastArrowWorldPathMutex_);
            lastArrowWorldPath_ = std::move(worldPath);
        }

        if (!hit) {
            // No surface hit within range — point at the curve endpoint
            // anyway so the player sees where the arrow falls. Keeping
            // the crosshair visible is critical for open-vista shots.
            hitPos  = curPos;
            outDist = kMaxRange;
        } else {
            const float dx = hitPos.x - firePos.x;
            const float dy = hitPos.y - firePos.y;
            const float dz = hitPos.z - firePos.z;
            outDist = std::sqrt(dx*dx + dy*dy + dz*dz);
        }

        // Project to HUD-local coords. On failure (no camera, behind
        // camera, no HUD movie) the fallback values written at function
        // entry (base_.x, base_.y) remain — crosshair stays at vanilla
        // position rather than vanishing.
        if (!ProjectWorldPointToHUD(hitPos, outScreenX, outScreenY)) return false;
        return hit;
    }

    void CrosshairManager::Tick()
    {
        ++frameCounter_;
        // Reset every frame; the bow path is the only one that re-arms
        // them. Any early-return path below leaves the overlay dark.
        showTrajectory_ = false;

        // Re-check SmoothCam ownership periodically (every 60 frames) so
        // late-loaded ownership transitions take effect without us doing
        // anything per-frame fancy.
        if (smoothCamIface_ &&
            (ownershipCheckDirty_ ||
             (frameCounter_ - lastOwnerCheckFrame_) >= 60))
        {
            smoothCamOwns_       = SmoothCamOwnsCrosshair();
            ownershipCheckDirty_ = false;
            lastOwnerCheckFrame_ = frameCounter_;
        }
        if (smoothCamOwns_) {
            // SmoothCam owns the crosshair — it also owns sneak meter
            // policy. Stand down on both.
            if (baselineCaptured_) RestoreStealthMeter();
            crosshairOverrideActive_.store(false, std::memory_order_release);
            return;
        }

        // Master toggle. The camera-aim snapshot drives BOTH archery
        // and spell-projectile true-aim (different vtables, same
        // snapshot source), so we only restore vanilla + invalidate
        // the snapshot when BOTH features are off. With only one off,
        // we keep publishing the snapshot so the other detour still
        // gets a valid target — the downstream archery prediction UI
        // is gated on bow-draw state below and is a no-op for spell
        // casting anyway.
        {
            const auto& sCfg = SettingsManager::GetSingleton();
            const bool anyTracingOn = sCfg.archeryTracingEnabled || sCfg.spellTracingEnabled;
            if (!anyTracingOn) {
                if (baselineCaptured_) {
                    RestoreCrosshair();
                    RestoreStealthMeter();
                }
                smoothedValid_ = false;
                crosshairOverrideActive_.store(false, std::memory_order_release);
                ArrowPathDetour::InvalidateCameraSnapshot();
                // No anchors to learn with tracing off, but the ring still
                // has to be kept empty or turning tracing back on replays
                // everything cast while it was off.
                DiscardPendingFireEvents(false);
                return;
            }
        }

        // Target Lock: hand the crosshair back to the engine / TDM.
        // TDM draws its own lock indicator on the enemy, which would
        // fight our predicted-impact reticle. Restore vanilla and
        // bail before any prediction or smoothing runs. Also invalidate
        // the arrow-path camera snapshot — TDM does its own arrow
        // steering toward the locked target, so the detour must fall
        // through instead of overriding with our last-published
        // (pre-lock) target point.
        const bool tdmLocked = TDMIntegration::GetSingleton().IsTargetLocked();
        if (tdmLocked) {
            if (baselineCaptured_) {
                RestoreCrosshair();
                RestoreStealthMeter();
            }
            smoothedValid_ = false;
            crosshairOverrideActive_.store(false, std::memory_order_release);
            ArrowPathDetour::InvalidateCameraSnapshot();
            lastTDMLocked_ = true;
            // Consume anything fired during the lock RIGHT NOW, while we
            // still know we are locked. This is the fix for traces appearing
            // after a lock-off kill — see DiscardPendingFireEvents.
            DiscardPendingFireEvents(SettingsManager::GetSingleton().spellTracingEnabled);
            return;
        }
        // Falling edge of TDM lock — RestoreCrosshair wrote
        // DisplayInfo.SetVisible(false) every frame during the lock and
        // that flag survives on the GFx Crosshair object. For melee /
        // magic / unarmed (mode==None below), the engine never writes
        // visible=true on its own, so the cursor stays hidden until the
        // user sheathes. Force visible=true once to release the cursor;
        // the engine's natural show/hide takes over from here.
        if (lastTDMLocked_ && !tdmLocked) {
            if (auto* uiNow = RE::UI::GetSingleton()) {
                if (auto hudPtr = uiNow->GetMenu(RE::HUDMenu::MENU_NAME)) {
                    auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
                    if (hud) {
                        auto& runtime = hud->GetRuntimeData();
                        RE::GFxValue ch;
                        if (runtime.root.GetMember("Crosshair", &ch)) {
                            RE::GFxValue::DisplayInfo di;
                            di.SetVisible(true);
                            ch.SetDisplayInfo(di);
                        }
                    }
                }
            }
            // Drop the SetCrosshairEnabled deduper too — engine writes
            // post-lock should land instead of being suppressed.
            haveLastWritten_ = false;
        }
        lastTDMLocked_ = false;

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        if (ui->GameIsPaused()) return;

        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (!playerCam || !playerCam->currentState) return;

        // Operate in third-person / mount / dragon AND first person
        // (2026-08-15: "make projectile tracing work for first person").
        // Other states (dialogue, animated, furniture, vanity, etc.) get
        // vanilla crosshair handling. First person matters twice over:
        // besides enabling 1p tracing, the old 1p early-out skipped
        // DetectAimMode — the ONLY place fired shots age and expire — so
        // a trail alive at the moment of a 3p→1p switch froze on screen
        // forever (the "permanent traces after killing a locked enemy"
        // report: the killing shot's trail entered the buffers right as
        // the lock dropped, then the POV switch stopped the clock).
        const auto stateId = playerCam->currentState->id;
        const bool eligibleState =
            stateId == RE::CameraState::kThirdPerson ||
            stateId == RE::CameraState::kMount       ||
            stateId == RE::CameraState::kDragon      ||
            stateId == RE::CameraState::kFirstPerson;
        if (!eligibleState) {
            if (baselineCaptured_) {
                RestoreCrosshair();
                RestoreStealthMeter();
            }
            crosshairOverrideActive_.store(false, std::memory_order_release);
            ArrowPathDetour::InvalidateCameraSnapshot();
            return;
        }

        // Publish the camera-aim snapshot for ArrowPathDetour. We
        // also raycast forward from the camera here so we can give
        // the detour a true-aim target point: the world position the
        // crosshair is pointing at. Aiming the arrow at that exact
        // point eliminates the bow-vs-camera parallax — arrows land
        // where the crosshair points, not parallel-shifted off-axis.
        {
            RE::NiCamera* publishCam = nullptr;
            if (ResolveCameraNi(publishCam) && publishCam) {
                // Strip this frame's noise/Repulse camera offsets: the
                // ENGINE aims and launches from the clean basis, so a
                // snapshot read off the rendered (kicked) matrix bent every
                // aim for ~0.4s after each shot — arrows redirected along
                // the kick and traces went vertically inaccurate during
                // rapid fire ("trouble with verticals from repulse").
                RE::NiMatrix3 pm = publishCam->world.rotate;
                RE::NiPoint3  pt = publishCam->world.translate;
                // The applied-offset strip is 3p-only bookkeeping — in
                // first person the ledger holds the LAST 3p frame's
                // offsets, and subtracting stale values would bend the
                // published aim (2026-08-15, 1p tracing).
                if (stateId != RE::CameraState::kFirstPerson) {
                    RE::NiMatrix3 nRot;
                    RE::NiPoint3  nTr;
                    if (CameraNoiseController::GetAppliedCameraOffset3p(nRot, nTr)) {
                        pm = MulByTranspose(pm, nRot);
                        pt = pt - nTr;
                    }
                }
                const RE::NiPoint3 fwd{ pm.entry[0][0], pm.entry[1][0], pm.entry[2][0] };

                // Raycast forward 8000 units from the camera. If
                // anything is hit, that's the target. Otherwise the
                // ray endpoint is the target (a 'point at infinity').
                constexpr float kAimRayLen = 8000.0f;
                RE::NiPoint3 target{
                    pt.x + fwd.x * kAimRayLen,
                    pt.y + fwd.y * kAimRayLen,
                    pt.z + fwd.z * kAimRayLen,
                };
                if (auto* ply = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* cell = ply->GetParentCell()) {
                        if (auto* bhkW = cell->GetbhkWorld()) {
                            float frac = 1.0f;
                            if (AimRayCastSkipProjectiles(bhkW, pt, target, frac)) {
                                target = RE::NiPoint3{
                                    pt.x + fwd.x * (kAimRayLen * frac),
                                    pt.y + fwd.y * (kAimRayLen * frac),
                                    pt.z + fwd.z * (kAimRayLen * frac),
                                };
                            }
                        }
                    }
                }

                ArrowPathDetour::PublishCameraSnapshot(
                    fwd.x, fwd.y, fwd.z,
                    pm.entry[0][1], pm.entry[1][1], pm.entry[2][1],
                    pt.x, pt.y, pt.z,
                    target.x, target.y, target.z);
            }
        }

        // HUD must be loaded before we can do anything.
        if (!ui->IsMenuOpen(RE::HUDMenu::MENU_NAME)) return;

        if (!EnsureBaseline()) return;
        if (!base_.valid)      return;

        const AimMode mode = DetectAimMode();
        const bool    modeChanged = (mode != lastMode_);
        lastMode_ = mode;

        // Live magic previews are repopulated in the magic branch
        // below. Clear here so non-magic frames render none.
        if (mode != AimMode::Magic) {
            std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
            liveSpellPreviews_.clear();
        }

        // Sneak-meter relocation — tied to SNEAK STATE ONLY, not
        // weapon mode. Earlier the relocation was gated on
        // mode==Bow||Magic, which created flicker for spells: rapid
        // charge → fire → in-flight → expire cycles flip mode through
        // Magic→Magic→None→Magic repeatedly, and the eye would snap
        // back to vanilla each time mode hit None. Tying purely to
        // sneak state means one transition per sneak edge — no flicker
        // during weapon-mode churn.
        //
        // State machine:
        //   - Sneaking: position at offset every frame, alpha=100.
        //   - Not sneaking: restore on the falling edge, alpha=0.
        // The vanilla engine fade-in/out is overridden by our alpha
        // writes, but engine POSITION writes outside sneak transitions
        // are left alone — no Crosshair-widget reset cascade fires
        // from us.
        {
            auto* plyForSneak = RE::PlayerCharacter::GetSingleton();
            const bool isSneaking = plyForSneak && plyForSneak->IsSneaking();
            // Detect IsSneaking edge — engine fires HUDMenu reset on
            // both rising and falling. Snapshot the frame so we can
            // mask the cursor for the duration of the engine's reset
            // cascade.
            if (isSneaking != lastIsSneaking_) {
                lastSneakChangeFrame_ = frameCounter_;
                sneakChangePending_   = true;
            }
            lastIsSneaking_ = isSneaking;

            // The relocation exists FOR projectile tracing (moving the eye
            // out of the trace's way). With both tracing toggles off, DDC
            // must hand the sneak eye back to the engine entirely — no
            // position writes, no alpha writes (user request 2026-08-15).
            const auto& sTrace = SettingsManager::GetSingleton();
            const bool tracingOn =
                sTrace.archeryTracingEnabled || sTrace.spellTracingEnabled;

            const bool wantOffset = tracingOn && base_.stealthValid && isSneaking;
            if (wantOffset) {
                const auto& s   = SettingsManager::GetSingleton();
                const double dx = static_cast<double>(s.sneakMeterOffsetX);
                const double dy = static_cast<double>(s.sneakMeterOffsetY);
                const double targetSx = base_.stealthX + dx;
                const double targetSy = base_.stealthY + dy;
                WriteStealthMeterPosition(targetSx, targetSy);
                stealthMeterRelocated_ = true;
                {
                    std::lock_guard<std::mutex> lock(poseMutex_);
                    echoStealthSx_ = targetSx;
                    echoStealthSy_ = targetSy;
                }
                stealthEchoActive_.store(true, std::memory_order_release);
            } else if (stealthMeterRelocated_) {
                RestoreStealthMeter();
                stealthEchoActive_.store(false, std::memory_order_release);
                // Handing back mid-sneak: leave the eye visible so the
                // engine's own animation resumes from a sane state.
                if (!tracingOn && isSneaking) WriteStealthMeterAlpha(100.0);
            }

            // Alpha is the source of truth for visibility WHILE the
            // relocation owns the eye. With tracing off the engine's own
            // fade animation runs untouched.
            if (tracingOn) WriteStealthMeterAlpha(isSneaking ? 100.0 : 0.0);
        }

        if (mode == AimMode::None) {
            // Live magic previews only exist during charging — clear
            // them when we leave magic mode so they don't stick on
            // screen.
            {
                std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
                liveSpellPreviews_.clear();
            }
            // Restore the baseline ONCE on the transition into None
            // (e.g., bow sheathed). Then stop touching the crosshair
            // entirely so the engine's vanilla hide path can take
            // over — sheathing while sneaking, the engine moves the
            // crosshair offscreen / hides it; if we keep writing
            // base_.x/base_.y every frame, the regular cursor gets
            // pinned at center and overlaps the sneak eye.
            if (modeChanged) {
                RestoreCrosshair();
                spdlog::info("[CrosshairManager] mode -> None (sheathed/no-cast)");
            }
            // Forget the smoothed pose so next entry into bow/magic
            // doesn't lerp from a stale draw position over the new
            // target.
            smoothedValid_         = false;
            stringWasTautLast_     = false;
            crosshairOverrideActive_.store(false, std::memory_order_release);
            return;
        }

        // Aim active — do the raycast(s).
        bool   hit  = false;
        float  dist = kRayLen;
        double sx   = base_.x;
        double sy   = base_.y;

        // Bow draw timer arms only when the player is actually pulling
        // the string (engine attack-state in the kBow* family) — not
        // merely when the bow is equipped. Equip-only would shrink the
        // reticle the moment you readied a bow, which is the wrong
        // signal: the taper is feedback that the *current shot* is
        // approaching full power.
        const auto nowSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        bool stringTautForTimer = false;
        if (mode == AimMode::Bow) {
            if (auto* plyForTimer = RE::PlayerCharacter::GetSingleton()) {
                const auto attackState = plyForTimer->AsActorState()->actorState1.meleeAttackState;
                stringTautForTimer =
                    attackState == RE::ATTACK_STATE_ENUM::kBowDraw    ||
                    attackState == RE::ATTACK_STATE_ENUM::kBowAttached||
                    attackState == RE::ATTACK_STATE_ENUM::kBowDrawn;
            }
        }
        if (!stringTautForTimer) {
            bowDrawTimerArmed_ = false;
        } else if (!bowDrawTimerArmed_) {
            bowDrawStartTime_ = nowSec;
            bowDrawTimerArmed_ = true;
        }

        // Pre-clear the published trajectory under the mutex; if we
        // take the magic / no-bow path the render thread sees an empty
        // polyline. PredictArrowImpact will repopulate it on the bow
        // path.
        {
            std::lock_guard<std::mutex> lock(trajectoryMutex_);
            trajectoryHud_.clear();
        }
        double size = 1.0;

        if (mode == AimMode::Magic) {
            // Rebuild live spell previews — one entry per actively-
            // charging caster (supports dual-cast / both-hands).
            // RenderTrajectoryHud renders each entry independently
            // with full visibility (no fade).
            std::vector<LiveSpellPreview> previews;
            if (magicWasCharging_ || magicReleaseBridge_) {
                RE::NiPoint3 camFwd, camUp, camPos, target;
                if (auto* ply = RE::PlayerCharacter::GetSingleton();
                    ply && ArrowPathDetour::ReadCameraSnapshot(camFwd, camUp, camPos, target))
                {
                    using CState = RE::MagicCaster::State;
                    // Broadened charging check — includes kCasting too,
                    // since one hand can momentarily flip into kCasting
                    // during a dual-hand release while the other is still
                    // charging. Without this both hands would alternately
                    // disappear from the preview during the fire frames.
                    auto chargingHostile = [](RE::MagicCaster* c) -> bool {
                        if (!c) return false;
                        const auto cs = c->state.get();
                        if (cs == CState::kNone)    return false;
                        if (cs == CState::kUnk07 || cs == CState::kUnk08 || cs == CState::kUnk09)
                            return false;
                        auto* spell = c->currentSpell;
                        if (!spell) return false;
                        if (spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget)
                            return false;
                        for (auto* e : spell->effects) {
                            if (e && e->baseEffect && e->baseEffect->IsHostile())
                                return true;
                        }
                        return false;
                    };

                    // Fallback magic node lookup by bone name — covers
                    // the case where caster->GetMagicNode() returns
                    // null transiently during state changes.
                    auto resolveHandPos = [&](RE::MagicCaster* c,
                                              RE::MagicSystem::CastingSource src) -> RE::NiPoint3 {
                        if (auto* n = c->GetMagicNode()) return n->world.translate;
                        if (auto* root = ply->Get3D()) {
                            const RE::BSFixedString kRMag{ "NPC R MagicNode [RMag]" };
                            const RE::BSFixedString kLMag{ "NPC L MagicNode [LMag]" };
                            const auto& name = (src == RE::MagicSystem::CastingSource::kLeftHand)
                                                 ? kLMag : kRMag;
                            if (auto* obj = root->GetObjectByName(name)) {
                                return obj->world.translate;
                            }
                        }
                        return camPos;  // last-resort: camera position
                    };
                    // Compute charge progress for a caster: 0 = just
                    // started, 1 = fully charged (kReady-equivalent).
                    // castingTimer counts DOWN from chargeTime to 0.
                    auto chargeProgressFor = [](RE::MagicCaster* c) -> float {
                        if (!c) return 0.0f;
                        using CState = RE::MagicCaster::State;
                        const auto cs = c->state.get();
                        if (cs == CState::kReady)   return 1.0f;
                        if (cs == CState::kCasting) return 1.0f;
                        if (auto* spell = c->currentSpell) {
                            if (auto* s = spell->As<RE::SpellItem>()) {
                                const float ct = (std::max)(0.1f, s->GetChargeTime());
                                return std::clamp(1.0f - c->castingTimer / ct, 0.0f, 1.0f);
                            }
                        }
                        return 0.5f;  // fallback when timer/spell unavailable
                    };
                    int srcIdx = 0;
                    for (auto src : { RE::MagicSystem::CastingSource::kRightHand,
                                      RE::MagicSystem::CastingSource::kLeftHand,
                                      RE::MagicSystem::CastingSource::kOther,
                                      RE::MagicSystem::CastingSource::kInstant })
                    {
                        auto* caster = ply->GetMagicCaster(src);
                        const bool isCharging = chargingHostile(caster);

                        // (Release-edge capture moved to HookedThirdPerson-
                        // Update — see HookManager.cpp. Capturing there
                        // happens BEFORE camera update finalizes niCam
                        // for the frame, so we get last-frame's niCam =
                        // what the user saw when they clicked. Capturing
                        // here at Tick = end of camera update = 1 frame
                        // newer than user intent.)
                        (void)perHandWasChargingHostile_;
                        (void)perHandSawFiringInCycle_;

                        // Per-hand eager push was removed 2026-05-21.
                        // It produced a DUPLICATE trail entry at a slightly
                        // different direction than the detour's actual fire
                        // event, because the eager push read niCam AFTER
                        // camera update (θ_(R+1)) while the detour reads
                        // niCam DURING actor update (θ_R, before camera
                        // R+1 ran). At fast spin the two trails diverged
                        // by one frame's worth of rotation — visible as
                        // "trail behind projectile". The detour-event
                        // drain in DetectAimMode already runs in the same
                        // Tick as the actor-update detour fire, so the
                        // trail appears in the same render frame as the
                        // projectile — no need for a separate eager path.

                        if (isCharging) {
                            // Update debounce state.
                            casterDebounce_[srcIdx].lastSeenSec = nowSec;
                            casterDebounce_[srcIdx].lastStartPos = resolveHandPos(caster, src);
                            casterDebounce_[srcIdx].lastCharge = chargeProgressFor(caster);
                        }
                        // Show if currently charging OR seen within
                        // the debounce window (100ms grace) — handles
                        // brief state-machine dropouts during dual-
                        // hand charging without flickering.
                        constexpr float kGrace = 0.10f;
                        const bool inGrace = (nowSec - casterDebounce_[srcIdx].lastSeenSec) < kGrace;
                        if (isCharging || inGrace) {
                            RE::NiPoint3 startPos = isCharging
                                ? resolveHandPos(caster, src)
                                : casterDebounce_[srcIdx].lastStartPos;
                            // HONEST ORIGIN: predict from where the spell
                            // will actually RELEASE (learned from real
                            // fires), not where the hand happens to hang
                            // mid-charge — the charge pose and the walk
                            // gait both move the live node, which is what
                            // made the trace lie until release. Falls back
                            // to the live node until this source has fired
                            // once in this stance/POV.
                            if (RE::NiPoint3 predicted{};
                                PredictedReleaseOrigin(static_cast<int>(src), predicted)) {
                                startPos = predicted;
                            }
                            // Sneak / close-quarters guard — same as
                            // MissileProjectileDetour. When target is
                            // too close along camera-forward, drawing
                            // from hand → target points backward
                            // through the player. Match the detour's
                            // parallel-aim fallback so prediction
                            // tracks where the spell will actually go.
                            // Anchor fallback endPt on camera-forward
                            // from CAMERA, not from startPos. A line
                            // (startPos → startPos + camFwd*N) is
                            // parallel to the camera view axis, which
                            // projects to a single screen point —
                            // visually invisible. Using camPos as the
                            // anchor gives endPt a position that's
                            // laterally offset from startPos, so the
                            // line has visible on-screen length.
                            // Try the published camera-cast target first
                            // (the standing case — works correctly there
                            // because the camera sits far enough behind
                            // the player that the ray doesn't self-hit).
                            // If `target` ends up at or behind the hand
                            // along camFwd (sneak case where the camera
                            // sits close to the player and the ray
                            // self-hits the char-controller), fall back
                            // to a fresh raycast FROM THE HAND along
                            // camFwd. The hand sits forward of the
                            // body, so the hand-cast ray bypasses the
                            // player capsule and lands on the actual
                            // scene geometry — which is where the spell
                            // will actually impact (the detour fires
                            // along camFwd from the hand in the same
                            // fallback case).
                            constexpr float kMinForwardDist = 500.0f;
                            const float spellFwdDist =
                                (target.x - startPos.x) * camFwd.x +
                                (target.y - startPos.y) * camFwd.y +
                                (target.z - startPos.z) * camFwd.z;
                            RE::NiPoint3 endPt = target;
                            bool usedHandCast = false;
                            if (spellFwdDist < kMinForwardDist) {
                                constexpr float kHandRayLen = 8000.0f;
                                endPt = RE::NiPoint3{
                                    startPos.x + camFwd.x * kHandRayLen,
                                    startPos.y + camFwd.y * kHandRayLen,
                                    startPos.z + camFwd.z * kHandRayLen,
                                };
                                if (auto* cell = ply->GetParentCell()) {
                                    if (auto* bhkW = cell->GetbhkWorld()) {
                                        float frac = 1.0f;
                                        if (AimRayCastSkipProjectiles(bhkW, startPos, endPt, frac)) {
                                            endPt = RE::NiPoint3{
                                                startPos.x + camFwd.x * (kHandRayLen * frac),
                                                startPos.y + camFwd.y * (kHandRayLen * frac),
                                                startPos.z + camFwd.z * (kHandRayLen * frac),
                                            };
                                        }
                                    }
                                }
                                usedHandCast = true;
                            }
                            // Cache the impact distance along camFwd. The
                            // renderer re-derives the endpoint each frame
                            // from the CURRENT camera forward, so fast
                            // spinning doesn't leave the trace pointing at
                            // where the camera USED to look. The length
                            // (cached this Tick) only refreshes when the
                            // raycast next runs, but the direction tracks
                            // the live camera.
                            //
                            // True-aim case (standing, !usedHandCast): the
                            // fired projectile flies hand -> `target`, where
                            // `target` is the CAMERA-ray impact point. To
                            // match that exactly (so the preview doesn't
                            // snap on release), anchor the endpoint at the
                            // camera ray: cache the CAMERA->impact distance
                            // and flag anchorAtCamera. The renderer rebuilds
                            // the endpoint as camPos + camFwd*dist, giving
                            // the same convergent true-aim line the fired
                            // shot draws.
                            //
                            // Close-range case (usedHandCast): the engine
                            // fires parallel-from-hand there, so keep the
                            // hand-relative distance + hand anchor.
                            const float cachedDist = usedHandCast
                                ? ((endPt.x - startPos.x) * camFwd.x +
                                   (endPt.y - startPos.y) * camFwd.y +
                                   (endPt.z - startPos.z) * camFwd.z)
                                : ((endPt.x - camPos.x) * camFwd.x +
                                   (endPt.y - camPos.y) * camFwd.y +
                                   (endPt.z - camPos.z) * camFwd.z);
                            LiveSpellPreview pv;
                            pv.worldPolyline   = { startPos, endPt };
                            pv.chargeProgress  = casterDebounce_[srcIdx].lastCharge;
                            pv.cachedDistance  = (cachedDist > 50.0f) ? cachedDist : 2000.0f;
                            pv.anchorAtCamera  = !usedHandCast;
                            pv.castingSource   = static_cast<int>(src);
                            if (auto* projectile = ChargingProjectile(caster)) {
                                pv.projectileFormID = projectile->GetFormID();
                                const auto& data = projectile->data;
                                if (data.types.any(RE::BGSProjectileData::Type::kMissile) &&
                                    !data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kHitScan)) {
                                    pv.acceleration = SpellTrajectory::Acceleration<RE::NiPoint3>(data.gravity);
                                    pv.integrationStep = SpellTrajectory::IntegrationStep(RE::GetSecondsSinceLastFrame());
                                    pv.launchSpeed = data.speed * MissileProjectileDetour::GetSpeedMultiplier(pv.projectileFormID);
                                    pv.ballistic = SpellTrajectory::Length(pv.acceleration) > 0.001f &&
                                        std::isfinite(pv.launchSpeed) && pv.launchSpeed > 0.001f;
                                    pv.parallelAim = usedHandCast || stateId == RE::CameraState::kFirstPerson;
                                    if (pv.ballistic) {
                                        const auto velocity = SpellTrajectory::Velocity(startPos, endPt, camFwd, pv.launchSpeed, pv.parallelAim);
                                        auto path = TraceSpellPath(ply->GetParentCell(), startPos, velocity, pv.acceleration, data.range,
                                            pv.integrationStep, MissileProjectileDetour::GetCollisionFilter(projectile));
                                        if (path.points.size() < 2) { ++srcIdx; continue; }
                                        pv.flightTime = path.duration;
                                        pv.worldPolyline = std::move(path.points);
                                        static std::array<unsigned, 4> previewLogCounts{};
                                        static std::array<double, 4> previewLogTimes{};
                                        const int hand = pv.castingSource;
                                        const double stamp = AnchorNowSeconds();
                                        if (hand >= 0 && hand < 4 && pv.chargeProgress >= 0.95f &&
                                            previewLogCounts[hand] < 3 && stamp-previewLogTimes[hand] > 2.0) {
                                            ++previewLogCounts[hand];
                                            previewLogTimes[hand] = stamp;
                                            const auto& end = pv.worldPolyline.back();
                                            spdlog::info("[SpellPreview] projectile={:08X} source={} "
                                                "origin=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f}) "
                                                "step={:.5f} duration={:.4f} hit={} end=({:.3f},{:.3f},{:.3f})",
                                                pv.projectileFormID, hand, startPos.x, startPos.y, startPos.z,
                                                velocity.x, velocity.y, velocity.z, pv.integrationStep, path.duration,
                                                path.hit, end.x, end.y, end.z);
                                        }
                                    }
                                }
                            }
                            previews.push_back(std::move(pv));
                        }
                        ++srcIdx;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
                liveSpellPreviews_ = std::move(previews);
            }
            // Magic mode doesn't use the live trail buffer; the
            // per-caster previews render directly from world coords.
            std::lock_guard<std::mutex> lock(trajectoryMutex_);
            trajectoryHud_.clear();
            showTrajectory_ = false;

            sx = base_.x;
            sy = base_.y;
            hit = false;
            size = 0.0;
            liveBowMarkerScale_ = 1.0f;
        }
        else
        {
            // Bow / Crossbow path — unified with magic. While the
            // string is taut we push a straight-line LiveSpellPreview
            // (arrow node → camera target), identical model to magic.
            // No curved arc, no engine-cursor pinning, no size taper.
            RE::ATTACK_STATE_ENUM attackState = RE::ATTACK_STATE_ENUM::kNone;
            auto* plyForState = RE::PlayerCharacter::GetSingleton();
            if (plyForState) {
                attackState = plyForState->AsActorState()->actorState1.meleeAttackState;
            }
            const bool stringTaut =
                attackState == RE::ATTACK_STATE_ENUM::kBowDraw    ||
                attackState == RE::ATTACK_STATE_ENUM::kBowAttached||
                attackState == RE::ATTACK_STATE_ENUM::kBowDrawn;

            // Release-edge detection. If string was taut last frame
            // but isn't now, an arrow was just released — eagerly
            // push a shot into firedShots_ so the trail and reticle
            // are visible IMMEDIATELY without waiting for the engine
            // to spawn the projectile and the detour to publish.
            // The detour event will arrive a frame or two later and
            // is de-duped against this eager push.
            const bool justReleased = stringWasTautLast_ && !stringTaut;
            stringWasTautLast_ = stringTaut;

            std::vector<LiveSpellPreview> previews;
            float liveChargeProgress = 0.0f;
            if (stringTaut && plyForState) {
                double dummySx = 0.0, dummySy = 0.0;
                float  dummyDist = 0.0f;
                PredictArrowImpact(dummySx, dummySy, dummyDist);
                LiveSpellPreview pv;
                {
                    std::lock_guard<std::mutex> lock(lastArrowWorldPathMutex_);
                    pv.worldPolyline = lastArrowWorldPath_;
                }
                // Compute bow draw progress 0..1 for the reticle.
                if (attackState == RE::ATTACK_STATE_ENUM::kBowDrawn) {
                    liveChargeProgress = 1.0f;
                } else if (bowDrawTimerArmed_) {
                    float weaponSpeed = 1.0f;
                    if (auto* right = plyForState->GetEquippedObject(false)) {
                        if (auto* weap = right->As<RE::TESObjectWEAP>()) {
                            const float ws = weap->weaponData.speed;
                            if (ws > 0.05f) weaponSpeed = ws;
                        }
                    }
                    constexpr float kVanillaFullDrawSeconds = 0.6f;
                    const float fullDrawSeconds = kVanillaFullDrawSeconds / weaponSpeed;
                    const float drawElapsed = ((nowSec > bowDrawStartTime_)
                                                ? (nowSec - bowDrawStartTime_)
                                                : 0.0f);
                    liveChargeProgress = std::clamp(drawElapsed / fullDrawSeconds, 0.0f, 1.0f);
                }
                // Bow draw timer arming (was tracked earlier in Tick,
                // moved here for cleanliness).
                if (!bowDrawTimerArmed_) {
                    bowDrawStartTime_ = nowSec;
                    bowDrawTimerArmed_ = true;
                }
                pv.chargeProgress = liveChargeProgress;
                if (pv.worldPolyline.size() >= 2) {
                    previews.push_back(std::move(pv));
                }
            } else {
                bowDrawTimerArmed_ = false;
            }

            // Eager release-edge push. Rate-limited to one push per
            // 200ms — defensive against any state-machine glitch that
            // could re-trigger the release edge (mode flips, animation
            // packs cycling attackState, etc.).
            const bool wantArrowGate = SettingsManager::GetSingleton().archeryTracingEnabled;
            if (justReleased && wantArrowGate &&
                (nowSec - lastBowFireWallSec_) > 0.2f)
            {
                std::vector<RE::NiPoint3> path;
                {
                    std::lock_guard<std::mutex> lock(lastArrowWorldPathMutex_);
                    path = lastArrowWorldPath_;
                }
                if (path.size() >= 2) {
                    float dist = 0.0f;
                    for (std::size_t i = 1; i < path.size(); ++i) {
                        const auto& a = path[i - 1];
                        const auto& b = path[i];
                        const float dx = b.x - a.x;
                        const float dy = b.y - a.y;
                        const float dz = b.z - a.z;
                        dist += std::sqrt(dx*dx + dy*dy + dz*dz);
                    }
                    // NaN guard — if anything in the polyline produces
                    // a non-finite distance, fall back to a 1s travel.
                    // Without this a NaN travelTime would make
                    // (elapsed > travel+settle) always false → shot
                    // never expires → "permanent" trail.
                    constexpr float kEstSpeed = 3000.0f;
                    float travelTime = 1.0f;
                    if (std::isfinite(dist) && dist > 0.1f) {
                        travelTime = std::clamp(dist / kEstSpeed, 0.05f, 5.0f);
                    }
                    ProjectileShot s;
                    s.worldPolyline     = std::move(path);
                    s.travelTime        = travelTime;
                    s.wallClockFireTime = nowSec;
                    s.elapsedGameTime   = 0.0f;
                    {
                        std::lock_guard<std::mutex> lock(firedShotsMutex_);
                        firedShots_.push_back(std::move(s));
                        if (firedShots_.size() > 16) {
                            firedShots_.erase(firedShots_.begin());
                        }
                    }
                    lastBowFireWallSec_ = nowSec;
                    spdlog::debug("[BowEagerFire] pushed dist={:.0f} travel={:.2f}s",
                                 dist, travelTime);
                }
            }
            {
                std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
                liveSpellPreviews_ = std::move(previews);
            }
            // Clear the legacy bow live trail buffer — rendering now
            // goes through liveSpellPreviews_ instead.
            {
                std::lock_guard<std::mutex> lock(trajectoryMutex_);
                trajectoryHud_.clear();
            }
            showTrajectory_ = false;
            liveBowMarkerScale_ = 1.0f;

            // Hide the engine crosshair while the string is taut OR
            // while any in-flight bow shot is still active in
            // firedShots_. Without the in-flight check, releasing the
            // string instantly brings back the vanilla cursor before
            // the shot's reticle has finished its impact + settle.
            bool hasShots = false;
            {
                std::lock_guard<std::mutex> lock(firedShotsMutex_);
                hasShots = !firedShots_.empty();
            }
            size = (stringTaut || hasShots) ? 0.0 : 1.0;
        }

        // Ease the native crosshair as projectile tracing hides/restores it.
        // The drawn trajectories and impact markers have their own timing.
        // First frame after the bow path enters: initialize smoothed
        // state to target so we don't lerp from stale (0,0,1) values.
        const float dt = smoothedValid_ ? std::clamp(nowSec - lastSmoothTime_, 0.0f, 0.25f) : 0.0f;
        lastSmoothTime_ = nowSec;
        if (!smoothedValid_) {
            smoothedX_     = sx;
            smoothedY_     = sy;
            smoothedSize_  = size;
            smoothedValid_ = true;
        } else {
            // Fixed at the former Smoothing slider's 0.30 setting.
            // Legacy preset values are retained for round-tripping only.
            constexpr float kTau = 0.30f;
            const float alpha = 1.0f - std::exp(-dt / kTau);
            smoothedX_    += (sx   - smoothedX_)    * static_cast<double>(alpha);
            smoothedY_    += (sy   - smoothedY_)    * static_cast<double>(alpha);
            smoothedSize_ += (size - smoothedSize_) * static_cast<double>(alpha);
        }

        // Single-frame hide on the exact sneak-edge tick. The engine
        // fires its HUDMenu reset cascade synchronously when sneak
        // toggles; if we hide the cursor for ONE frame at that edge,
        // the cascade fires inside the hidden frame and never renders
        // the vanilla-position flash. 16ms at 60fps is below human
        // perception threshold for a flicker; the player just sees a
        // smooth handoff. Multi-frame masks (which we tried earlier)
        // were themselves visible.
        const bool sneakEdgeFrame = sneakChangePending_ &&
            (frameCounter_ - lastSneakChangeFrame_) <= 1;
        if (sneakChangePending_ && (frameCounter_ - lastSneakChangeFrame_) > 1) {
            sneakChangePending_ = false;
        }
        cursorMaskActive_.store(sneakEdgeFrame, std::memory_order_release);
        if (sneakEdgeFrame) {
            auto* uiHide = RE::UI::GetSingleton();
            if (uiHide) {
                if (auto hudPtr = uiHide->GetMenu(RE::HUDMenu::MENU_NAME)) {
                    auto* hudHide = static_cast<RE::HUDMenu*>(hudPtr.get());
                    if (hudHide) {
                        auto& runtime = hudHide->GetRuntimeData();
                        RE::GFxValue ch;
                        if (runtime.root.GetMember("Crosshair", &ch)) {
                            RE::GFxValue::DisplayInfo di;
                            di.SetVisible(false);
                            ch.SetDisplayInfo(di);
                        }
                    }
                }
            }
        } else {
            WriteCrosshairScreenPos(smoothedX_, smoothedY_, /*forceVisible=*/true);
            WriteCrosshairScaleXY(base_.xScale * smoothedSize_, base_.yScale * smoothedSize_);
            SetVisibility(true);
        }

        // Save echoed pose for the HudElement callback. RenderTrajectoryHud
        // re-issues this write during HUD render so the engine's
        // un-sneak transition (which resets Crosshair pose for one
        // frame) gets clobbered before the frame ships.
        {
            std::lock_guard<std::mutex> lock(poseMutex_);
            echoSx_     = smoothedX_;
            echoSy_     = smoothedY_;
            echoScaleX_ = base_.xScale * smoothedSize_;
            echoScaleY_ = base_.yScale * smoothedSize_;
        }
        crosshairOverrideActive_.store(true, std::memory_order_release);

        // Mode-change diagnostic + periodic dump (every 60 frames so
        // we get a tighter cadence near a fired shot).
        if (modeChanged ||
            (frameCounter_ % 60u) == 0u)
        {
            spdlog::info(
                "[CrosshairManager] mode={} hit={} dist={:.1f} sx={:.1f} sy={:.1f} drawT={:.2f}",
                (mode == AimMode::Magic) ? "Magic" : "Bow",
                hit ? 1 : 0, dist, sx, sy,
                bowDrawTimerArmed_ ? ((nowSec > bowDrawStartTime_) ? (nowSec - bowDrawStartTime_) : 0.0f) : 0.0f);
        }
    }

    // HudElement callback. The SKSE Menu Framework calls this each
    // frame the HUD is up. We translate the captured trajectory points
    // (in HUDMovieBaseInstance-local coords) into screen pixel coords
    // for ImGui's foreground draw list.
    //
    // ImGui's draw list operates in physical screen pixels. The HUD's
    // visible-frame rect tells us the device-pixel region the HUD is
    // mapped into; that lets us turn HUD-local coords into screen
    // pixels: pixel = (hud + rectMid) * (deviceSize / rectSize).
    void CrosshairManager::RenderTrajectoryHud()
    {
        using namespace ImGuiMCP;
        if (smoothCamOwns_) return;

        // TDM target lock takes over aim — Tick() already stops capturing
        // new shots and previews while locked, but in-flight shots
        // captured before the lock keep rendering trails + cursors here
        // until they age out (~1s). Bail unconditionally so projectile
        // tracing visually deactivates the moment lock engages. Also
        // drop stored shots/previews so they don't replay on lock
        // release as a burst of stale trails.
        {
            const bool tlLockedHud = TDMIntegration::GetSingleton().IsTargetLocked();
            // One extra clear on the RELEASE edge: a shot fired at the very
            // boundary can land in the buffers between the last locked
            // frame and the first unlocked one, which replayed as a stale
            // trail right after locking off (user report 2026-08-15).
            static bool sTlLockedHudPrev = false;
            const bool releaseEdge = !tlLockedHud && sTlLockedHudPrev;
            sTlLockedHudPrev = tlLockedHud;
            if (tlLockedHud || releaseEdge) {
                {
                    std::lock_guard<std::mutex> lock(firedShotsMutex_);
                    firedShots_.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
                    liveSpellPreviews_.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(trajectoryMutex_);
                    trajectoryHud_.clear();
                    showTrajectory_ = false;
                }
                if (tlLockedHud) return;
            }
        }

        // Echo the Crosshair pose write before we touch anything else.
        // The engine resets Crosshair position/scale on certain HUD
        // transitions (notably the un-sneak with-bow handoff) for one
        // render frame. Tick wrote our override earlier this frame,
        // but the engine reset slipped in afterward; re-issuing here
        // (DURING HUD render) clobbers the reset before the frame
        // ships. Cheap one-write fast path: skip when not active.
        if (crosshairOverrideActive_.load(std::memory_order_acquire) &&
            !cursorMaskActive_.load(std::memory_order_acquire))
        {
            double sx, sy, scaleX, scaleY;
            {
                std::lock_guard<std::mutex> lock(poseMutex_);
                sx     = echoSx_;
                sy     = echoSy_;
                scaleX = echoScaleX_;
                scaleY = echoScaleY_;
            }
            WriteCrosshairScreenPos(sx, sy, /*forceVisible=*/true);
            WriteCrosshairScaleXY(scaleX, scaleY);
        }

        // Same echo for the StealthMeter offset — the engine resets
        // the meter to vanilla center for one frame on sneak-entry,
        // producing a brief pop at center before our offset takes
        // effect.
        if (stealthEchoActive_.load(std::memory_order_acquire)) {
            double sSx, sSy;
            {
                std::lock_guard<std::mutex> lock(poseMutex_);
                sSx = echoStealthSx_;
                sSy = echoStealthSy_;
            }
            WriteStealthMeterPosition(sSx, sSy);
        }

        // Snapshot live trail under mutex.
        std::vector<float> points;
        bool               haveLiveTrail = false;
        double cursorSx = 0.0, cursorSy = 0.0;
        {
            std::lock_guard<std::mutex> lock(trajectoryMutex_);
            haveLiveTrail = showTrajectory_ && trajectoryHud_.size() >= 4;
            if (haveLiveTrail) points = trajectoryHud_;
        }
        // Snapshot in-flight shots count + live spell previews count
        // to decide whether to bail.
        bool haveShots = false;
        {
            std::lock_guard<std::mutex> lock(firedShotsMutex_);
            haveShots = !firedShots_.empty();
        }
        bool haveLivePreviews = false;
        {
            std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
            haveLivePreviews = !liveSpellPreviews_.empty();
        }
        if (!haveLiveTrail && !haveShots && !haveLivePreviews) return;

        if (haveLiveTrail) {
            std::lock_guard<std::mutex> lock(poseMutex_);
            cursorSx = echoSx_;
            cursorSy = echoSy_;
        }
        // Anchor the live polyline's last point to the smoothed cursor
        // position so the trail visually terminates at the center of
        // the reticle — bow path only. In Magic mode the engine
        // crosshair stays at vanilla; the live trail's last point
        // already comes from the world-projected target, so we want
        // to keep it (not snap it back to the crosshair).
        if (haveLiveTrail && points.size() >= 2 && lastMode_ != AimMode::Magic) {
            points[points.size() - 2] = static_cast<float>(cursorSx);
            points[points.size() - 1] = static_cast<float>(cursorSy);
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto hudPtr = ui->GetMenu(RE::HUDMenu::MENU_NAME);
        if (!hudPtr) return;
        auto* hud = static_cast<RE::HUDMenu*>(hudPtr.get());
        if (!hud || !hud->uiMovie) return;
        const auto frame = hud->uiMovie->GetVisibleFrameRect();
        const float rectMidX = (frame.right + frame.left) * 0.5f;
        const float rectMidY = (frame.bottom + frame.top) * 0.5f;
        const float rectW    = frame.right - frame.left;
        const float rectH    = frame.bottom - frame.top;
        if (rectW <= 0.0f || rectH <= 0.0f) return;

        // Draw-list coordinates belong to ImGui's display, which need not be
        // the game's internal render resolution (upscaling/window scaling).
        const auto* io = ImGuiMCP::ImGui::GetIO();
        if (!io || io->DisplaySize.x <= 0 || io->DisplaySize.y <= 0) return;
        const float devW = io->DisplaySize.x;
        const float devH = io->DisplaySize.y;
        static bool loggedViewport = false;
        if (!loggedViewport) {
            loggedViewport = true;
            const auto* state = RE::BSGraphics::State::GetSingleton();
            spdlog::info("[SpellTrace] display={:.0f}x{:.0f} game={}x{} HUD=({:.1f},{:.1f},{:.1f},{:.1f})",
                devW, devH, state ? state->screenWidth : 0u, state ? state->screenHeight : 0u,
                frame.left, frame.top, frame.right, frame.bottom);
        }
        const float scaleX = devW / rectW;
        const float scaleY = devH / rectH;

        auto* dl = ImGuiMCP::ImGui::GetForegroundDrawList();
        if (!dl) return;

        const auto& sCfg = SettingsManager::GetSingleton();
        const float reticleSizeScale = std::clamp(sCfg.projectileReticleSizeScale, 0.25f, 6.0f);
        const float reticleThickness = std::clamp(sCfg.projectileReticleThickness, 0.5f, 12.0f);
        // Trail lines draw heavier than the reticle strokes — the thin lines
        // read as faint scratch marks at gameplay distance. Scales with the
        // user's Thickness slider.
        const float trailThickness   = reticleThickness * 1.75f;

        // Live trail render — bow-draw prediction populates `points`
        // each frame. Plain alpha curve, no tail fade (the bow path
        // shows the entire predicted arc while the string is taut).
        const std::size_t pairs = points.size() / 2;
        ImVec2 prev{};
        for (std::size_t i = 0; i < pairs; ++i) {
            const float hx = points[i * 2 + 0];
            const float hy = points[i * 2 + 1];
            // HUD-local (origin at rectMid) → frame-rect coord (origin
            // at rect.left/.top) → device pixel.
            const float frX = (hx - base_.x) + rectMidX;
            const float frY = (hy - base_.y) + rectMidY;
            const ImVec2 px{ (frX - frame.left) * scaleX,
                             (frY - frame.top)  * scaleY };
            if (i > 0) {
                const float t = pairs > 1 ? static_cast<float>(i) / static_cast<float>(pairs - 1) : 0.0f;
                // Fade from low alpha at the start to higher near impact.
                const float alpha = 0.15f + 0.55f * t;
                const ImU32 col = ImGuiMCP::ImGui::ColorConvertFloat4ToU32(
                    ImVec4(1.0f, 1.0f, 1.0f, alpha));
                ImGuiMCP::ImGui::ImDrawListManager::AddLine(dl, prev, px, col, trailThickness);
            }
            prev = px;
        }
        // End-of-trail dot — the bow arc used to just stop mid-air; cap it
        // with a filled dot at the predicted impact, matching the trail's
        // end alpha.
        if (pairs >= 2) {
            const ImU32 dotCol = ImGuiMCP::ImGui::ColorConvertFloat4ToU32(
                ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
            ImGuiMCP::ImGui::ImDrawListManager::AddCircleFilled(
                dl, prev, 3.0f * reticleSizeScale, dotCol, 12);
        }

        // Simple reticle: center dot only. The outer ring was removed
        // (user preference); kept intentionally minimal so the engine HUD
        // doesn't fight visually with our predictive overlay.
        auto drawReticle = [&](ImVec2 center, float baseScale,
                               float thickness, float alpha,
                               float ringRadius)
        {
            (void)thickness;   // outer ring removed -> thickness now unused
            if (alpha < 0.01f) return;
            const ImU32 col = ImGuiMCP::ImGui::ColorConvertFloat4ToU32(
                ImVec4(1.0f, 1.0f, 1.0f, alpha));
            // Keep the dot's show/hide threshold identical to when the ring
            // existed (it gated on the scaled ring radius going sub-pixel).
            const float r = ringRadius * baseScale;
            if (r <= 0.5f) return;
            // A real DOT at the trail's end — the old 1px point vanished at
            // gameplay distance and the trail looked like it just stopped.
            ImGuiMCP::ImGui::ImDrawListManager::AddCircleFilled(
                dl, center, 3.0f * baseScale, col, 12);
        };

        // ----- Fired shots pass (post-fire, fading trail) -----
        std::vector<ProjectileShot> firedShotsCopy;
        {
            std::lock_guard<std::mutex> lock(firedShotsMutex_);
            firedShotsCopy = firedShots_;
        }
        if (!firedShotsCopy.empty()) {
            constexpr float kPostImpactSettle = 1.0f;
            constexpr float kFadeBand = 0.12f;
            // Handoff blend window: how long a fired spell shot eases from
            // the preview's camera-locked line to its frozen world line.
            constexpr float kHandoffBlend = 0.10f;

            // Live camera for the handoff blend (reproduces the preview's
            // camera-locked endpoint for the first kHandoffBlend seconds).
            RE::NiPoint3 liveCamPosF{}, liveCamFwdF{};
            bool haveLiveCamF = false;
            {
                RE::NiCamera* lc = nullptr;
                if (ResolveCameraNi(lc) && lc) {
                    const auto& m = lc->world.rotate;
                    liveCamFwdF = RE::NiPoint3{ m.entry[0][0], m.entry[1][0], m.entry[2][0] };
                    liveCamPosF = lc->world.translate;
                    haveLiveCamF = true;
                }
            }

            for (const auto& shot : firedShotsCopy) {
                if (shot.worldPolyline.size() < 2) continue;
                const float elapsed = shot.elapsedGameTime;

                // Handoff blend: for the first kHandoffBlend seconds,
                // ease the rendered polyline from the preview's CAMERA-
                // LOCKED line (anchor + camFwd*camAnchorDist, live hand)
                // to the frozen WORLD-LOCKED worldPolyline. At elapsed 0
                // this reproduces the preview's last frame exactly (no
                // position jump, even while turning); by kHandoffBlend
                // it's fully world-locked on the true impact.
                std::vector<RE::NiPoint3> renderPoly = shot.worldPolyline;

                // Reticle brightness eases from the preview's 0.85 to the
                // in-flight 1.0 over the handoff window, for EVERY spell
                // shot — independent of the position blend below, which
                // needs an inherited endpoint and is skipped when none
                // exists. This is what actually kills the cursor pop the
                // frame-by-frame log pinned down (a:0.85 -> 1.00 at release).
                float retAlphaMul = 1.0f;
                if (shot.castingSource >= 0 && elapsed < kHandoffBlend) {
                    const float bRaw = std::clamp(elapsed / kHandoffBlend, 0.0f, 1.0f);
                    const float b    = bRaw * bRaw * (3.0f - 2.0f * bRaw);
                    retAlphaMul = 0.85f + 0.15f * b;
                }

                if (shot.camAnchorDist >= 0.0f && haveLiveCamF &&
                    shot.castingSource >= 0 && renderPoly.size() == 2 &&
                    elapsed < kHandoffBlend)
                {
                    const float bRaw = std::clamp(elapsed / kHandoffBlend, 0.0f, 1.0f);
                    const float b    = bRaw * bRaw * (3.0f - 2.0f * bRaw);
                    RE::NiPoint3 liveHand = shot.worldPolyline.front();
                    if (auto* livePly = RE::PlayerCharacter::GetSingleton()) {
                        const auto src = static_cast<RE::MagicSystem::CastingSource>(shot.castingSource);
                        if (auto* caster = livePly->GetMagicCaster(src)) {
                            if (auto* node = caster->GetMagicNode())
                                liveHand = node->world.translate;
                        }
                    }
                    const RE::NiPoint3 anchor = shot.anchorAtCamera ? liveCamPosF : liveHand;
                    const RE::NiPoint3 camLockedEnd{
                        anchor.x + liveCamFwdF.x * shot.camAnchorDist,
                        anchor.y + liveCamFwdF.y * shot.camAnchorDist,
                        anchor.z + liveCamFwdF.z * shot.camAnchorDist,
                    };
                    const RE::NiPoint3 wStart = shot.worldPolyline.front();
                    const RE::NiPoint3 wEnd   = shot.worldPolyline.back();
                    renderPoly.front() = RE::NiPoint3{
                        liveHand.x + (wStart.x - liveHand.x) * b,
                        liveHand.y + (wStart.y - liveHand.y) * b,
                        liveHand.z + (wStart.z - liveHand.z) * b,
                    };
                    renderPoly.back() = RE::NiPoint3{
                        camLockedEnd.x + (wEnd.x - camLockedEnd.x) * b,
                        camLockedEnd.y + (wEnd.y - camLockedEnd.y) * b,
                        camLockedEnd.z + (wEnd.z - camLockedEnd.z) * b,
                    };
                }
                const float fadeProgress = shot.travelTime > 0.001f
                    ? std::clamp(elapsed / shot.travelTime, 0.0f, 1.0f)
                    : 1.0f;
                const float bandStart = fadeProgress - kFadeBand;

                const float settleStart = shot.travelTime;
                const float settleEnd   = shot.travelTime + kPostImpactSettle;
                float cursorAlpha;
                if (elapsed < settleStart) {
                    cursorAlpha = 1.0f;
                } else if (elapsed < settleEnd - 0.3f) {
                    cursorAlpha = 1.0f;
                } else if (elapsed < settleEnd) {
                    cursorAlpha = (settleEnd - elapsed) / 0.3f;
                } else {
                    cursorAlpha = 0.0f;
                }

                // Sampled polyline render — subdivides short polylines
                // (2-point spell shots) so the fade gradient still
                // shows. t = 0..1 across the polyline; each segment
                // fades via the tail-fade band.
                constexpr int kMinSamples = 24;
                const std::size_t polyN = renderPoly.size();
                const int N = (std::max)(kMinSamples, static_cast<int>(polyN));
                ImVec2 prevShot{};
                bool   havePrev = false;
                for (int i = 0; i <= N; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(N);
                    const float idxF = t * static_cast<float>(polyN - 1);
                    const std::size_t i0 = (std::min)(static_cast<std::size_t>(idxF), polyN - 1);
                    const std::size_t i1 = (std::min)(i0 + 1, polyN - 1);
                    const float frac = idxF - static_cast<float>(i0);
                    const auto& a = renderPoly[i0];
                    const auto& b = renderPoly[i1];
                    const RE::NiPoint3 p{
                        a.x + (b.x - a.x) * frac,
                        a.y + (b.y - a.y) * frac,
                        a.z + (b.z - a.z) * frac,
                    };
                    double hx = 0.0, hy = 0.0;
                    if (!ProjectWorldPointToHUD(p, hx, hy)) {
                        havePrev = false;
                        continue;
                    }
                    const float frX = (static_cast<float>(hx) - static_cast<float>(base_.x)) + rectMidX;
                    const float frY = (static_cast<float>(hy) - static_cast<float>(base_.y)) + rectMidY;
                    const ImVec2 px{ (frX - frame.left) * scaleX,
                                     (frY - frame.top)  * scaleY };

                    if (havePrev && i > 0) {
                        float alpha = 0.15f + 0.55f * t;
                        const float bandT = (t - bandStart) / kFadeBand;
                        const float clamped = std::clamp(bandT, 0.0f, 1.0f);
                        const float visibility = clamped * clamped * (3.0f - 2.0f * clamped);
                        alpha *= visibility * cursorAlpha;
                        if (alpha >= 0.005f) {
                            const ImU32 col2 = ImGuiMCP::ImGui::ColorConvertFloat4ToU32(
                                ImVec4(1.0f, 1.0f, 1.0f, alpha));
                            ImGuiMCP::ImGui::ImDrawListManager::AddLine(dl, prevShot, px, col2, trailThickness);
                        }
                    }
                    prevShot = px;
                    havePrev = true;
                }

                // AAA reticle at the polyline's end (impact point).
                // shrinkT = fadeProgress so it shrinks as the
                // projectile approaches its mark.
                if (cursorAlpha > 0.01f) {
                    const auto& impactPt = renderPoly.back();
                    double tx = 0.0, ty = 0.0;
                    if (ProjectWorldPointToHUD(impactPt, tx, ty)) {
                        const float frX = (static_cast<float>(tx) - static_cast<float>(base_.x)) + rectMidX;
                        const float frY = (static_cast<float>(ty) - static_cast<float>(base_.y)) + rectMidY;
                        const ImVec2 center{ (frX - frame.left) * scaleX,
                                             (frY - frame.top)  * scaleY };
                        // Continuous radial tighten — no outward release
                        // bloom. Both ring AND ticks contract together.
                        // At elapsed=0 the values match the live-preview
                        // fully-charged endpoint (ring=6, tick=4), so
                        // the handoff from "drawn / charged" to "in
                        // flight" is seamless. By impact the ring is
                        // at its minimum (2) and ticks have shrunk to
                        // zero — leaving just a small ring + dot to
                        // mark the impact while alpha fades out.
                        const float t = std::clamp(
                            elapsed / (std::max)(0.05f, shot.travelTime),
                            0.0f, 1.0f);
                        const float eased = t * t * (3.0f - 2.0f * t);
                        const float ringR  = 6.0f + (2.0f - 6.0f) * eased;
                        // retAlphaMul eases the cursor from the preview's
                        // 0.85 to 1.0 across the handoff window so it
                        // doesn't pop brighter the instant the shot fires.
                        drawReticle(center, reticleSizeScale, reticleThickness,
                                    cursorAlpha * retAlphaMul, ringR);
                    }
                }
            }
        }

        // ----- Live spell previews pass (full visibility, no fade) -----
        // One entry per actively-charging caster — supports two-hand
        // simultaneous charge (dual-cast) with each hand showing its
        // own trail + cursor.
        std::vector<LiveSpellPreview> previews;
        {
            std::lock_guard<std::mutex> lock(liveSpellPreviewsMutex_);
            previews = liveSpellPreviews_;
        }
        // Live-direction re-anchoring for straight previews only: rebuild the line
        // from the CURRENT camera forward + current hand position so
        // fast spinning between Tick and HUD render doesn't leave the
        // trace pointing where the camera USED to look. Only the
        // direction is live; the length stays at the cached raycast
        // distance from Tick (refreshed each Tick — typically 16ms).
        if (!previews.empty()) {
            RE::NiCamera* liveCam = nullptr;
            if (ResolveCameraNi(liveCam) && liveCam) {
                // Same clean-basis strip as the snapshot publisher: the
                // preview must aim where the ENGINE aims, and the rendered
                // matrix carries the noise/Repulse kick the projectile
                // never sees. (The trace drawing itself still projects
                // through the rendered camera — so during a kick the trace
                // correctly appears displaced from the crosshair, because
                // that IS where the shot will go.)
                RE::NiMatrix3 lcm = liveCam->world.rotate;
                RE::NiPoint3  liveCamPos = liveCam->world.translate;
                {
                    RE::NiMatrix3 nRot;
                    RE::NiPoint3  nTr;
                    const auto* playerCamera = RE::PlayerCamera::GetSingleton();
                    if (playerCamera && !playerCamera->IsInFirstPerson() &&
                        CameraNoiseController::GetAppliedCameraOffset3p(nRot, nTr)) {
                        lcm        = MulByTranspose(lcm, nRot);
                        liveCamPos = liveCamPos - nTr;
                    }
                }
                const RE::NiPoint3 liveCamFwd{
                    lcm.entry[0][0], lcm.entry[1][0], lcm.entry[2][0] };
                auto* livePly = RE::PlayerCharacter::GetSingleton();
                for (auto& pv : previews) {
                    // Preserve every collision-tested point and the exact hit
                    // endpoint. A new direction needs a new game-thread trace;
                    // merely keeping the old flight time misses obstructions.
                    if (pv.ballistic) continue;
                    if (pv.cachedDistance <= 0.0f || pv.castingSource < 0 ||
                        pv.worldPolyline.size() < 2 || !livePly)
                    {
                        continue;
                    }
                    const auto src = static_cast<RE::MagicSystem::CastingSource>(pv.castingSource);
                    auto* caster = livePly->GetMagicCaster(src);
                    RE::NiPoint3 liveHand = pv.worldPolyline.front();
                    bool gotLive = false;
                    if (caster) {
                        if (auto* node = caster->GetMagicNode()) {
                            liveHand = node->world.translate;
                            gotLive = true;
                        }
                    }
                    if (!gotLive) {
                        if (auto* root = livePly->Get3D()) {
                            const RE::BSFixedString kRMag{ "NPC R MagicNode [RMag]" };
                            const RE::BSFixedString kLMag{ "NPC L MagicNode [LMag]" };
                            const auto& nm =
                                (src == RE::MagicSystem::CastingSource::kLeftHand)
                                    ? kLMag : kRMag;
                            if (auto* obj = root->GetObjectByName(nm)) {
                                liveHand = obj->world.translate;
                            }
                        }
                    }
                    // HONEST ORIGIN, render side: same learned release
                    // anchor as the Tick builder, reconstructed at the
                    // player's live position/heading — so strafing and
                    // turning move the trace start smoothly instead of
                    // riding the charge animation's hand wobble.
                    if (RE::NiPoint3 predicted{};
                        PredictedReleaseOrigin(pv.castingSource, predicted)) {
                        liveHand = predicted;
                    }
                    // True-aim previews anchor the endpoint on the live
                    // camera ray (camPos + camFwd*dist) so the line
                    // converges on the actual impact point, matching the
                    // fired projectile. The close-range fallback anchors
                    // the endpoint at the hand (hand + camFwd*dist) for
                    // parallel aim, matching the engine there.
                    const RE::NiPoint3 anchor = pv.anchorAtCamera ? liveCamPos : liveHand;
                    const RE::NiPoint3 liveEnd{
                        anchor.x + liveCamFwd.x * pv.cachedDistance,
                        anchor.y + liveCamFwd.y * pv.cachedDistance,
                        anchor.z + liveCamFwd.z * pv.cachedDistance,
                    };
                    pv.worldPolyline = { liveHand, liveEnd };

                    // Publish this endpoint for the fired-shot handoff. The
                    // shot (created in Tick) inherits the last value the
                    // user actually saw, so the trace doesn't jump at
                    // release. See PreviewEndpointCache.
                    if (pv.castingSource >= 0 && pv.castingSource < 4) {
                        const float nowSec = std::chrono::duration<float>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        std::lock_guard<std::mutex> lk(previewEndCacheMutex_);
                        auto& cache = previewEndCache_[pv.castingSource];
                        cache = {liveEnd, nowSec, true, pv.cachedDistance, pv.anchorAtCamera};
                        cache.projectileFormID = pv.projectileFormID;
                    }
                }
            }
            constexpr int kMinSamples = 24;
            for (const auto& pv : previews) {
                if (pv.worldPolyline.size() < 2) continue;
                // Sample N points along the polyline (linear by index
                // fraction). Subdivide short polylines so straight-line
                // magic previews get a gradient too. Long arrow arcs
                // get more samples for smoother curves.
                const int N = (std::max)(kMinSamples,
                                          static_cast<int>(pv.worldPolyline.size()));
                const std::size_t polyN = pv.worldPolyline.size();
                ImVec2 prevPv{};
                bool   havePrev = false;
                for (int i = 0; i <= N; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(N);
                    const float idxF = t * static_cast<float>(polyN - 1);
                    const std::size_t i0 = (std::min)(static_cast<std::size_t>(idxF), polyN - 1);
                    const std::size_t i1 = (std::min)(i0 + 1, polyN - 1);
                    const float frac = idxF - static_cast<float>(i0);
                    const auto& a = pv.worldPolyline[i0];
                    const auto& b = pv.worldPolyline[i1];
                    const RE::NiPoint3 p{
                        a.x + (b.x - a.x) * frac,
                        a.y + (b.y - a.y) * frac,
                        a.z + (b.z - a.z) * frac,
                    };
                    double hx = 0.0, hy = 0.0;
                    if (!ProjectWorldPointToHUD(p, hx, hy)) {
                        havePrev = false;
                        continue;
                    }
                    const float frX = (static_cast<float>(hx) - static_cast<float>(base_.x)) + rectMidX;
                    const float frY = (static_cast<float>(hy) - static_cast<float>(base_.y)) + rectMidY;
                    const ImVec2 px{ (frX - frame.left) * scaleX,
                                     (frY - frame.top)  * scaleY };
                    if (havePrev && i > 0) {
                        const float alpha = 0.15f + 0.55f * t;
                        const ImU32 col2 = ImGuiMCP::ImGui::ColorConvertFloat4ToU32(
                            ImVec4(1.0f, 1.0f, 1.0f, alpha));
                        ImGuiMCP::ImGui::ImDrawListManager::AddLine(dl, prevPv, px, col2, trailThickness);
                    }
                    prevPv = px;
                    havePrev = true;
                }
                // Reticle at polyline end (predicted impact).
                const auto& impact = pv.worldPolyline.back();
                double tx = 0.0, ty = 0.0;
                if (ProjectWorldPointToHUD(impact, tx, ty)) {
                    const float frX = (static_cast<float>(tx) - static_cast<float>(base_.x)) + rectMidX;
                    const float frY = (static_cast<float>(ty) - static_cast<float>(base_.y)) + rectMidY;
                    const ImVec2 center{ (frX - frame.left) * scaleX,
                                         (frY - frame.top)  * scaleY };
                    // Live preview: ring contracts as charge fills.
                    // At 0% charge ring=14 (loose); at full charge
                    // ring=6 — exactly where the fired-shot animation
                    // picks up, so the handoff from live-preview to
                    // fired-trail is seamless (no visual jump on release).
                    const float chargeT = std::clamp(pv.chargeProgress, 0.0f, 1.0f);
                    const float ringR   = 14.0f + (6.0f - 14.0f) * chargeT;
                    drawReticle(center, reticleSizeScale, reticleThickness,
                                0.85f, ringR);
                }
            }
        }

    }
}
