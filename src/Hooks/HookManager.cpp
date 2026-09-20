#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include <chrono>
#include "Camera/VanityCamera.h"
#include <array>
#include <unordered_set>
#include <Windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#include "UI/SKSEMenuFramework.h"
#include "UI/CrosshairManager.h"
#include "Camera/AnimationCameraController.h"
#include "Camera/BleedoutFreeLook.h"
#include "Camera/CameraController.h"
#include "Camera/CameraCollision.h"
#include "Camera/FleeFraming.h"
#include "Camera/TargetLockPreview.h"
#include "Camera/MountedTargetLock.h"
#include "Camera/CameraNoiseController.h"
#include "Camera/EventBeatSources.h"
#include "Camera/StateResolver.h"
#include "Core/Spring.h"
#include "Audio/AudioListenerProbe.h"
#include "Dialogue/DialogueLookPicker.h"
#include "Input/DialogueInputSink.h"
#include "Input/POVSlideRecovery.h"
#include "Hooks/HookManager.h"
#include "Hooks/TweenCameraTrace.h"
#include "Hooks/ParaglideTrace.h"
#include "Hooks/MissileProjectileDetour.h"
#include "LockOn/EnemyDetector.h"
#include "LockOn/TDMIntegration.h"
#include "Locations/LocationDetector.h"
#include "Menus/ShowPlayerInMenusController.h"
#include "Settings/SettingsManager.h"
#include "Settings/Defaults.h"
#include "UI/MenuUI.h"
#include "Unpause/UnpauseManager.h"

#include <RE/C/CrosshairPickData.h>
#include <RE/A/AttackBlockHandler.h>
#include <RE/H/HorseCameraState.h>
#include <RE/M/MagicCaster.h>
#include <RE/M/MiddleHighProcessData.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/SpellItem.h>
#include <RE/S/SprintHandler.h>
#include <RE/T/TESHavokUtilities.h>
#include <RE/T/TESObjectDOOR.h>
#include <RE/T/TogglePOVHandler.h>

namespace DietDrCamera
{
    static void SyncDialogueLookLock();

    static MountedTargetLock s_mountedLockPitch;
    static std::uint64_t s_mountedLockFrame = 0;
    static float s_mountedLockDt = 0.0f;
    static bool s_mountedLockSteady = false;
    static bool s_mountedLockOutputAllowed = false;
    static RE::ThirdPersonState* s_mountedRotationState = nullptr;
    static bool s_mountedLockOwnsYaw = false;
    static float s_mountedLockWorldYaw = 0.0f;

    static float TrackingYawReference(RE::ThirdPersonState* state, RE::PlayerCharacter* player)
    {
        if (state->id == RE::CameraState::kMount) {
            if (auto mount = static_cast<RE::HorseCameraState*>(state)->horseRefHandle.get()) {
                if (auto* actor = mount->As<RE::Actor>()) return actor->GetHeading(false);
            }
        }
        return player->data.angle.z;
    }

    // Read device state before the menu/handler event filters. In particular,
    // a swallowed Up event must not masquerade as a physically held POV key.
    //
    // 2026-09-10: this read answered Unknown at BOTH [POVSLIDE] edges of the
    // 19:21:54 latch, so the recovery below could never have fired on this
    // machine. Unknown now means only "a device the game believes could be
    // holding the key cannot be read". A device that cannot deliver the key
    // at all (no keyboard object, an uninitialised mouse, an out-of-range
    // mapping) cannot be the one holding it and no longer poisons the
    // verdict. The `pollingEnabled` early-out is gone: the XInput read is a
    // direct OS query that does not depend on the engine's polling flag, and
    // a guess about that flag is exactly the kind of thing that left the read
    // blind. `a_why` (optional) receives what was actually read, for the log.
    static POVButtonState ReadPhysicalPOVButton(std::string* a_why = nullptr)
    {
        auto* devices = RE::BSInputDeviceManager::GetSingleton();
        auto* map = RE::ControlMap::GetSingleton();
        auto* events = RE::UserEvents::GetSingleton();
        if (!devices || !map || !events) {
            if (a_why) *a_why = "no input manager";
            return POVButtonState::Unknown;
        }
        std::string why;
        bool unknown = false;
        const auto keyFor = [&](RE::INPUT_DEVICE device) {
            return map->GetMappedKey(events->togglePOV, device,
                                    RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
        };
        const auto keyboardKey = keyFor(RE::INPUT_DEVICE::kKeyboard);
        if (keyboardKey != RE::ControlMap::kInvalid) {
            auto* keyboard = devices->GetKeyboard();
            if (!keyboard || keyboardKey >= 256) {
                why += fmt::format("kb[{:#x}]=unreadable ", keyboardKey);
            } else {
                const bool held = (keyboard->curState[keyboardKey] & 0x80) != 0;
                why += fmt::format("kb[{:#x}]={} ", keyboardKey, held ? "HELD" : "up");
                if (held) { if (a_why) *a_why = why; return POVButtonState::Held; }
            }
        }
        const auto mouseKey = keyFor(RE::INPUT_DEVICE::kMouse);
        if (mouseKey != RE::ControlMap::kInvalid) {
            auto* mouse = devices->GetMouse();
            if (!mouse || mouse->notInitialized || mouseKey >= 8) {
                why += fmt::format("mouse[{}]=unreadable ", mouseKey);
            } else {
                const bool held = (mouse->dInputNextState.rgbButtons[mouseKey] & 0x80) != 0;
                why += fmt::format("mouse[{}]={} ", mouseKey, held ? "HELD" : "up");
                if (held) { if (a_why) *a_why = why; return POVButtonState::Held; }
            }
        }
        const auto padKey = keyFor(RE::INPUT_DEVICE::kGamepad);
        if (padKey != RE::ControlMap::kInvalid) {
            bool foundPad = false;
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                XINPUT_STATE state{};
                if (XInputGetState(i, &state) != ERROR_SUCCESS) continue;
                foundPad = true;
                using Keys = RE::BSWin32GamepadDevice::Keys;
                const bool held = padKey == Keys::kLeftTrigger
                    ? state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                    : padKey == Keys::kRightTrigger
                        ? state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                        : (state.Gamepad.wButtons & padKey) != 0;
                why += fmt::format("pad{}[{:#x}]={} ", i, padKey, held ? "HELD" : "up");
                if (held) { if (a_why) *a_why = why; return POVButtonState::Held; }
            }
            // A non-XInput pad cannot be confirmed released by this poll.
            if (!foundPad) {
                if (devices->IsGamepadConnected()) { unknown = true; why += "pad=connected-but-not-XInput "; }
                else                               why += "pad=none ";
            }
        }
        if (a_why) *a_why = why;
        return unknown ? POVButtonState::Unknown : POVButtonState::Released;
    }

    static void LogFreezeMovementState(std::string_view a_reason, bool a_reportWarning = false)
    {
        const auto level = a_reportWarning ? spdlog::level::warn : spdlog::level::debug;
        if (!spdlog::should_log(level)) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* controls = RE::PlayerControls::GetSingleton();
        auto* main = RE::Main::GetSingleton();
        spdlog::log(level, "[FREEZE-STATE] {} freezeTime={} gameActive={} smfBlock={} "
                     "qtOpen={} blockPlayerInput={}",
                     a_reason, main ? static_cast<int>(main->freezeTime) : -1,
                     main ? static_cast<int>(main->gameActive) : -1,
                     MenuUI::IsGameInputBlocked(), MenuUI::IsQuickTuneWindowOpen(),
                     controls ? static_cast<int>(controls->blockPlayerInput) : -1);
        if (controls) {
            auto* attack = controls->attackBlockHandler;
            auto* sprint = controls->sprintHandler;
            auto* pov    = controls->togglePOVHandler;
            // povRegistered/povHeld are the engine's own Toggle POV latches:
            // a registered press with no matching release at a menu edge is
            // the seed of the fovSlideMode latch the [POVSLIDE] lines track.
            spdlog::log(level, "[FREEZE-STATE] handlers moveEnabled={} attackEnabled={} "
                         "attackHeld={} left={} right={} releasePending={} ignore={} "
                         "sprintHeld={} fovSlide={} povRegistered={} povHeld={} povRelease={} remap={}",
                         controls->movementHandler
                             ? static_cast<int>(controls->movementHandler->IsInputEventHandlingEnabled()) : -1,
                         attack ? static_cast<int>(attack->IsInputEventHandlingEnabled()) : -1,
                         attack ? static_cast<int>(attack->heldStateActive) : -1,
                         attack ? static_cast<int>(attack->GetRuntimeData().heldLeft) : -1,
                         attack ? static_cast<int>(attack->GetRuntimeData().heldRight) : -1,
                         attack ? static_cast<int>(attack->triggerReleaseEvent) : -1,
                         attack ? static_cast<int>(attack->GetRuntimeData().ignore) : -1,
                         sprint ? static_cast<int>(sprint->heldStateActive) : -1,
                         controls->data.fovSlideMode,
                         pov ? static_cast<int>(pov->pressRegistered) : -1,
                         pov ? static_cast<int>(pov->heldStateActive) : -1,
                         pov ? static_cast<int>(pov->triggerReleaseEvent) : -1,
                         controls->data.remapMode);
        }
        WORD buttons = 0;
        BYTE leftTrigger = 0, rightTrigger = 0;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state{};
            if (XInputGetState(slot, &state) != ERROR_SUCCESS) continue;
            buttons |= state.Gamepad.wButtons;
            leftTrigger = (std::max)(leftTrigger, state.Gamepad.bLeftTrigger);
            rightTrigger = (std::max)(rightTrigger, state.Gamepad.bRightTrigger);
        }
        spdlog::log(level, "[FREEZE-STATE] raw mouseLeft={} mouseRight={} padButtons=0x{:X} "
                     "leftTrigger={} rightTrigger={}",
                     (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
                     (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
                     buttons, leftTrigger, rightTrigger);
        if (player) {
            std::string graph;
            for (const auto* name : { "IsNPC", "bAnimationDriven", "bMotionDriven",
                                     "bAnimationDrivenDialogue", "bMotionDrivenDialogue",
                                     "TDM_LockRotation", "bIsSynced" }) {
                bool value = false;
                const bool valid = player->GetGraphVariableBool(name, value);
                graph += fmt::format(" {}={}", name, valid ? (value ? "1" : "0") : "?");
            }
            std::int32_t state = -1;
            const bool stateValid = player->GetGraphVariableInt("iState", state);
            spdlog::log(level, "[FREEZE-STATE] actor animationDriven={} aiPackage={} "
                         "race={:08X} iState={} valid={} graph:{}",
                         player->IsAnimationDriven(), static_cast<bool>(player->GetPlayerRuntimeData().playerFlags.aiControlledPackage),
                         player->GetRace() ? player->GetRace()->GetFormID() : 0,
                         state, stateValid, graph);
        }
        TDMIntegration::GetSingleton().LogMovementState(a_reportWarning);
    }

    // Driven by Plugin.cpp's MenuCloseSink via OnDialogueMenuOpenChange.
    // When the Dialogue Menu is open, the lastSpeaker fallback in the
    // face-lock and input gates is valid (catches transient null-speaker
    // windows during a conversation). When the menu closes (user pressed
    // Goodbye/TAB/walked away), the fallback is suppressed so face-lock
    // releases the instant the player exits, regardless of whether the
    // NPC's voice line is still playing in the background.
    static inline bool s_dialogueMenuOpen = false;

    // Prime flag set on every dialogue OPEN edge, consumed by the next
    // option-select poll tick. Without this, the OPEN-frame poll sees
    // sLastSelDlg=nullptr â†’ curDlg=<initial dlg> as a dlgEdge and fires
    // a random re-pick that competes with the OnDialogueOpen() bucket
    // pick already done from OnDialogueMenuOpenChange. Result: two
    // different look targets within one millisecond, and channels lurch
    // toward A then toward B for the entire entry blend. Prime swallows
    // the first edge so only true mid-dialogue option changes re-pick.
    static inline bool s_dialoguePollNeedsPrime = false;

    // Auto-POV switch on dialogue entry. On open, if the user enabled
    // force-3p or force-1p AND the player isn't already in that POV,
    // flip the camera and remember the previous POV. On close, restore.
    // If the player was already in the chosen POV, no flip + no restore.

    // Forward declaration so HookedFirstPersonUpdate can pin the engine's
    // pitch-offset spring during face-lock. Definition lives later in the
    // file alongside the rest of the face-lock state.
    static inline bool s_dialogueFaceLockWasActive = false;

    // Set true when force-1p flips POV at dialogue open. Consumed by the
    // 1p face-lock code on the very next frame to snap the spring's
    // position to the NPC face (instead of running the 200ms entry lerp
    // from cameraNI). Without this, the freshly-activated 1p camera
    // starts at the player's head-bone orientation (typically off-axis
    // toward where they were facing pre-dialogue) and visibly slides to
    // the NPC â€” the user reported "off center, then transitions to face."
    static inline bool s_dialogueSnapFaceLockNextFrame = false;

    // Face-lock matrix shared between HookedUpdateCameraPost (writer) and
    // HookedNiCameraUpdateWorldData (renderer-side post-original override).
    // Updated each frame face-lock writes its matrix; consumed in the
    // NiCamera scene-graph recompose hook to win the final write.
    static inline RE::NiMatrix3 s_faceLockRenderMatrix{};
    static inline bool          s_faceLockShouldRender = false;

    // Release-tail deadline for the 1p dialogue noise. While the face-lock is
    // active the noise rides its render matrix; the instant it releases the
    // engine retakes the camera but the NiCamera render hook keeps firing during
    // the settle, and its recompute wipes the direct-path noise write â€” so noise
    // drops out then snaps back. The render hook re-applies the noise on top while
    // this deadline is in the future AND slides the deadline forward on every fire,
    // so the tail lasts exactly as long as the post-release recompute keeps firing
    // (the settle), then expires once the camera goes quiet and the direct path
    // survives on its own again.
    static inline std::chrono::steady_clock::time_point s_faceLockNoiseTailUntil{};

    // Driven by Plugin.cpp's MenuCloseSink via OnMapMenuOpenChange. While
    // the World/Local Map is open the 1p/3p FOV pipelines are dormant, so
    // the guard below (and the per-frame enforcement in
    // HookedUpdateCameraPost) keeps the map rendering at the game's default
    // world FOV instead of whatever the last pipeline latched â€” e.g. a
    // zoomed dialogue FOV when Better Carriage Destinations opens the map
    // mid-conversation.
    static inline bool s_mapMenuOpen = false;

    // The engine's configured default world FOV (SkyrimPrefs override
    // first, then Skyrim.ini, then the hardcoded 80). Used as the map's
    // neutral FOV â€” respects a user's custom INI FOV.
    static float DefaultWorldFov()
    {
        if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
            if (auto* st = prefs->GetSetting("fDefaultWorldFOV:Display"); st && st->data.f > 1.0f) {
                return st->data.f;
            }
        }
        if (auto* ini = RE::INISettingCollection::GetSingleton()) {
            if (auto* st = ini->GetSetting("fDefaultWorldFOV:Display"); st && st->data.f > 1.0f) {
                return st->data.f;
            }
        }
        return 80.0f;
    }

    // ----- Vanilla pitch zoom-out suppression -----------------------------
    // The engine pushes the third-person camera BACK as the player pitches
    // down toward the ground. Two halves, and both have to be answered:
    //
    //   * the SOURCE â€” `fPitchZoomOutMaxDist:Camera` (Skyrim.ini), the
    //     maximum distance the engine is allowed to add. Zeroing it is what
    //     actually stops the behaviour, because the engine recomputes the
    //     offset from the current pitch inside ThirdPersonState::Update and
    //     consumes it in the same call â€” anything we write afterwards is
    //     already too late for the frame that rendered.
    //   * the FIELD â€” `ThirdPersonState::pitchZoomOffset`, where the result
    //     lands. Cleared as well so a value latched before the toggle (or by
    //     another plugin) doesn't sit there for a frame.
    //
    // The vanilla value is still captured and logged once â€” Skyrim.ini
    // normally omits the key, so the engine default is the only thing that
    // decides how strong the effect was, and the diagnostic suspend path
    // hands it back so "pure vanilla" stays pure.
    //
    // Suppression is UNCONDITIONAL: it shipped behind a toggle for one build
    // (2026-08-22) and became permanent the same day once the user confirmed
    // the fix. It is a correction, not a preference â€” the preset decides the
    // camera distance, and this was the engine quietly overriding it.
    static void ApplyPitchZoomOut(RE::ThirdPersonState* a_tps, bool a_allow)
    {
        static float sVanillaMaxDist = 0.0f;
        static bool  sCaptured       = false;

        RE::Setting* st = nullptr;
        if (auto* ini = RE::INISettingCollection::GetSingleton())
            st = ini->GetSetting("fPitchZoomOutMaxDist:Camera");
        if (!st)
            if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton())
                st = prefs->GetSetting("fPitchZoomOutMaxDist:Camera");

        if (st) {
            if (!sCaptured) {
                sVanillaMaxDist = st->data.f;
                sCaptured       = true;
                spdlog::info("HookManager: fPitchZoomOutMaxDist vanilla value = {:.2f}", sVanillaMaxDist);
            }
            const float desired = a_allow ? sVanillaMaxDist : 0.0f;
            if (st->data.f != desired) st->data.f = desired;
        } else if (!sCaptured) {
            sCaptured = true;
            spdlog::warn("HookManager: fPitchZoomOutMaxDist:Camera not found â€” "
                         "pitch zoom-out suppression falls back to clearing pitchZoomOffset only");
        }

        if (!a_allow && a_tps && a_tps->pitchZoomOffset != 0.0f)
            a_tps->pitchZoomOffset = 0.0f;
    }

    void HookManager::OnMapMenuOpenChange(bool a_open)
    {
        s_mapMenuOpen = a_open;
        if (a_open) {
            // The map usually pauses the game, so per-frame hooks may never
            // run while it's up â€” restore the neutral FOV right here on the
            // open edge. On close nothing to do: the 1p/3p pipelines resume
            // and rewrite their spring values within a frame.
            if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                const float def = DefaultWorldFov();
                spdlog::debug("[MAP] menu OPEN â€” worldFOV {:.1f} -> default {:.1f}",
                             cam->worldFOV, def);
                cam->worldFOV = def;
            }
        } else {
            spdlog::debug("[MAP] menu CLOSE");
        }
    }

    void HookManager::OnDialogueMenuOpenChange(bool a_open)
    {
        s_dialogueMenuOpen = a_open;
        spdlog::debug("[DLG] menu {}", a_open ? "OPEN" : "CLOSE");
        if (a_open) {
            s_dialoguePollNeedsPrime = true;
            // Snap 1p face-lock to the NPC face on the first frame of
            // every dialogue, regardless of whether we forced POV. The
            // 200ms entry lerp from cameraNI was reading as off-axis
            // ("starts looking lower-left, then transitions to face").
            // 1p face-lock code consumes + clears this flag; non-1p
            // dialogues see the flag set but it's only acted on by the
            // 1p face-lock branch (and is cleared on close).
            s_dialogueSnapFaceLockNextFrame = true;
            DialogueLookPicker::OnDialogueOpen();
            // Dialogue uses whatever POV the player is in; the camera is not
            // forced on entry. (The force-1p / force-3p-on-entry toggles were
            // removed â€” forcing 1p->3p triggered an unfixable entry snap.)
        } else {
            DialogueLookPicker::OnDialogueClose();
            // A Ready Weapon press made during the conversation lands here,
            // not while the menu was up â€” see the deferral note in
            // DialogueInputSink.
            DialogueInputSink::ApplyPendingSheathe();
            s_dialogueSnapFaceLockNextFrame = false;
        }
    }

    // Dialogue face-lock blend state. Every reference mod that aims the
    // camera at an NPC face (ACC, SmoothCam FaceToFaceDialogue, TDM target
    // lock) interpolates per frame and never snaps. Snapping on the first
    // post-dialogue frame was causing the floor-flash on moving entries,
    // because (a) cameraRoot->world.translate is a frame behind during a
    // run-to-idle blend, (b) player.data.angle.x carries a transient lean
    // from the locomotion anim, and (c) the NPC head bone isn't settled.
    // A time-based lerp from the player's current rotation to the aim
    // target absorbs all three: frame-1 scalarâ‰ˆ0 â†’ write â‰ˆ current rotation
    // (no-op), frame-N â†’ full face-lock. No more flash.
    static inline bool  s_dialogueWasOpenLastFrame = false;
    static inline bool  s_dialogueAimInit          = false;
    // TDM yaw-control claim for keeping the BODY facing a non-Actor dialogue
    // speaker (Statue of Mara etc.). Held while facing, released on sprint /
    // speaker-becomes-Actor (in the 3p face-lock block) and on dialogue end
    // (TickDialogueAimInit, which runs even after the speaker clears).
    static inline bool  s_dlgFaceYawTaken          = false;
    static inline float s_dialogueAimStartYaw      = 0.0f;
    static inline float s_dialogueAimStartPitch    = 0.0f;
    static inline float s_dialogueAimStartFreeY    = 0.0f;  // pre-dialogue freeRotation.y, blended to 0
    // double, not float: these are absolute steady_clock-since-epoch
    // seconds. On any system with non-trivial uptime that value is
    // 10^5â€“10^6 and float's 24-bit mantissa quantizes it to ~0.03â€“0.06s,
    // which catastrophically cancels when computing (now - startTime)
    // for the cubic-blend t. At 30 FPS the cancellation rounds every
    // other frame's delta to zero, producing the (step, hold, step,
    // hold) staircase in freeRotation.x AND freeRotation.y observed in
    // the dialogue-entry [DLG-PERFRAME] log. Channel blend doesn't have
    // this problem because it uses a dt accumulator (small values, no
    // cancellation). Keep these as double end-to-end through the
    // subtraction; cast to float only after dividing by blend duration
    // (result is in [0,1] where float is fine).
    static inline double s_dialogueAimStartTime     = 0.0;  // seconds (steady_clock epoch)
    static inline double s_dialogueLastFrameTime    = 0.0;  // for dt computation

    // Live NPC tracking via exponentially-smoothed target. The live
    // desiredYaw/Pitch (from head-bone sampling) feeds a smoothed target
    // that converges with time constant kDialogueAimTau â€” fast enough to
    // follow player movement (which can spin desired yaw at 4-6 rad/s
    // while walking around the NPC), slow enough to absorb head-bone
    // jitter. Replaces an earlier 1 rad/s hard rate-limit that lagged
    // visibly whenever the player moved.
    static inline float s_dialogueSmoothedTargetYaw   = 0.0f;
    static inline float s_dialogueSmoothedTargetPitch = 0.0f;
    // Spring velocities for the two smoothed targets above. Carrying velocity
    // is what lets the aim be re-targeted mid-flight (a preset switch, the
    // reverse shot flipping) without a kink â€” the new approach starts from the
    // speed the old one was already travelling at.
    static inline float s_dialogueAimYawVel   = 0.0f;
    static inline float s_dialogueAimPitchVel = 0.0f;
    // Proximity-warped aim clock: accumulates dt Ã— dlgPaceMul instead of
    // wall-clock, so the aim blend and the position blend (whose elapsed is
    // paced the same way in CameraController) slow down TOGETHER near a face.
    // A wall-clock aim over a paced position would desync â€” the exact
    // "fast then crawl" failure the shared DialogueBlendDuration exists to
    // prevent.
    static inline float s_dialogueAimWarpT = 0.0f;
    static constexpr float kDialogueAimTau = 0.18f;  // seconds (~80% catchup in 0.3s)

    // Tracks whether the 1p face-lock has set the player's IsNPC anim var
    // to true (which suppresses the engine's dialogue head-track animation).
    // We must only reset it back to false if WE set it; writing to the
    // anim var otherwise triggers graph re-evaluation that can break 3p.
    static inline bool s_isNpcOverriddenByUs = false;
    // Pre-override snapshots, captured on the frame the face-lock first takes
    // the graph over and put back by RestoreDialogueNpcOverride. Captured
    // rather than assumed: head tracking is normally 1 on the player, but
    // "normally" is exactly the kind of assumption that leaves a flag wrong
    // for the rest of a session when it isn't.
    static inline std::uint32_t s_savedHeadTracking          = 1;
    static inline std::int32_t  s_savedSyncDialogueResponse  = 0;

    // Dialogue state is "active" the moment the engine registers the player's
    // activation, even while the previous goodbye voice line is still playing
    // and the DialogueMenu hasn't been constructed. `ui->IsMenuOpen` is false
    // during that window â€” TDM, ACC, and SmoothCam all skip IsMenuOpen and
    // read MenuTopicManager directly for exactly this reason.
    //
    // Gate the lastSpeaker fallback on `s_dialogueMenuOpen && !isSayingGoodbye`
    // (matches the 1p face-lock gate). Without this, the 3p face-lock keeps
    // tracking the NPC's face after the player exits dialogue while the
    // goodbye voice line is still playing â€” `speaker` clears but `lastSpeaker`
    // lingers for the duration of the line.
    // Returns the active dialogue speaker as a TESObjectREFR (NOT Actor): the
    // Statue of Mara and other shrines are talking ACTIVATORS, not Actors, so
    // As<Actor>() nulled them and the dialogue face-lock never engaged. The 1p
    // face-lock was already generalized this way; the 3p path goes through here,
    // so returning the ref keeps both POVs working for non-Actor speakers.
    static RE::TESObjectREFR* GetActiveDialogueSpeaker()
    {
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) return nullptr;
        RE::TESObjectREFRPtr refPtr;
        if (auto p = mtm->speaker.get(); p) {
            refPtr = p;
        } else if (s_dialogueMenuOpen && !mtm->forceGoodbye) {
            if (auto p2 = mtm->lastSpeaker.get(); p2) {
                refPtr = p2;
            }
        }
        if (!refPtr) return nullptr;
        return refPtr.get();
    }

    static double DialogueNowSeconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // Called at the top of each camera-state hook. Resets the blend state
    // on the dialogue-open edge so the next face-lock call captures a
    // fresh starting rotation, and clears it on close so re-entry works.
    // Gate must match the face-lock gate (GetActiveDialogueSpeaker) â€” if
    // this used IsMenuOpen while the face-lock used MTM, init would reset
    // every frame during the mid-goodbye pre-render window and the blend
    // would never advance (pitch frozen at whatever the player was looking
    // at when they hit Activate, then a weird pitch snap when the menu
    // finally renders).
    //
    // Debounced: if the speaker briefly goes null for a frame or two (the
    // engine clears mtm->speaker between topic transitions and during some
    // movement-anim stops), keep the "active" state until ~150ms have
    // passed. Without this, stopping after walking at full speed produced
    // a single-frame null window that was triggering a full transition
    // reinit â€” visible as the camera "swinging" back to the face.
    static void TickDialogueAimInit()
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point sLastSeenSpeaker{};

        const auto now = clock::now();
        const bool rawActive = GetActiveDialogueSpeaker() != nullptr;
        if (rawActive) {
            sLastSeenSpeaker = now;
        }
        const float sinceLastSeen =
            std::chrono::duration<float>(now - sLastSeenSpeaker).count();
        const bool dialogueActive = rawActive || sinceLastSeen < 0.15f;

        if (dialogueActive && !s_dialogueWasOpenLastFrame) {
            s_dialogueAimInit = false;
        }
        if (!dialogueActive) {
            s_dialogueAimInit = false;
            // Dialogue ended (speaker cleared) â€” the 3p face-lock block no
            // longer runs, so release the body-face yaw claim here or it leaks
            // and TDM keeps holding the player's yaw after the conversation.
            if (s_dlgFaceYawTaken) {
                TDMIntegration::GetSingleton().ReleaseYawControl();
                s_dlgFaceYawTaken = false;
            }
        }
        s_dialogueWasOpenLastFrame = dialogueActive;
    }

    // Single-sourced restore of the anim-graph overrides the 1p dialogue face-
    // lock installs (IsNPC=true + the dialogue-idle pins, written per-frame in
    // the 1p hook to suppress the engine's greeting->talking pose snap). Gated
    // on s_isNpcOverriddenByUs so it's a no-op when nothing was overridden.
    // Called both at dialogue close AND â€” to fix the POV-switch exit snap â€” when
    // the user switches 1p->3p mid-dialogue, so the whole graph reset doesn't
    // fire in one frame on the 3p exit (which reads as a pose snap).
    static void RestoreDialogueNpcOverride()
    {
        if (!s_isNpcOverriddenByUs) return;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            player->SetGraphVariableBool("IsNPC", false);
            player->SetGraphVariableBool("bIdleBeforeConversation", true);
            player->SetGraphVariableBool("bAnimationDrivenDialogue", true);
            player->SetGraphVariableBool("bMotionDrivenDialogue",    true);
            player->SetGraphVariableBool("bIsDialogueExpressive",    true);
            player->SetGraphVariableBool("bHHDialogue",              true);
            player->SetGraphVariableBool("bHDDialogue",              true);
            // The face-lock also pins the response-sync int and ZEROES the
            // actor's head-tracking flag every frame. Those two were the ONLY
            // things it changed that this restore didn't put back â€” so after a
            // first-person conversation the player kept walking around with
            // head tracking disabled and the dialogue response sync pinned,
            // for the rest of the session. That is the "dialogue settings
            // leaked after exiting dialogue" report: it only ever happened
            // after a 1p conversation, which is why it looked intermittent.
            player->SetGraphVariableInt("iSyncDialogueResponse", s_savedSyncDialogueResponse);
            if (auto* aState = player->AsActorState()) {
                aState->actorState2.headTracking = s_savedHeadTracking;
            }
            player->NotifyAnimationGraph("IdleDialogueUnlock");
        }
        s_isNpcOverriddenByUs = false;
    }

    namespace
    {
        // Own DirectInput mouse device for free-look during bleedout. Uses
        // non-exclusive / background cooperative level so it coexists with
        // the engine's own DirectInput mouse acquisition without stealing
        // input. Unlike raw input, DirectInput does NOT register a window-
        // scoped input sink â€” so this doesn't collide with SKSE Menu
        // Framework's input layer. GetDeviceState returns relative deltas
        // independent of OS cursor clipping.
        class DirectInputMouseCapture
        {
        public:
            static DirectInputMouseCapture& GetSingleton()
            {
                static DirectInputMouseCapture instance;
                return instance;
            }

            // Returns (dx, dy). Zero if not yet initialised, device not
            // acquired, or no movement. Auto-reacquires on DIERR_INPUTLOST.
            void Poll(long& dx, long& dy)
            {
                dx = 0;
                dy = 0;
                if (!EnsureInitialized()) return;
                DIMOUSESTATE2 state{};
                HRESULT hr = device->GetDeviceState(sizeof(state), &state);
                if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                    device->Acquire();
                    hr = device->GetDeviceState(sizeof(state), &state);
                }
                if (FAILED(hr)) return;
                dx = state.lX;
                dy = state.lY;
            }

        private:
            LPDIRECTINPUT8       di8       = nullptr;
            LPDIRECTINPUTDEVICE8 device    = nullptr;
            bool                 attempted = false;
            bool                 ready     = false;

            static HWND FindTopLevelGameWindow()
            {
                struct Ctx { DWORD pid; HWND out; } ctx{ GetCurrentProcessId(), nullptr };
                EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD wpid = 0;
                    GetWindowThreadProcessId(h, &wpid);
                    if (wpid == c->pid && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
                        c->out = h;
                        return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&ctx));
                return ctx.out;
            }

            bool EnsureInitialized()
            {
                if (attempted) return ready;
                attempted = true;
                HWND hwnd = FindTopLevelGameWindow();
                if (!hwnd) {
                    spdlog::warn("DirectInputMouseCapture: no game window found");
                    return false;
                }
                HMODULE hInst = GetModuleHandleW(nullptr);
                HRESULT hr = DirectInput8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8, reinterpret_cast<LPVOID*>(&di8), nullptr);
                if (FAILED(hr) || !di8) {
                    spdlog::warn("DirectInputMouseCapture: DirectInput8Create failed 0x{:x}", static_cast<std::uint32_t>(hr));
                    return false;
                }
                hr = di8->CreateDevice(GUID_SysMouse, &device, nullptr);
                if (FAILED(hr) || !device) {
                    spdlog::warn("DirectInputMouseCapture: CreateDevice failed 0x{:x}", static_cast<std::uint32_t>(hr));
                    return false;
                }
                hr = device->SetDataFormat(&c_dfDIMouse2);
                if (FAILED(hr)) {
                    spdlog::warn("DirectInputMouseCapture: SetDataFormat failed 0x{:x}", static_cast<std::uint32_t>(hr));
                    return false;
                }
                hr = device->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
                if (FAILED(hr)) {
                    spdlog::warn("DirectInputMouseCapture: SetCooperativeLevel failed 0x{:x}", static_cast<std::uint32_t>(hr));
                    return false;
                }
                device->Acquire();  // may initially fail; Poll() will retry on DIERR_NOTACQUIRED
                ready = true;
                spdlog::info("DirectInputMouseCapture: initialized on HWND {}", reinterpret_cast<void*>(hwnd));
                return true;
            }
        };

        // Flag: set true on bleedout entry with free look enabled so that
        // UpdateCameraCasterPatch forces the engine's camera-collision
        // raycast off for the duration of free-look bleedout. Cleared in
        // HookedThirdPersonUpdate once the player is alive again. Lives at
        // namespace scope so every hook site can see it.
        bool s_forceCasterPatchForDeathFreeLook = false;
        BleedoutFreeLook s_bleedoutFreeLook;

        // Death-cam slow-motion envelope. Armed on BleedoutCameraState::Begin.
        // The envelope advances on a WALL CLOCK (steady_clock) so the slowdown
        // can't stretch its own duration, but the accumulator is PAUSED while
        // any menu/overlay is open or the game is paused â€” so "Duration = 5"
        // means 5 real seconds of actual running game, not wall time that also
        // ticks through menus. Restored to normal on completion (and as a
        // safety on End). Lives at namespace scope so the Begin trigger and the
        // Update driver share it.
        bool                                  s_deathSlowmoActive  = false;
        std::chrono::steady_clock::time_point s_deathSlowmoLast{};      // prev driver-frame timestamp
        float                                 s_deathSlowmoElapsed = 0.0f;  // accumulated RUNNING seconds

        // True only when the bleedout camera was entered by an ACTUAL player death
        // (not a recoverable ragdoll/knockdown â€” Unrelenting Force, paralysis, a
        // giant's club, etc.). The engine routes BOTH through BleedoutCameraState,
        // so DDC's death-cam effects + reload-time priming must run only for a real
        // death; otherwise a mere ragdoll triggers the death cam and the load-save
        // prompt. Set at Begin, read by the Update driver.
        bool s_deathCamRealDeath = false;

        // Slow-mo envelope parameters captured at arm time so the shared driver in
        // HookedBleedoutUpdate doesn't need to know whether it's a death (death-cam
        // sliders) or a ragdoll (ragdoll sliders).
        float s_slowmoStrengthActive = 0.0f;   // 0..90 (%)
        float s_slowmoDurationActive = 4.0f;   // seconds
        // Set by the ragdoll fade hotkey (input sink); the driver then eases the
        // slow-mo back to normal early instead of waiting out the duration.
        bool  s_slowmoFadeReq = false;

        // The player is in bleedout because they actually died, vs a recoverable
        // knockdown. Dead OR health fully spent reads as death; alive-with-health
        // is a ragdoll the player will get back up from.
        bool BleedoutIsRealDeath()
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) return true;            // unknown -> behave as before (death)
            if (pc->IsDead()) return true;
            if (auto* avo = pc->AsActorValueOwner())
                return avo->GetActorValue(RE::ActorValue::kHealth) <= 0.0f;
            return true;
        }

        // Restore the global time multiplier to 1.0 and disarm the death-cam
        // slow-mo. Safe to call when inactive.
        void RestoreDeathSlowmo()
        {
            if (s_deathSlowmoActive) {
                if (auto* timer = RE::BSTimer::GetSingleton()) {
                    timer->SetGlobalTimeMultiplier(1.0f, true);
                }
                s_deathSlowmoActive = false;
            }
        }


    }

    HookManager& HookManager::GetSingleton()
    {
        static HookManager instance;
        return instance;
    }

    // ===== [ANIMCAM-HOOK] the animated-camera event handler ================
    //
    // FINAL LINK IN THE CHAIN, and the measurement that forced it: with the
    // Cam3 scene node HELD AT REST for two confirmed charge windows, the
    // rendered tilt still hit the full +0.2675 rad. The animated-camera mode
    // does not read the scene-graph bone — it reads the camera track from
    // inside the behavior graph. No output-side or scene-side write can ever
    // beat that. The only clean point is the DISPATCH: if the engine's
    // AnimatedCameraDeltaStartHandler never executes for the player's shield
    // charge, the mode never opens and there is nothing downstream to fight.
    //
    // The handler classes are not in CommonLib, but their MSVC RTTI is in the
    // binary, so the vtable is found BY NAME at runtime — no Address Library
    // IDs, no version-specific offsets. The vfunc index is library fact, not
    // a guess: RE::IHandlerFunctor declares 0 = dtor, 1 = ExecuteHandler
    // (bool, Handler&, const Parameter&).
    namespace
    {
        using AnimHandlerExecute_t = bool(*)(void*, RE::Actor&, const RE::BSFixedString&);
        AnimHandlerExecute_t sOrigAnimCamDeltaExecute = nullptr;
        AnimHandlerExecute_t sOrigAnimCamStartExecute = nullptr;
        AnimHandlerExecute_t sOrigAnimCamEndExecute   = nullptr;
        // The REAL End-handler singleton, captured from the engine's own
        // dispatch (hooked as a passthrough). Every bob and charge ends with
        // a genuine EndAnimatedCamera, so this fills within seconds of the
        // first sprint. The Start singletons are same-layout flyweights and
        // serve as the pre-capture fallback `this`.
        std::atomic<void*> sAnimCamEndThis{ nullptr };
        std::atomic<void*> sAnimCamAnyThis{ nullptr };

        // Set every 3p frame the player is actually blocking; read at event
        // time. The charge fires its Start event on the FIRST frame of the
        // sprint, before the state resolver has flipped, and IsBlocking() is
        // already false by then — but the player was provably blocking within
        // the last half second, because holding block and then sprinting IS
        // the charge. A plain sprint with a shield merely equipped has no
        // recent block, so its stride-bob window stays untouched.
        std::atomic<long long> sLastBlockingMs{ 0 };

        // The confirmed shield-charge window, mirrored into atomics so the
        // EVENT THREAD can read it. Written by the [ANIMCAM] tick (camera
        // thread): open when the window confirms as Shield Sprinting, closed
        // (with a timestamp) when it ends. This is suppress leg 3 — the
        // charge-EXIT animation fires its own StartAnimatedCameraDelta at a
        // moment when BOTH other legs are stale: IsBlocking() reads false
        // for the whole charge (so lastBlock is 1-2s old at exit), and the
        // resolver flips off Shield Sprinting within ~100ms of the exit.
        // "A window was confirmed and closed under a second ago" is the
        // signature of that exit event and of nothing else; plain-sprint
        // bobs never confirm, so they keep forwarding.
        std::atomic<bool>      sShieldChargeOpen{ false };
        std::atomic<long long> sShieldChargeCloseMs{ 0 };

        // Was the player's most recent Start FORWARDED (mode live)? The last
        // leak path (10:45:22, 2026-08-29): plain sprint opens a forwarded
        // stride-bob window; block raised MID-SPRINT crossfades the graph
        // into the charge with NO new Start event — nothing to suppress, the
        // charge track plays through the open door. The [ANIMCAM] tick reads
        // this at CONFIRM time and closes the live mode right there.
        std::atomic<bool> sAnimCamLastStartForwarded{ false };

        long long NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool ShouldSuppressAnimatedCamera(RE::Actor& a_actor)
        {
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (&a_actor != ply || !ply) return false;
            // NO IsSprinting() gate. The 16:28 run proved the predicate (or
            // the dispatch) failed SILENTLY: hook installed, zero suppression
            // lines, full tilt rendered. The event fires on the charge's
            // first frame, and the sprint flag's timing relative to it is
            // exactly the kind of assumption that already burned IsBlocking()
            // — so the gate is the recent-block sample alone: the ONLY
            // animated-camera delta the player triggers within half a second
            // of blocking is the shield charge. Plain sprint (shield merely
            // equipped, never raised) has no recent block and keeps its
            // stride-bob window.
            // Leg 3 FIRST (cheapest): the charge's own window is open, or
            // closed under a second ago — catches the charge-exit
            // animation's Start, which neither other leg can see.
            if (sShieldChargeOpen.load(std::memory_order_relaxed)) return true;
            if (NowMs() - sShieldChargeCloseMs.load(std::memory_order_relaxed) < 1000)
                return true;
            const bool blockedRecently =
                (NowMs() - sLastBlockingMs.load(std::memory_order_relaxed)) < 500;
            if (blockedRecently) return true;
            // Fallback for a charge entered with the resolver already settled
            // (block held long enough that the entry resolved before the event).
            auto& setH = SettingsManager::GetSingleton();
            const CameraProfile* res = CameraController::GetLastResolvedProfile();
            return res && (res == &setH.weaponsBlockingShieldSprint ||
                           res == setH.IndoorVariantOf(&setH.weaponsBlockingShieldSprint));
        }

        // Close any ALREADY-OPEN animated-camera window by executing the
        // engine's own End handler out of the HandlerDictionary — the exact
        // machinery a real 'EndAnimatedCamera' graph event runs.
        //
        // Why this exists (the 16:37:57 leak, adjudicated from the log):
        // plain sprint fires its own forwarded Start (the stride bob — by
        // design untouched), and raising block MID-SPRINT flows straight
        // into the charge. The charge's Start was duly SUPPRESSED — but the
        // mode was already open from the plain-sprint window, needed no new
        // Start, and played the charge's camera track anyway: full +15.33°,
        // then the engine's end-blend snapped it back in <200 ms (the "exit
        // snap"). Suppressing Starts is porous by construction; closing the
        // mode at suppression time is not. A stray End on a closed mode is
        // a normal animation-land occurrence the engine already tolerates.
        void CloseAnimatedCameraMode(RE::Actor& a_actor)
        {
            // History of failure, fully adjudicated 2026-08-29 21:50: the
            // HandlerDictionary lookup read NOT FOUND on every call (the
            // dictionary creates handlers through its miss policy on the
            // ENGINE's lookup path — a raw find() bypasses creation), and
            // NotifyAnimationGraph read FAILED (annotation handlers are fed
            // by clip payloads, not by events posted into the graph). Both
            // are gone. The one mechanism left is the only honest one:
            // REPLAY a real dispatch — the End handler's own function
            // pointer and its own singleton, both captured live from the
            // engine's genuine End dispatches by the passthrough hook.
            const RE::BSFixedString key("EndAnimatedCamera");
            void* thisPtr = sAnimCamEndThis.load(std::memory_order_relaxed);
            const bool realThis = thisPtr != nullptr;
            if (!thisPtr) thisPtr = sAnimCamAnyThis.load(std::memory_order_relaxed);
            const char* how = "UNAVAILABLE";
            if (sOrigAnimCamEndExecute && thisPtr) {
                sOrigAnimCamEndExecute(thisPtr, a_actor, key);
                how = realThis ? "replayed (real End singleton)"
                               : "replayed (flyweight fallback this)";
            }
            static int sCloseLog = 0;
            if (sCloseLog < 30) {
                ++sCloseLog;
                spdlog::debug("[ANIMCAM-HOOK] close-mode: {}", how);
            }
        }

        // Shared body for BOTH Start handlers. The 10:54 leak's lesson: the
        // engine registers AnimatedCameraStartHandler (plain) alongside the
        // Delta one, and only the Delta was hooked — a plain Start opened
        // the mode invisibly (no log line, no suppression). Both dispatch
        // paths now flow through here, tagged so the log names which fired.
        bool AnimCamStartCommon(void* a_this, RE::Actor& a_actor,
                                const RE::BSFixedString& a_tag,
                                AnimHandlerExecute_t a_orig, const char* a_kind)
        {
            sAnimCamAnyThis.store(a_this, std::memory_order_relaxed);
            const bool suppress = ShouldSuppressAnimatedCamera(a_actor);
            // Every call is logged (capped), verdict included — a silent
            // path is unadjudicable, proven three times now.
            static int sACHLog = 0;
            if (sACHLog < 200) {
                ++sACHLog;
                auto* ply = RE::PlayerCharacter::GetSingleton();
                const auto msAgo = NowMs() - sLastBlockingMs.load(std::memory_order_relaxed);
                spdlog::debug("[ANIMCAM-HOOK] {} called: actor={} player={} "
                             "sprinting={} lastBlock={}ms ago -> {}",
                             a_kind, a_actor.GetFormID(), &a_actor == ply,
                             a_actor.AsActorState() && a_actor.AsActorState()->IsSprinting(),
                             msAgo > 999999 ? 999999 : msAgo,
                             suppress ? "SUPPRESSED" : "forwarded");
            }
            if (&a_actor == RE::PlayerCharacter::GetSingleton())
                sAnimCamLastStartForwarded.store(!suppress, std::memory_order_relaxed);
            if (suppress) {
                CloseAnimatedCameraMode(a_actor);
                return true;   // handled, engine does nothing
            }
            return a_orig(a_this, a_actor, a_tag);
        }

        bool HookedAnimCamDeltaExecute(void* a_this, RE::Actor& a_actor,
                                       const RE::BSFixedString& a_tag)
        {
            return AnimCamStartCommon(a_this, a_actor, a_tag,
                                      sOrigAnimCamDeltaExecute, "DeltaStart");
        }

        bool HookedAnimCamStartExecute(void* a_this, RE::Actor& a_actor,
                                       const RE::BSFixedString& a_tag)
        {
            return AnimCamStartCommon(a_this, a_actor, a_tag,
                                      sOrigAnimCamStartExecute, "Start");
        }

        // Pure passthrough on the END handler: captures the real singleton
        // (CloseAnimatedCameraMode replays it) and logs the engine's genuine
        // End dispatches so the close/open ledger is complete.
        bool HookedAnimCamEndExecute(void* a_this, RE::Actor& a_actor,
                                     const RE::BSFixedString& a_tag)
        {
            sAnimCamEndThis.store(a_this, std::memory_order_relaxed);
            static int sEndLog = 0;
            if (sEndLog < 60 && &a_actor == RE::PlayerCharacter::GetSingleton()) {
                ++sEndLog;
                spdlog::debug("[ANIMCAM-HOOK] End dispatched for the player (engine-genuine)");
            }
            return sOrigAnimCamEndExecute(a_this, a_actor, a_tag);
        }

        // MSVC x64 RTTI walk: TypeDescriptor by mangled name -> Complete
        // Object Locator (sig 1, matching TD RVA) -> the vtable whose meta
        // pointer references that COL. Every step validated; any miss aborts
        // the install and leaves the game untouched.
        void InstallAnimatedCameraEventHook()
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
            if (!base) return;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto* sec = IMAGE_FIRST_SECTION(nt);

            std::uintptr_t rdataA = 0, rdataSz = 0, dataA = 0, dataSz = 0,
                           textA = 0, textSz = 0;
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                const auto& s = sec[i];
                const std::uintptr_t va = base + s.VirtualAddress;
                if (memcmp(s.Name, ".rdata", 6) == 0) { rdataA = va; rdataSz = s.Misc.VirtualSize; }
                else if (memcmp(s.Name, ".data", 6) == 0) { dataA = va; dataSz = s.Misc.VirtualSize; }
                // The Steam exe carries TWO sections named ".text": the real
                // 24 MB executable one, and a 6 KB NON-executable stub later
                // in the image. Overwriting on every name match made the
                // bounds check test against the stub and refuse a perfectly
                // good vfunc (measured 2026-08-28). Require the EXECUTE
                // characteristic and keep the first match.
                else if (memcmp(s.Name, ".text", 6) == 0 && !textA &&
                         (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
                    textA = va; textSz = s.Misc.VirtualSize;
                }
            }
            if (!rdataA || !textA) return;

            auto findBytes = [](std::uintptr_t a_start, std::uintptr_t a_size,
                                const void* a_pat, std::size_t a_len) -> std::uintptr_t {
                const auto* hay = reinterpret_cast<const unsigned char*>(a_start);
                const auto* pat = reinterpret_cast<const unsigned char*>(a_pat);
                if (a_size < a_len) return 0;
                for (std::uintptr_t i = 0; i + a_len <= a_size; ++i) {
                    if (hay[i] == pat[0] && memcmp(hay + i, pat, a_len) == 0)
                        return a_start + i;
                }
                return 0;
            };

            // One walk per handler class. The 10:54 leak: only the Delta
            // handler was hooked, and a plain StartAnimatedCamera opened the
            // mode with no log line and nothing to suppress it.
            auto hookHandler = [&](const char* a_name, std::size_t a_nameLen,
                                   std::uintptr_t a_hookFn,
                                   AnimHandlerExecute_t& a_origOut,
                                   const char* a_label) {
                // TypeDescriptor: name string sits at TD+0x10. Names live in .data.
                std::uintptr_t nameAt = dataA ? findBytes(dataA, dataSz, a_name, a_nameLen) : 0;
                if (!nameAt) nameAt = findBytes(rdataA, rdataSz, a_name, a_nameLen);
                if (!nameAt) {
                    spdlog::warn("[ANIMCAM-HOOK] {}: TypeDescriptor name not found — hook not installed", a_label);
                    return;
                }
                const std::uint32_t tdRVA = static_cast<std::uint32_t>(nameAt - 0x10 - base);

                // COL: dwords { sig=1, off=0, cdOff=0, tdRVA, cdRVA, selfRVA }.
                std::uintptr_t col = 0;
                for (std::uintptr_t p = rdataA; p + 0x18 <= rdataA + rdataSz; p += 4) {
                    const auto* d = reinterpret_cast<const std::uint32_t*>(p);
                    if (d[0] == 1 && d[3] == tdRVA) { col = p; break; }
                }
                if (!col) {
                    spdlog::warn("[ANIMCAM-HOOK] {}: CompleteObjectLocator not found — hook not installed", a_label);
                    return;
                }

                // vtable: the pointer-sized slot holding &COL, vtable begins after it.
                std::uintptr_t vtbl = 0;
                for (std::uintptr_t p = rdataA; p + 8 <= rdataA + rdataSz; p += 8) {
                    if (*reinterpret_cast<const std::uintptr_t*>(p) == col) { vtbl = p + 8; break; }
                }
                if (!vtbl) {
                    spdlog::warn("[ANIMCAM-HOOK] {}: vtable not found — hook not installed", a_label);
                    return;
                }
                auto* slots = reinterpret_cast<std::uintptr_t*>(vtbl);
                const std::uintptr_t exec = slots[1];
                if (exec < textA || exec >= textA + textSz) {
                    spdlog::warn("[ANIMCAM-HOOK] {}: vfunc[1] (0x{:X}) is not in .text — refusing to hook",
                                 a_label, exec);
                    return;
                }

                DWORD oldProt = 0;
                if (!VirtualProtect(&slots[1], sizeof(std::uintptr_t), PAGE_READWRITE, &oldProt)) {
                    spdlog::warn("[ANIMCAM-HOOK] {}: VirtualProtect failed — hook not installed", a_label);
                    return;
                }
                a_origOut = reinterpret_cast<AnimHandlerExecute_t>(exec);
                slots[1] = a_hookFn;
                VirtualProtect(&slots[1], sizeof(std::uintptr_t), oldProt, &oldProt);
                spdlog::info("[ANIMCAM-HOOK] {}::ExecuteHandler hooked "
                             "(vtbl=0x{:X}, orig=0x{:X}) — found by RTTI name, no fixed offsets",
                             a_label, vtbl - base, exec - base);
            };

            static constexpr char kDeltaName[] = ".?AVAnimatedCameraDeltaStartHandler@@";
            static constexpr char kStartName[] = ".?AVAnimatedCameraStartHandler@@";
            static constexpr char kEndName[]   = ".?AVAnimatedCameraEndHandler@@";
            hookHandler(kDeltaName, sizeof(kDeltaName),
                        reinterpret_cast<std::uintptr_t>(&HookedAnimCamDeltaExecute),
                        sOrigAnimCamDeltaExecute, "AnimatedCameraDeltaStartHandler");
            hookHandler(kStartName, sizeof(kStartName),
                        reinterpret_cast<std::uintptr_t>(&HookedAnimCamStartExecute),
                        sOrigAnimCamStartExecute, "AnimatedCameraStartHandler");
            hookHandler(kEndName, sizeof(kEndName),
                        reinterpret_cast<std::uintptr_t>(&HookedAnimCamEndExecute),
                        sOrigAnimCamEndExecute, "AnimatedCameraEndHandler");
        }
    }

    void HookManager::Install()
    {
        spdlog::debug("[TWEEN-TRACE] v3 available with Verbose Logging: two captures, 20 seconds after close, 60-second maximum");
        InstallAnimatedCameraEventHook();
        AnimationCameraController::GetSingleton().InstallHook();
        // Warn about conflicting camera mods
        const char* conflicts[] = { "SmoothCam.dll", "ImprovedCameraSE.dll" };
        for (auto* name : conflicts) {
            if (GetModuleHandleA(name)) {
                spdlog::warn("HookManager: detected conflicting camera mod: {} â€” may cause issues", name);
            }
        }

        // [SHOULDER] the vanilla over-shoulder pair, logged once.
        //
        // These are INI settings ("...:Camera"), NOT GameSettings — a
        // GameSettingCollection lookup returns null and prints nan, which is
        // how the first attempt to read them was wasted.
        //
        // The 15:11 run equalised CombatPosX to PosX in memory to test whether
        // the menu walk was the engine sliding between the pair. It was NOT:
        // this setup reads PosX=30 CombatPosX=0 PosZ=-10 CombatPosZ=20
        // AddY=-100, so the walk's 25.00 endpoint was never the combat
        // shoulder, and equalising them left the walk untouched. The
        // experiment is removed; the carrier is the animated-camera delta at
        // ThirdPersonState+0xC0 — see [ANIMDELTA] in HookedThirdPersonUpdate.
        // The line stays because a preset that looks wrong on someone else's
        // machine wants these five numbers in the log.
        if (auto* ini = RE::INISettingCollection::GetSingleton()) {
            auto* posX    = ini->GetSetting("fOverShoulderPosX:Camera");
            auto* combatX = ini->GetSetting("fOverShoulderCombatPosX:Camera");
            auto* posZ    = ini->GetSetting("fOverShoulderPosZ:Camera");
            auto* combatZ = ini->GetSetting("fOverShoulderCombatPosZ:Camera");
            auto* addY    = ini->GetSetting("fOverShoulderCombatAddY:Camera");
            spdlog::info("[SHOULDER] INI pair: PosX={} CombatPosX={} PosZ={} CombatPosZ={} AddY={}",
                         posX ? posX->GetFloat() : -999.0f,
                         combatX ? combatX->GetFloat() : -999.0f,
                         posZ ? posZ->GetFloat() : -999.0f,
                         combatZ ? combatZ->GetFloat() : -999.0f,
                         addY ? addY->GetFloat() : -999.0f);
        }

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ThirdPersonState[0] };
        _originalThirdPersonUpdate = vtbl.write_vfunc(0x3, &HookedThirdPersonUpdate);
        spdlog::info("HookManager: ThirdPersonState::Update hooked");

        _originalTrackingUpdateRotation = vtbl.write_vfunc(0xE, &HookedTrackingUpdateRotation);
        _originalGetRotation = vtbl.write_vfunc(0x4, &HookedGetRotation);
        spdlog::info("HookManager: ThirdPersonState::GetRotation hooked");

        _originalGetTranslation = vtbl.write_vfunc(0x5, &HookedGetTranslation);
        spdlog::info("HookManager: ThirdPersonState::GetTranslation hooked (post-update)");

        _originalProcessWeaponDrawnChange = vtbl.write_vfunc(0xB, &HookedProcessWeaponDrawnChange);
        spdlog::info("HookManager: ThirdPersonState::ProcessWeaponDrawnChange hooked (draw-edge framing guard)");

        // TogglePOVHandler::ProcessButton â€” vtable slot 4. Lets us catch the
        // "Toggle POV" user event (keyboard F, controller R3-hold) and call
        // PlayerCamera::ForceFirstPerson/ForceThirdPerson directly, bypassing
        // the zoom axis so our zoom hard-lock doesn't block POV entry.
        REL::Relocation<std::uintptr_t> povVtbl{ RE::VTABLE_TogglePOVHandler[0] };
        _originalTogglePOVProcessButton    = povVtbl.write_vfunc(RuntimeHooks::InputSlot(0x4), &HookedTogglePOVProcessButton);
        _originalTogglePOVUpdateHeldState  = povVtbl.write_vfunc(RuntimeHooks::InputSlot(0x5), &HookedTogglePOVUpdateHeldState);
        spdlog::info("HookManager: TogglePOVHandler::ProcessButton hooked");
        spdlog::info("HookManager: TogglePOVHandler::UpdateHeldStateActive hooked");

        // Vanilla also receives POV input on each camera state's secondary
        // vtable. Its release handler resets freeRotation, independently of
        // TogglePOVHandler and TDM's separate failed-target reset preference.
        REL::Relocation<std::uintptr_t> orbitInputVtbl{ RE::VTABLE_ThirdPersonState[1] };
        REL::Relocation<std::uintptr_t> horseInputVtbl{ RE::VTABLE_HorseCameraState[1] };
        _originalOrbitProcessButton = orbitInputVtbl.write_vfunc(RuntimeHooks::InputSlot(0x4), &HookedOrbitProcessButton);
        _originalHorseOrbitProcessButton = horseInputVtbl.write_vfunc(RuntimeHooks::InputSlot(0x4), &HookedOrbitProcessButton);
        spdlog::info("HookManager: native R3 orbit reset disabled for third-person and mounted camera input");

        // HorseCameraState inherits from ThirdPersonState but has its OWN vtable.
        //
        // THE RETURNED POINTERS ARE NOW KEPT, AND THIS IS NOT COSMETIC.
        // The old code wrote the same three hooks in and THREW THE ORIGINALS
        // AWAY, on the assumption that the horse merely inherits these slots so
        // calling _originalThirdPersonUpdate would be equivalent. Two things
        // are wrong with that:
        //
        //   1. It assumes, where the DragonCameraState block below CHECKS. If
        //      the horse overrides any of these, its override stopped running
        //      the moment DDC loaded.
        //   2. Far worse, it is only true if nothing else is hooked there.
        //      True Directional Movement hooks HorseCameraState. Installing
        //      over its pointer and then calling ThirdPersonState's instead
        //      SILENTLY UNHOOKS TDM for the entire mounted camera â€” its horse
        //      tracking never updates, and when its OnEnterState finally runs
        //      it dereferences a horse that was never resolved:
        //
        //        EXCEPTION_ACCESS_VIOLATION, subss xmm0,[rdi+0x50], rdi = 0
        //        Hooks::HorseCameraStateHook::OnEnterState
        //
        //      Reproduced on dismount twice (2026-08-23 18:04 and 18:19).
        //
        // Keeping the displaced pointer and chaining to IT is correct in every
        // case: identical when the slot really was inherited, preserves the
        // horse's own override when it is not, and preserves another mod's
        // hook when one is present. [[compatibility-first-design]].
        REL::Relocation<std::uintptr_t> horseVtbl{ RE::VTABLE_HorseCameraState[0] };
        _originalHorseTrackingUpdateRotation = horseVtbl.write_vfunc(0xE, &HookedTrackingUpdateRotation);
        spdlog::info("HookManager: mounted tracking v2 installed after chained UpdateRotation");
        _originalHorseUpdate         = horseVtbl.write_vfunc(0x3, &HookedThirdPersonUpdate);
        _originalHorseGetRotation    = horseVtbl.write_vfunc(0x4, &HookedGetRotation);
        _originalHorseGetTranslation = horseVtbl.write_vfunc(0x5, &HookedGetTranslation);
        // SLOT 0xB TOO, and this one is not inherited — HorseCameraState.h
        // declares `void ProcessWeaponDrawnChange(bool) override;`. Hooking it
        // only on ThirdPersonState meant the draw-edge guard NEVER RAN while
        // mounted, which is why drawing a weapon on a horse moved the camera
        // 150 units and no amount of profile tuning could match Horseback to
        // Horseback Melee. Measured by [MOUNTPROF]: identical profiles,
        // posY -300 sheathed vs -150 drawn.
        _originalHorseProcessWeaponDrawnChange =
            horseVtbl.write_vfunc(0xB, &HookedProcessWeaponDrawnChange);
        spdlog::info("HookManager: HorseCameraState::ProcessWeaponDrawnChange hooked ({} "
                     "ThirdPersonState's — expected OVERRIDDEN)",
                     _originalHorseProcessWeaponDrawnChange.address() ==
                         _originalProcessWeaponDrawnChange.address()
                         ? "same as" : "differs from");
        spdlog::info("HookManager: HorseCameraState vtable hooks installed "
                     "(Update {}, GetRotation {}, GetTranslation {} vs ThirdPersonState)",
                     _originalHorseUpdate.address()      == _originalThirdPersonUpdate.address()
                         ? "inherited" : "OVERRIDDEN-OR-HOOKED",
                     _originalHorseGetRotation.address() == _originalGetRotation.address()
                         ? "inherited" : "OVERRIDDEN-OR-HOOKED",
                     _originalHorseGetTranslation.address() == _originalGetTranslation.address()
                         ? "inherited" : "OVERRIDDEN-OR-HOOKED");

        // DragonCameraState: CommonLibSSE has no header for it, so we can't
        // verify its layout. Safety check â€” for each slot we want to hook,
        // only patch if its current value matches the corresponding
        // ThirdPersonState entry (i.e. inherited, not overridden). Hooking
        // an overridden slot would break dragon-specific camera behavior.
        {
            REL::Relocation<std::uintptr_t> dragonVtbl{ RE::VTABLE_DragonCameraState[0] };

            // Records the displaced pointer so the dispatch helpers chain to
            // the dragon's own slot. It can only equal `inheritedPtr` here
            // (that is the condition for patching at all), but storing it
            // keeps every derived vtable following the same rule instead of
            // one of them relying on a coincidence.
            const auto patchIfInherited = [&](std::size_t slot, std::uintptr_t inheritedPtr,
                                              void* hookedPtr, const char* name,
                                              std::uintptr_t* outOriginal) {
                const auto slotAddr  = dragonVtbl.address() + slot * sizeof(void*);
                const auto dragonPtr = *reinterpret_cast<std::uintptr_t*>(slotAddr);
                if (dragonPtr == inheritedPtr) {
                    dragonVtbl.write_vfunc(slot, hookedPtr);
                    if (outOriginal) *outOriginal = dragonPtr;
                    spdlog::info("HookManager: DragonCameraState::{} hook installed (inherited from ThirdPersonState)", name);
                } else {
                    spdlog::warn("HookManager: DragonCameraState::{} appears overridden (0x{:x} vs 0x{:x}) â€” skipping hook.", name, dragonPtr, inheritedPtr);
                }
            };

            std::uintptr_t dragonUpd = 0, dragonRot = 0;
            patchIfInherited(0x3, _originalThirdPersonUpdate.address(), reinterpret_cast<void*>(&HookedThirdPersonUpdate), "Update",      &dragonUpd);
            patchIfInherited(0x4, _originalGetRotation.address(),       reinterpret_cast<void*>(&HookedGetRotation),       "GetRotation", &dragonRot);
            if (dragonUpd) _originalDragonUpdate      = REL::Relocation<decltype(&HookedThirdPersonUpdate)>(dragonUpd);
            if (dragonRot) _originalDragonGetRotation = REL::Relocation<decltype(&HookedGetRotation)>(dragonRot);
        }

        // BleedoutCameraState vtable â€” Begin + Update. GetRotation/
        // GetTranslation are inherited from ThirdPersonState and left
        // untouched so bleedout's head-bone tracking stays intact.
        REL::Relocation<std::uintptr_t> bleedoutVtbl{ RE::VTABLE_BleedoutCameraState[0] };
        _originalBleedoutBegin  = bleedoutVtbl.write_vfunc(0x1, &HookedBleedoutBegin);
        _originalBleedoutEnd    = bleedoutVtbl.write_vfunc(0x2, &HookedBleedoutEnd);
        _originalBleedoutUpdate = bleedoutVtbl.write_vfunc(0x3, &HookedBleedoutUpdate);
        spdlog::info("HookManager: BleedoutCameraState::Begin hooked");
        spdlog::info("HookManager: BleedoutCameraState::End hooked");
        spdlog::info("HookManager: BleedoutCameraState::Update hooked");

        // TweenMenuCameraState::Update — see the header note. The tween
        // state's own ease is the writer no later hook could beat.
        REL::Relocation<std::uintptr_t> tweenVtbl{ RE::VTABLE_TweenMenuCameraState[0] };
        _originalTweenMenuUpdate = tweenVtbl.write_vfunc(0x3, &HookedTweenMenuUpdate);
        spdlog::info("HookManager: TweenMenuCameraState::Update hooked (menu FOV freeze)");

        // Furniture camera kill switch â€” port of Ersh's NoFurnitureCamera.
        // The previous downstream attempts (FurnitureCameraState::Begin
        // hooks, save+restore of NiCamera/cameraRoot/player angle,
        // PlayerCameraTransitionState abort) couldn't suppress the
        // entry/exit snap because the engine has already written the
        // chair-facing pose to multiple places before any of those
        // hooks fire. The actual entry point is a `call` inside
        // TESFurniture::Activate that asks the camera to enter
        // kFurniture; replacing that call with our own thunk means
        // the engine never even REQUESTS the kFurniture state.
        // See `EnterFurniture` for the dispatch logic.
        InstallFurnitureCameraKill();

        // FirstPersonState vtable â€” only Update is hooked so we can override
        // world/hands FOV while the player is in first person. Other slots
        // (GetRotation/GetTranslation) are left to the engine so 1p aiming
        // and head-tracking stay untouched.
        REL::Relocation<std::uintptr_t> fpVtbl{ RE::VTABLE_FirstPersonState[0] };
        _originalFirstPersonUpdate      = fpVtbl.write_vfunc(0x3, &HookedFirstPersonUpdate);
        InstallUpdateCameraPostHook();
        InstallNiCameraUpdateWorldDataHook();
        spdlog::info("HookManager: FirstPersonState::Update hooked");

        // PlayerCameraTransitionState::Update â€” lets us abort a 1pâ†’3p
        // transition that the engine starts while R3 is being held
        // with a 1p press, so no mid-transition 3p frames leak through.
        REL::Relocation<std::uintptr_t> transVtbl{ RE::VTABLE_PlayerCameraTransitionState[0] };
        _originalTransitionUpdate = transVtbl.write_vfunc(0x3, &HookedTransitionUpdate);
        spdlog::info("HookManager: PlayerCameraTransitionState::Update hooked");

        // MenuControls::ProcessEvent â€” live-gated dialogue-movement unlock.
        REL::Relocation<std::uintptr_t> mcVtbl{ RE::VTABLE_MenuControls[0] };
        _originalMenuControlsProcessEvent = mcVtbl.write_vfunc(0x1, &HookedMenuControlsProcessEvent);
        spdlog::info("HookManager: MenuControls::ProcessEvent hooked (dialogue-movement unlock)");

        // Save original bytes of the camera collision raycast function.
        // Used as a fallback if the trampoline detour below fails to install:
        // we can still patch xor/ret to honor the master "Disable Camera
        // Collision" toggle. With the detour active, byte-level patching is
        // dormant â€” the detour reads settings live every call.
        REL::Relocation<std::uintptr_t> casterAddr{ RELOCATION_ID(32270, 33007) };
        cameraCasterAddr = casterAddr.address();
        std::memcpy(cameraCasterOrigBytes, reinterpret_cast<void*>(cameraCasterAddr), 6);
        spdlog::info("HookManager: CameraCaster at 0x{:x}, original bytes saved", cameraCasterAddr);
        CameraCollision::Install();

        // Build a 14-byte absolute-jump stub in trampoline memory:
        //   FF 25 00 00 00 00            jmp qword ptr [rip+0]
        //   <8-byte abs addr>            &HookedCameraCaster
        // A 5-byte rel32 JMP written at the function entry (within +/-2GB of the
        // trampoline) can then reach our 64-bit hook. Allocated once; the
        // diversion itself is toggled in UpdateCameraCasterPatch.
        if (auto* stub = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(14))) {
            stub[0] = 0xFF;
            stub[1] = 0x25;
            *reinterpret_cast<std::uint32_t*>(stub + 2) = 0;
            *reinterpret_cast<std::uint64_t*>(stub + 6) =
                reinterpret_cast<std::uint64_t>(&HookManager::HookedCameraCaster);
            cameraCasterDivertStub = reinterpret_cast<std::uintptr_t>(stub);
            spdlog::info("HookManager: CameraCaster divert stub at 0x{:x}", cameraCasterDivertStub);
        } else {
            spdlog::warn("HookManager: failed to allocate CameraCaster divert stub; "
                         "per-layer collision exceptions will fall back to full pass-through");
        }

        // Camera Noise â€” trampoline the call to TESCamera::Update inside
        // PlayerCamera::Update (doodlum's exact site). Post-original, we
        // overlay additive Perlin noise + impulse shakes on cameraRoot->local.
        CameraNoiseController::GetSingleton().InstallHook();

        // Event-beat signal intake: Explosion::Initialize +
        // ImageSpaceModifierInstanceForm::Apply vfunc hooks. The handlers
        // early-out on an atomic flag when no beat is enabled, so an
        // untouched install pays two compares per explosion / imod frame.
    }


    namespace { std::atomic<bool> s_lastFrameWasFirstPerson{false}; }
    // See HookManager::SetUiDriveActive.
    namespace { std::atomic<bool> s_uiDriveActive{false}; }

    bool HookManager::WasLastFrameFirstPerson()
    {
        return s_lastFrameWasFirstPerson.load(std::memory_order_relaxed);
    }

    namespace
    {
        struct MenuAnimationSnapshot
        {
            std::array<float, 4> values{};
            RE::ThirdPersonState* state = nullptr;
            RE::NiAVObject* playerRoot = nullptr;
            bool hadAnimationSource = false;
            bool valid = false;
        };

        MenuAnimationSnapshot sLastCameraAnimation;
        MenuAnimationSnapshot sPendingMenuAnimation;

        void RememberCameraAnimation(RE::ThirdPersonState* state)
        {
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!state || state->id != RE::CameraState::kThirdPerson ||
                !camera || camera->currentState.get() != state ||
                !player || !player->Get3D()) {
                sLastCameraAnimation.valid = false;
                return;
            }
            std::memcpy(sLastCameraAnimation.values.data(), &state->unkC0,
                        sizeof(sLastCameraAnimation.values));
            sLastCameraAnimation.state = state;
            sLastCameraAnimation.playerRoot = player->Get3D();
            sLastCameraAnimation.hadAnimationSource = state->unkA0 != 0;
            sLastCameraAnimation.valid = true;
        }

        bool RestorePendingMenuAnimation(RE::ThirdPersonState* state)
        {
            if (!sPendingMenuAnimation.valid) return false;
            const auto saved = sPendingMenuAnimation;
            sPendingMenuAnimation.valid = false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!state || state != saved.state || state->id != RE::CameraState::kThirdPerson ||
                !player || player->Get3D() != saved.playerRoot) return false;

            std::array<float, 4> before{};
            std::memcpy(before.data(), &state->unkC0, sizeof(before));
            TweenCameraTrace::Sample("menu-animation-before", true);
            std::memcpy(&state->unkC0, saved.values.data(), sizeof(saved.values));
            TweenCameraTrace::Sample("menu-animation-after", true);
            spdlog::debug("[ANIMDELTA] menu-edge restore ({:.3f},{:.3f},{:.3f}) blend={:.3f} "
                         "-> ({:.3f},{:.3f},{:.3f}) blend={:.3f}; engine resumes normally",
                         before[0], before[1], before[2], before[3],
                         saved.values[0], saved.values[1], saved.values[2], saved.values[3]);
            return true;
        }
    }

    void HookManager::PreserveMenuCameraAnimation()
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!sLastCameraAnimation.valid || WasLastFrameFirstPerson() ||
            !camera || !camera->currentState || !player || player->IsOnMount() ||
            player->IsDead() || player->Get3D() != sLastCameraAnimation.playerRoot ||
            SettingsManager::GetSingleton().diagnosticSuspendOverrides) return;
        const auto state = camera->currentState->id;
        if (state != RE::CameraState::kThirdPerson && state != RE::CameraState::kTween) return;
        sPendingMenuAnimation = sLastCameraAnimation;
    }

    void HookManager::RestoreMenuCameraAnimationSource(RE::ThirdPersonState* state)
    {
        const auto& saved = sPendingMenuAnimation;
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!saved.valid || saved.hadAnimationSource || !state || state != saved.state ||
            state->id != RE::CameraState::kThirdPerson || !camera ||
            camera->currentState.get() != state || !player || player->Get3D() != saved.playerRoot ||
            SettingsManager::GetSingleton().diagnosticSuspendOverrides) return;

        auto* source = reinterpret_cast<RE::NiAVObject*>(state->unkA0);
        if (!source) return;
        TweenCameraTrace::Sample("menu-source-before", true);
        state->unkA0 = 0;
        source->DecRefCount();
        TweenCameraTrace::Sample("menu-source-after", true);
        spdlog::debug("[ANIMDELTA] unframed menu re-entry acquired an inactive animation source; "
                     "released the new reference, preserving the pre-menu camera channel");
    }

    void HookManager::ResetMenuCameraAnimation()
    {
        sLastCameraAnimation.valid = false;
        sPendingMenuAnimation.valid = false;
    }

    bool HookManager::IsDialogueFacingBodyYaw()
    {
        return s_dlgFaceYawTaken;
    }

    namespace
    {
        // ================================================================
        // [SNAP] â€” the menu-camera JUMP detector. (2026-08-19)
        //
        // Replaces [SNAPXRAY], the 2026-08-14 three-stage probe that asked
        // "which pipeline stage writes the Tween-open +4.8u?". That question
        // is ANSWERED and the answer is "none of them": the 19:11 capture
        // shows tpsIn r41.16 -> niCamOut r41.25 -> tpsOut r41.25 -> next
        // frame r41.14, i.e. every stage agrees to within 0.1u across the
        // whole activation. Meanwhile the probe's steady sampler wrote 22,535
        // lines â€” 82% of the entire log â€” measuring a solved problem, and
        // still could not see the snap the user reports, because it only ever
        // armed for 24 frames (~50ms) on the menu OPEN edge. A snap on CLOSE,
        // or 200ms into the menu, was outside the window every single time.
        //
        // So: sample ONE number per frame and never print it. The ring buffer
        // runs always and costs a handful of floats; lines are written only
        // when the camera actually JUMPS inside a menu window, and then with
        // real before/after context instead of a 50ms keyhole.
        //
        // WHY MENU-SCOPED: measured in the player-yaw frame, ordinary play
        // legitimately moves the camera (preset transitions, look, zoom), so
        // a bare threshold would fire constantly. While a menu is up the
        // player is still and DDC pins the framing, so ANY per-frame motion
        // past ~1.5u is by definition the artefact.
        struct SnapFrame
        {
            float t        = 0.0f;   // seconds since the window armed
            float r        = 0.0f;   // rendered camera in the player-yaw frame
            float f        = 0.0f;
            float u        = 0.0f;
            float dCam     = 0.0f;   // |this - previous| of (r,f,u)
            float dPlayer  = 0.0f;   // player xy travel this frame
            float side     = 0.0f;   // what DDC asked the engine for
            float height   = 0.0f;
            float zoomCur  = 0.0f;
            float zoomTgt  = 0.0f;
            float freeRotX = 0.0f;
            int   camState = -1;
            bool  spim     = false;
            // ---- added 2026-08-20, to end the +4.8u argument -----------------
            // Four investigations have now pinned cameraRoot (d=0), pinned
            // ThirdPersonState::translation (d=0) and re-asserted every data
            // channel, and the lateral bias survives all of it: rendered r sits
            // a CONSTANT 5.0u short of posOffsetActual.x during play (25.00 vs
            // 30.0, and -4.80 vs 0.2 at a different framing â€” a difference, not
            // a ratio), and closes to ~0 on the menu frame.
            //
            // A constant additive lateral that appears on the spim edge and is
            // invisible to every pin has one suspect nobody has excluded: this
            // probe READS TWO DIFFERENT NODES. `cw` prefers the NiCamera child
            // and falls back to cameraRoot when the child isn't there. If the
            // child carries a local offset and the menu path changes which node
            // we find, the "jump" is the MEASUREMENT moving, not the camera. So
            // record both, unconditionally, and whether the child was found.
            float rootR    = 0.0f;   // cameraRoot only, player-yaw frame
            float niR      = 0.0f;   // NiCamera child only (== rootR if absent)
            bool  haveNi   = false;
            // The other live suspect: the engine only PARTIALLY applies the
            // over-shoulder offset in the combat-ready pose unless toggleAnimCam
            // is set (see the note at ApplyFraming's toggleAnimCam write). A
            // partial apply is exactly a constant shortfall against `side`.
            bool  animCam  = false;
            bool  drawn    = false;
            // posOffsetActual.x as it stood at the TOP of PlayerCamera::Update,
            // before the engine's own pass ran (2026-08-20 round 2). SnapTick
            // samples after that pass, so `side` is the post-update value; if
            // these two disagree the engine is rewriting the field mid-frame
            // and the render is reading its value, not ours. That is the last
            // unexcluded explanation for the steady 5.00u shortfall now that
            // the node-swap and toggleAnimCam suspects are both dead.
            float preX     = 0.0f;
            // The noise controller's TOTAL applied 3p translation this frame
            // (ambient + bob + repulse, everything it adds to cameraRoot
            // late), projected onto the same lateral axis as `r`
            // (2026-09-04). Every engine-side suspect for the steady
            // shortfall is now excluded by measurement — preX == side every
            // frame, collision state unchanged across the open — so the last
            // candidate is a DDC layer applied AFTER the engine composes. If
            // shortfall == -n3pR on the steady frames and n3pR reads ~0 on
            // the open frame, the whole Tween pop is this layer being dropped
            // for a frame and ramped back.
            float n3pR     = 0.0f;
            bool  n3pAny   = false;
            // The pipeline split that decides it (2026-09-05): `engR` is the
            // camera's lateral as the ENGINE composed it this frame, read at
            // the top of the looseness block before anything of ours is
            // added; `looseR` is what the looseness block then added. The
            // 09:45 capture read noise_r=+0.46 against a shortfall of 4.99,
            // so the noise is cleared; if engR == side and looseR == -5 the
            // follow lag is the layer the Tween open drops for a frame.
            float engR       = 0.0f;
            float looseR     = 0.0f;
            bool  looseValid = false;
            // ---- 2026-09-05: the engine's own terms, so the steady 5u names
            // itself. The 09:45:34 capture had the shortfall at 4.96 while the
            // player was still decelerating AND at 4.99 after fifteen
            // stationary frames - no lag term does that, and the noise stack
            // measured 0.46. What is left is the composition itself: the
            // pivot the engine offsets from (the skeleton's Camera3rd node),
            // ThirdPersonState::translation and collisionPos, and the root's
            // own local translate. Each projected on the player's lateral
            // axis, relative to the player root.
            float cam3R      = 0.0f;   // "Camera3rd [Cam3]" world, lateral
            float headR      = 0.0f;   // "NPC Head [Head]" world, lateral
            float transR     = 0.0f;   // tps->translation, lateral
            float collR      = 0.0f;   // tps->collisionPos, lateral
            float rootLocalR = 0.0f;   // cameraRoot->local.translate, lateral
            bool  haveCam3   = false;
            bool  haveHead   = false;
            bool  haveTps    = false;
            // Yaws, degrees (2026-09-05): the 14:10 capture put the whole
            // gap in ThirdPersonState::translation, which is composed from
            // the state's rotation quaternion - so print every yaw that
            // could feed it, and the rendered one.
            float rotYaw     = 0.0f;   // tps->rotation, as yaw
            float curYaw     = 0.0f;   // tps->currentYaw
            float tgtYaw     = 0.0f;   // tps->targetYaw
            float bodyYaw    = 0.0f;   // player data.angle.z
            float camYaw     = 0.0f;   // cameraRoot world rotate, forward yaw
        };
        // Published by the looseness block in HookedUpdateCameraPost for the
        // [SNAP] probe: the engine-composed camera position it started from
        // and the world-space offset it added on top. Valid only on frames
        // the block ran.
        RE::NiPoint3 s_looseEngineCam{};
        RE::NiPoint3 s_looseApplied{};
        bool         s_looseValid = false;
        constexpr int kSnapRing = 24;      // ~0.2-0.4s of pre-roll
        SnapFrame s_snapRing[kSnapRing]{};
        int   s_snapRingN    = 0;          // total frames recorded (wraps)
        bool  s_snapPrimed   = false;      // have a previous frame to diff
        float s_snapPrevR    = 0.0f;
        float s_snapPrevF    = 0.0f;
        float s_snapPrevU    = 0.0f;
        bool  s_snapPrevSpim = false;
        float s_snapPrevFreeRotX = 0.0f;   // orbit-motion excuse, see the trigger
        float s_snapPreUpdSideX  = 0.0f;   // see SnapFrame::preX
        // The menu FOV freeze (user ruling 2026-08-31: an unconfigured menu
        // "should have zero movement and behave like a pause"). Latched to
        // the RENDERED worldFOV on every frame no menu window is open, then
        // asserted verbatim for the whole Tween / Inventory / Magic window.
        // The previous source, GetLastAppliedWorldFov, is the 3p pipeline's
        // last value — stale whenever the menu opens from first person or
        // any state that pipeline wasn't ticking in, which is what read as
        // the "flicker snap when menus aren't configured at all".
        float s_menuFovFreeze    = -1.0f;  // <=1 = nothing latched yet
        // Stamped by HookedMenuControlsProcessEvent on the Tween Menu
        // keypress. The ENGINE dilates global time from the keypress,
        // several frames BEFORE the menu registers open ([TWEENSTUT]
        // 2026-08-31: spd 380 -> 40 for five frames at steady dt, menu=0)
        // — the "stutter forward while walking". (Initially misattributed
        // to Tween Menu Overhaul; its DLL was string-dumped 2026-09-01 and
        // contains no camera/timer code at all — the slow-mo, the ±10° FOV
        // ease, and the exit gate are all vanilla TweenMenu behavior.)
        // IsMenuOpen can't see that pre-open window; the raw input event
        // can.
        std::chrono::steady_clock::time_point s_tweenKeyTp{};
        // The 1p writer's most recent worldFOV write. Published so the
        // render-side tween hold can re-assert the LIVE 1p value against
        // menu mods (Tween Menu Overhaul) that rewrite the field after every
        // earlier hook — the NiCamera-time write is what the frustum build
        // consumes ([TWEENFOV] frustum column, 2026-08-31), so whoever
        // writes there controls the screen. 3p publishes via
        // GetLastAppliedWorldFov already.
        float s_fpLastWrittenFov = -1.0f;
        // [TWEENSTUT] open-edge motion probe: "stutter forward if walking
        // while opening it in first person with unpaused enabled". A dt
        // spike = frame hitch (menu/swf load); a speed dip at steady dt =
        // input drop; both steady = camera-side. Ring holds the pre-roll.
        struct TweenStutFrame { float dtMs; float spd; int cs; float camDp; };
        TweenStutFrame s_tsRing[10] = {};
        int  s_tsRingN    = 0;
        int  s_tsLive     = 0;      // frames still to print after an edge
        int  s_tsWindows  = 0;      // info budget
        bool s_tsPrevOpen = false;
        RE::NiPoint3 s_tsPrevPos{};
        std::chrono::steady_clock::time_point s_tsPrevTp{};
        // [TWEENFOV] bracket probe (2026-08-31). The tween FOV ramp survived
        // three fixes because every round guessed the writer; this samples
        // worldFOV at each stage of ONE frame so a single repro names the
        // gap the writer lives in. -1 = not sampled this frame.
        float s_tfStatePre  = -1.0f;   // before TweenMenuCameraState::Update
        float s_tfStatePost = -1.0f;   // after it (before our freeze write)
        float s_tfUcpIn     = -1.0f;   // HookedUpdateCameraPost entry
        float s_tfUcpOut    = -1.0f;   // after its kTween hold
        int   s_tfLines     = 0;       // info cap
        // ----- THE FIRST-PERSON FOLLOW SPRING -------------------------------
        //
        // "Opening the tween menu while in first person makes the character
        // skip forward" (2026-09-01), with unpause on OR off — so it is not
        // the unpause path.
        //
        // FirstPersonState carries a damped follow spring of its own:
        // lastPosition / lastFrameSpringVelocity / dampeningOffset. While the
        // player runs it holds the camera BEHIND the reported player position,
        // converging to ~21u at run speed and back to 0 at rest — measured,
        // not assumed: [SNAP] cam=(r-0.39,f-21.02) at dPly=5.38/frame while
        // walking, 17:08:30. It is invisible in play because it is continuous.
        //
        // The three fields are one mechanism, measured 18:53:21.789:
        // lastPosition is the camera's LAGGED ANCHOR (lastPos - ply == damp on
        // every frame), dampeningOffset is that difference recomputed, and
        // lastFrameSpringVelocity is the anchor's velocity. Re-entering
        // kFirstPerson sets the anchor onto the player — on the return frame
        // and only there, lastPos reads exactly ply — which collapses the
        // trail, and the camera covers all of it at once:
        //     [TWEENCAM] EDGE menu Tween -> Tween | camState 7 -> 0
        //     [SNAP] JUMP cam=(r-0.00,f-0.01) dCam=26.33 dPly=0.00
        // — 26 units forward with the player standing still. That is the skip.
        // The tween ENTRY is innocent (distRoot held 26.39 across the kTween
        // frame); the return is the writer. It is also why third person is
        // perfect and must not be touched: 3p's follow lag is DDC's own
        // Looseness tracker, which lives outside the camera state and survives
        // any number of Begin calls.
        //
        // So carry the spring across the bounce instead of letting it reset:
        // every first-person frame latches these three fields, and the first
        // first-person update after the return writes them back before the
        // engine reads them.
        RE::NiPoint3 s_fpSpringLastPos{};
        RE::NiPoint3 s_fpSpringVel{};
        RE::NiPoint3 s_fpSpringDamp{};
        bool s_fpSpringSaved = false;   // nothing latched yet this session
        bool s_fpSpringHold  = false;   // a return is pending / being held
        float s_fpSpringWind = 0.0f;    // seconds into the release wind-down
        int  s_fpSpringLogs  = 0;       // [FPSPRING] info cap
        // Player position one frame back, for the tween dead-frame carry in
        // HookedNiCameraUpdateWorldData. Latched every rendered frame.
        // POSITIONAL ONLY — this is the distance the held pose is advanced by.
        // It is NOT the clock any longer; see s_tweenCarryPrevCam1.
        RE::NiPoint3 s_tweenCarryPrevPly{};
        bool         s_tweenCarryPrimed = false;
        // THE CLOCK. `Camera1st [Cam1]` — the first-person skeleton's camera
        // node — one rendered pass back. This, not the player's data position,
        // is what the engine's follow spring converges toward, and with an
        // unpaused menu open the two tick on DIFFERENT passes: the camera is
        // pumped twice per rendered frame while the skeleton is refreshed once,
        // so of the two passes only one has a fresh target. Measured
        // 2026-09-02 19:19:27, unpaused Tween window, player running:
        //
        //   ran=1 dPly=5.38 dCam1=0.00  trail 26.36->20.99   <- integrated
        //   ran=0 dPly=0.00 dCam1=5.38  trail 20.99->20.99   <- skipped
        //   ... and after the close, in phase: dPly=5.38 dCam1=5.38, ran=1
        //
        // Every integration inside the window was chasing a one-frame-old
        // target, which is the whole residual artefact. Latched from the
        // render hook for the same reason the stamp above is: the first-person
        // update does not run at all while the camera is parked in kTween.
        RE::NiPoint3 s_tweenCarryPrevCam1{};
        bool         s_tweenCarryCam1Primed = false;

        // The 1p camera node, cached against the 3D it was resolved from so the
        // name walk happens once per skeleton rather than once per pass.
        RE::NiAVObject* FirstPersonCamNode(RE::PlayerCharacter* a_ply)
        {
            static RE::NiAVObject* sFrom = nullptr;
            static RE::NiAVObject* sNode = nullptr;
            auto* fp3 = a_ply ? a_ply->Get3D(true) : nullptr;
            if (!fp3) { sFrom = nullptr; sNode = nullptr; return nullptr; }
            if (fp3 != sFrom || !sNode) {
                sFrom = fp3;
                sNode = fp3->GetObjectByName("Camera1st [Cam1]");
            }
            return sNode;
        }
        // Last moment an unpaused menu window was observed open, stamped from
        // HookedNiCameraUpdateWorldData — which runs on every rendered pass
        // whatever camera state is current. HookedFirstPersonUpdate cannot
        // keep this itself: it does not run AT ALL while the camera is parked
        // in kTween, so a window edge detected there is never seen for a
        // parked window, and the close grace that depends on it never arms.
        std::chrono::steady_clock::time_point s_menuWindowSeenTp{};
        float s_snapWindowT  = -1.0f;      // >=0 while a menu window is open
        float s_snapPeak     = 0.0f;       // biggest dCam seen in this window
        int   s_snapPost     = 0;          // frames still to print after a trigger
        bool  s_snapTripped  = false;      // this window already dumped its pre-roll
        int   s_snapDumps    = 0;          // whole-session dump budget
        constexpr int kSnapMaxDumps = 40;   // raised for the 2026-09-06 spam capture

        // ================= [TWEENX] ==========================================
//
        // THE QUESTION EVERY EARLIER PROBE LEFT OPEN.
        //
        // [SNAP] samples ONCE per frame, after the whole pipeline has run, so
        // it can say the camera moved 4.85u and which field holds the gap
        // (ThirdPersonState::translation, named 2026-09-05) but NOT who put it
        // there. translation is already pinned in ActivateForMenu and the pop
        // survives, which means the engine RECOMPOSES it after the pin — so
        // the thing to measure is the engine's own pass itself.
        //
        // This straddles CallOriginalUpdate: every field the third-person
        // update composes FROM and the one it composes TO, captured
        // immediately before and immediately after the original runs, on the
        // same line. `dTrans` is then the ENGINE'S OWN contribution to the
        // lateral position for that frame.
        //
        // VERDICT RULE, decided before the capture so the log cannot be read
        // to taste:
        //   - dTrans ~0 on steady frames and ~+4.8 on the open frame => the
        //     engine composed the jump. The IN columns beside it say which
        //     input differed from the steady frames, and THAT is the field to
        //     pin. rotYaw is the standing suspect (Begin resets it; 1.75 deg
        //     at 133u zoom = 4.08u) and the pin for it shipped UNTESTED.
        //   - in.transR ALREADY carries the +4.8 on the open frame => nobody
        //     needs to recompose anything; something wrote translation before
        //     the engine ran, and the only writers are ActivateForMenu's own
        //     restore, the Looseness block and the Stairs block.
        //   - dTrans ~0 AND in.transR steady on the open frame, yet [SNAP]
        //     still reports the jump => the move happens after this pass
        //     entirely, i.e. downstream of the 3p update, and the next probe
        //     is the render matrix rather than ThirdPersonState.
        //
        // Ring of pre-frames so the steady baseline is on screen next to the
        // open frame; flushed on the menu edge, then live for a few frames.
        struct TweenXRec {
            float inTransR = 0.0f, outTransR = 0.0f;
            float inRotYaw = 0.0f, outRotYaw = 0.0f;
            float inSide   = 0.0f, outSide   = 0.0f;
            // posOffsetEXPECTED — the one composition input never sampled at
            // the engine's own pass. `side` above is posOffsetACTUAL, and it
            // reads 30.0 in and out on every frame, menu or not, so the pose
            // is not moving through it. DDC re-asserts BOTH triples at
            // CameraController::Update, which runs ~860 lines further down
            // this same hook, so anything the engine writes to expected
            // during its pass is erased before any other probe in the build
            // can see it. These two columns are inside that window.
            float inExpX   = 0.0f, outExpX   = 0.0f;
            float inExpZ   = 0.0f, outExpZ   = 0.0f;
            // THE GEOMETRY, RESOLVED INTO THE CAMERA'S OWN FRAME.
            //
            // 2026-09-07: the walk is 4.98u along the camera's RIGHT axis at
            // constant height — a five-unit shoulder offset, the size of the
            // vanilla fOverShoulderPosX(30)/fOverShoulderCombatPosX(25) delta.
            // `shoulder` is (camera - player) projected on the camera's own
            // horizontal right axis, i.e. the SAME quantity posOffsetActual.x
            // is supposed to set. Verdict rule: shoulder reads ~30.0 on
            // gameplay frames before any menu and walks to ~25.0 while the
            // menu is up => the engine is composing from the combat shoulder
            // and the fix is at whatever selects it. `back` is the matching
            // forward projection, so a distance change cannot masquerade as a
            // lateral one.
            float inShoulder = 0.0f, outShoulder = 0.0f;
            float inBack     = 0.0f, outBack     = 0.0f;
            // [TWEENU] — EVERY FIELD OF ThirdPersonState NOBODY HAS READ.
            //
            // The shoulder walk has to be composed from something, and every
            // NAMED input is byte-identical across it (posOffset both triples,
            // both zoom pairs, freeRotation, the rotation quaternion, collision
            // unset, the yaw pair). What is left in the 0xE8 struct is the
            // camera-target pointer the pose is composed AROUND and five
            // unnamed slots. One line per frame, in and out of the engine's
            // pass: whichever of these walks 0.14/frame with the pose, or sits
            // at a constant 5.0 offset, is the carrier.
            struct Unk {
                float    camObjR = 0.0f;      // thirdPersonCameraObj, lateral vs player
                bool     haveCamObj = false;
                float    u8C  = 0.0f;
                float    a0a  = 0.0f, a0b = 0.0f;
                float    c0a  = 0.0f, c0b = 0.0f;
                float    c8a  = 0.0f, c8b = 0.0f;
                uint32_t d0   = 0;
                float    animYaw = 0.0f;
                int      flags = 0;           // applyOffsets|animCam|notActive|freeRot
            };
            Unk inU{}, outU{};
            float inZoom   = 0.0f, outZoom   = 0.0f;
            float inFrX    = 0.0f, outFrX    = 0.0f;
            float inCollR  = 0.0f, outCollR  = 0.0f;
            float inRootR  = 0.0f, outRootR  = 0.0f;
            float bodyYaw  = 0.0f;
            float end3pRoot = 0.0f, end3pTrans = 0.0f;   // previous frame's tail
            float ucpRoot   = 0.0f, ucpTrans   = 0.0f;   // previous frame's post-hook head
            float engRoot   = 0.0f, engTrans   = 0.0f;   // previous frame's post-engine
            int   camState = -1;
            bool  menuOpen = false;
            bool  valid    = false;
        };
        constexpr int kTweenXRing = 5;
        TweenXRec s_tweenXRing[kTweenXRing]{};
        int       s_tweenXHead    = 0;
        int       s_tweenXPost    = 0;    // frames still to print live
        int       s_tweenXWindows = 0;    // whole-session window budget
        int       s_tweenXFrame   = 0;    // frame index within the window
        constexpr int kTweenXMaxWindows = 8;

        // ===== 2026-09-07: WHAT THE 14:40 CAPTURE SETTLED ====================
        //
        // Seven spammed Tween opens, standing still, every composition input
        // printed. Read it as a sequence, not as single frames:
        //
        //   pre-menu gameplay   lat 41.63, flat for as long as it is watched
        //   menu up             lat walks DOWN at a constant 0.14/frame ...
        //   ... 35 frames later lat 36.65 and it STOPS DEAD, still in the menu
        //   menu closed         lat 36.65 forever — gameplay never corrects it
        //   next open           lat 41.49 in ONE frame, then walks again
        //
        // Three things follow, and each kills a theory this file has carried:
        //
        //  1. IT IS A TARGET, NOT A RUNAWAY. The walk stops on arrival and
        //     holds for the rest of the window. So the drift is a rate-limited
        //     approach to a fixed pose 4.98u away, not an integrator, and the
        //     "engine integrates the root/translation gap at 0.36x" model from
        //     2026-09-06 is dead: on window 2 the root sat 0.11u BELOW
        //     translation and the walk kept both its sign and its rate.
        //     The noise gate that model shipped was reverted with this commit.
        //
        //  2. THE SNAP IS AT THE OPEN, BUT THE DEFECT IS AT THE CLOSE. 41.49 is
        //     the pose the engine composes from DDC's own settings — every open
        //     lands on it to the hundredth. Gameplay after a menu is the frame
        //     that is WRONG, sitting 4.98u off with nothing to correct it,
        //     because nothing recomposes ThirdPersonState::translation during
        //     play (dTrans=+0.00 on every gameplay frame in this capture).
        //
        //  3. THE MOVE IS FIVE UNITS OF SHOULDER. Decomposed: dr=+4.84,
        //     df=+0.43, du=0.00 — a move of 4.86u whose direction is 5.08 deg
        //     off body-right, against a camera/body yaw split of 5.05 deg. So
        //     it is exactly along the CAMERA'S right axis, at constant height,
        //     and (taking the pre-menu 41.63 rather than the already-stepped
        //     open frame) exactly 4.98u long. Independent check: the horizontal
        //     player-camera distance goes 136.82 -> 135.84, and a 30 -> 25
        //     shoulder at this zoom predicts 135.81. Two quantities, one free
        //     parameter, both land. fOverShoulderPosX(30) - CombatPosX(25) is
        //     5.00, and the 2026-08-20 dump ("written 0.2 -> rendered -4.80")
        //     says the term is ADDITIVE, not a jump to the value 25.
        //
        // WHICH IS WHY THIS PROBE GREW TWO COLUMNS. `SHOULDER` is the rendered
        // pose resolved onto the camera's own right axis — the same quantity
        // posOffsetActual.x sets — so the walk can be read in shoulder units
        // instead of world lateral, and `expX/expZ` is the last composition
        // input nobody has sampled at the engine's pass. Verdict rule, fixed
        // before the capture:
        //   - SHOULDER 30 -> 25 while actX stays 30.0 => the engine composes
        //     from something other than posOffsetActual, and expX says whether
        //     that something is posOffsetExpected. If expX reads 25 (or the
        //     combat value) at the pass and 30 afterwards, the writer is named
        //     and the fix is to stop the menu session selecting the combat
        //     shoulder — NOT another pin.
        //   - SHOULDER flat at 30 while lat walks => the move is not a
        //     shoulder change at all, the arithmetic above is a coincidence,
        //     and this whole reading is to be dropped rather than tuned.
        //   - `back` moving with `shoulder` => a zoom/pivot change wearing a
        //     lateral disguise; re-read from there.
        //
        // ===== 2026-09-06, SUPERSEDED — kept for the measurement only =======
        // THE 5.0 IS BETWEEN TWO PROBES, NOT INSIDE EITHER (2026-09-06 capture).
        // [TWEENX] reads tps->translation at the engine's own pass and sees
        // 41.63; [SNAP] reads the SAME FIELD later in the frame and sees
        // 36.65. 41.63 - 36.65 = 4.98 - the whole snap. On the Tween-open
        // frame both read 41.49, i.e. the mover simply does not run. So the
        // question is no longer "which field" but "which INTERVAL", and
        // these two extra samples bracket the three candidates:
        //   endOf3p  - end of HookedThirdPersonUpdate, after the original AND
        //              after everything this hook does on top of it.
        //   ucpIn    - top of HookedUpdateCameraPost, before SnapTick.
        // Reading, with out=41.63 at the engine's pass and SNAP=36.65:
        //   endOf3p ~36.6 => the mover is inside the 3p hook, after the
        //                    original. Bisect this hook next.
        //   endOf3p ~41.6 and ucpIn ~36.6 => it runs between the two hooks,
        //                    i.e. in the engine's own later camera pass or
        //                    another DDC hook entirely.
        //   ucpIn ~41.6 => it runs inside UpdateCameraPost above SnapTick.
        float s_tweenXEnd3pRoot   = 0.0f;   // published by the 3p hook tail
        float s_tweenXEnd3pTrans  = 0.0f;
        float s_tweenXUcpRoot     = 0.0f;   // published by UpdateCameraPost head
        float s_tweenXUcpTrans    = 0.0f;
        // Bracket C - immediately AFTER _originalUpdateCameraPost returns.
        // Brackets A and B put the ~5u between the top of UpdateCameraPost and
        // SnapTick, and nothing in OUR hook body in that span touches the
        // camera - the only thing in it is the engine's own continuation. This
        // column proves or refutes that in one capture: steady frames reading
        // ~36.6 here (against ~41.6 at bracket B) mean the ENGINE applies the
        // lateral every frame and simply skips it on the activation frame, and
        // the fix belongs at the render level rather than in ThirdPersonState.
        // Still ~41.6 here would mean it happens after this point, i.e. in our
        // own hook body after all, and the next step is bisecting that.
        float s_tweenXEngRoot     = 0.0f;
        float s_tweenXEngTrans    = 0.0f;

        // [TWEENR] — RAW COMPONENTS AT TWO SAMPLE POINTS.
        //
        // Bracket C reads cameraRoot->world.translate as 41.63 and SnapTick
        // reads THE SAME FIELD as 36.65, in the same function, ~150 lines
        // apart, with nothing in between that writes it (checked line by line:
        // FOV holds, posOffset writes, ReleaseLeakedLockYaw, indoorMode,
        // LocationDetector). One of the two readings is therefore not built
        // from what it claims. Both derive a LATERAL from three inputs —
        // camera world xy, player world xy, and the player's body yaw — so
        // this prints all three raw at both points and lets a direct diff say
        // which one moved. Note the camera sits ~131u behind the player, so a
        // yaw difference of only 2.2 deg reproduces the whole 4.98u by itself.
        // [TWEENR] Looseness-block terms. In 3p gameplay this block is the
        // ONLY per-frame writer of the four fields that move in lockstep
        // (cameraNI world, cameraRoot world AND local, ThirdPersonState::
        // translation) - the stairs block is z-only, the FP pose block is
        // first person, and SPIM's ApplyRenderMatrix is gated off in this
        // user's camera-control mode. Every composition INPUT is frozen
        // across the drift (19:38 trace: actX/expX/actZ/expZ/zC/zT/frX/frY/
        // rotYaw/quaternion/cam3 all byte-identical while lat moves 5u), so
        // the pose is not being composed - it is being written. This prints
        // what that writer saw and what it wrote.
        //
        // Verdict rule: `lag` non-zero with the player standing still means
        // the follow-lag tracker is the source and the fix is in it. `lag`
        // ~0 with eng already moving means this block is a pass-through and
        // the mover is upstream, in the engine's own pass - which is where
        // [TWEENX]'s dTrans=-0.14 points, and then the two measurements
        // finally agree instead of contradicting.
        float s_tweenLooseEngLat  = 0.0f;
        float s_tweenLooseRendLat = 0.0f;
        float s_tweenLooseLagLat  = 0.0f;
        float s_tweenLooseFade    = 0.0f;
        bool  s_tweenLooseRan     = false;
        float s_snapLastRootR = 0.0f;   // published by SnapTick for [TWEENR]
        float s_snapLastR     = 0.0f;
        float s_snapLastF     = 0.0f;
        // [TWEENR] only starts once a Tween open has happened, so its lines
        // land in the SAME frames as the [SNAP] dump and can be compared
        // timestamp for timestamp. The 18:32 capture proved C and the
        // pre-SnapTick sample identical (both 41.63) but had no [SNAP] output
        // to compare against, because [SNAP] only prints on a JUMP trigger.
        // Armed from the START now, one line per frame. The 18:51 capture
        // settled the earlier questions: POST and SnapTick's own rootR agree
        // to the digit on every line, so SnapTick is correct and there is no
        // probe bug; and C == POST, so nothing inside UpdateCameraPost moves
        // the camera. What it could NOT show is the steady state, because it
        // armed on the open and only ever saw the walk-back
        // (41.49 -> 41.35 -> 41.21 -> 41.06 -> 40.92, the 0.14/frame return).
        //
        // The picture that leaves is the REVERSE of how this bug has been read
        // for six weeks. A fresh load sits at 41.63 and does NOT drift
        // (18:32: forty frames, constant). After a Tween open the camera sits
        // at 41.49 and walks DOWN at 0.14/frame. 41.49 - 36.65 = 4.84, which
        // at 0.14/frame is ~35 frames - so the '36.65 steady' every earlier
        // capture reported is not a resting state at all, it is where the walk
        // ENDS. The open does not drop a 5u layer; it RESETS an accumulated
        // one, and the walk re-accumulates it.
        //
        // So the question is now: what is the steady value before any menu has
        // been opened, what does the walk converge to, and does it re-drift
        // after each open. One continuous trace answers all three.
        // ARMED BY THE TWEEN OPEN, not at load. The 19:48 run spent all 400
        // frames on pre-menu steady state and caught none of the seven opens
        // [SNAP] logged later - the budget has to be spent where the event
        // is. Ring of 10 steady frames flushed on the rising edge, then 70
        // live frames, which covers the open, the whole ~37-frame drift and
        // the close. Lines are pre-formatted into the ring so the record
        // stays one string instead of twenty fields.
        std::string s_tweenRRing[10];
        int  s_tweenRHead    = 0;
        int  s_tweenRPost    = 0;
        int  s_tweenRWindows = 0;
        constexpr int kTweenRMaxWindows = 4;
        int s_tweenRLines = 0;
        constexpr int kTweenRMaxLines = 400;
        void TweenRawPrint(const char* a_tag)
        {
            (void)a_tag;
            if (s_tweenRLines >= kTweenRMaxLines) return;
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (!cam || !cam->cameraRoot || !ply || !cam->currentState) return;
            if (cam->currentState.get() !=
                cam->cameraStates[RE::CameraState::kThirdPerson].get()) return;
            // NOTE: the budget is spent per PRINTED line, in the emit branches
            // below - not per call. Counting per call here is exactly what
            // burned all 400 frames on pre-menu steady state twice (19:48 and
            // 19:53 both caught zero of the seven opens).
            const RE::NiPoint3 rootW = cam->cameraRoot->world.translate;
            RE::NiPoint3       niW   = rootW;
            bool haveNi = false;
            if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                    niW = ni->world.translate; haveNi = true;
                }
            }
            const RE::NiPoint3 pp   = ply->GetPosition();
            const float        byaw = ply->data.angle.z;
            const float sy = std::sin(byaw), cy = std::cos(byaw);
            const float lat = (rootW.x - pp.x) * cy - (rootW.y - pp.y) * sy;

            // EVERY INPUT THE ENGINE COMPOSES THE 3P POSE FROM.
            //
            // The drift is real and it is the engine's own pass doing it
            // ([TWEENX] dTrans=-0.14/frame while the menu is up, +0.00 in
            // gameplay), but every input printed so far - posOffsetActual.x,
            // currentZoomOffset, freeRotation.x, the rotation YAW - is
            // constant across the whole drift. A composition cannot move with
            // all its inputs still, so the one that moves is one nobody has
            // printed yet. This prints the rest of them: the Expected half of
            // both offset pairs (the engine lerps Actual toward Expected), the
            // zoom TARGET, the pitch channel, the quaternion's non-yaw
            // components, and the Camera3rd pivot in BOTH axes - cam3's
            // lateral has been logged as 0.00 all along, but nothing has ever
            // looked at its forward component, and the camera orbits the
            // player at a constant ~136u radius, which is a pivot move.
            auto* tpsIn = static_cast<RE::ThirdPersonState*>(cam->currentState.get());
            const auto& q = tpsIn->rotation;
            const float rotYaw = std::atan2(2.0f * (q.x * q.y - q.w * q.z),
                                            1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.2957795f;
            float cam3Lat = 0.0f, cam3Fwd = 0.0f; bool haveCam3 = false;
            if (auto* root3d = ply->Get3D(false)) {
                if (auto* c3 = root3d->GetObjectByName("Camera3rd [Cam3]")) {
                    const auto& w = c3->world.translate;
                    cam3Lat = (w.x - pp.x) * cy - (w.y - pp.y) * sy;
                    cam3Fwd = (w.x - pp.x) * sy + (w.y - pp.y) * cy;
                    haveCam3 = true;
                }
            }
            auto* uiR = RE::UI::GetSingleton();
            const bool tweenNow = uiR && uiR->IsMenuOpen("TweenMenu");
            const bool spimNow  = ShowPlayerInMenusController::GetSingleton().IsActive();
            char buf[640];
            std::snprintf(buf, sizeof(buf),
                "menu=%d spim=%d lat=%.2f | actX=%.2f expX=%.2f actZ=%.2f expZ=%.2f "
                "zC=%.3f zT=%.3f frX=%.4f frY=%.4f rotYaw=%.3f q=(%.4f,%.4f,%.4f,%.4f) "
                "cam3=(%.2f,%.2f)%s byaw=%.4f || loose ran=%d eng=%.2f rend=%.2f lag=%.2f fade=%.2f",
                tweenNow ? 1 : 0, spimNow ? 1 : 0, lat,
                tpsIn->posOffsetActual.x, tpsIn->posOffsetExpected.x,
                tpsIn->posOffsetActual.z, tpsIn->posOffsetExpected.z,
                tpsIn->currentZoomOffset, tpsIn->targetZoomOffset,
                tpsIn->freeRotation.x, tpsIn->freeRotation.y,
                rotYaw, q.w, q.x, q.y, q.z,
                cam3Lat, cam3Fwd, haveCam3 ? "" : "(none)", byaw,
                s_tweenLooseRan ? 1 : 0, s_tweenLooseEngLat, s_tweenLooseRendLat,
                s_tweenLooseLagLat, s_tweenLooseFade);
            s_tweenLooseRan = false;

            static bool sTweenRPrevOpen = false;
            const bool  rising = tweenNow && !sTweenRPrevOpen;
            sTweenRPrevOpen = tweenNow;

            if (s_tweenRPost > 0) {
                ++s_tweenRLines;
                --s_tweenRPost;
                spdlog::debug("[TWEENR] live {}", buf);
            } else if (rising && s_tweenRWindows < kTweenRMaxWindows) {
                ++s_tweenRWindows;
                spdlog::debug("[TWEENR] ==== TweenMenu OPEN (window {}/{}) ====",
                             s_tweenRWindows, kTweenRMaxWindows);
                for (int i = 0; i < 10; ++i) {
                    const auto& pr = s_tweenRRing[(s_tweenRHead + i) % 10];
                    if (!pr.empty()) {
                        ++s_tweenRLines;
                        spdlog::debug("[TWEENR] pre  {}", pr);
                    }
                }
                ++s_tweenRLines;
                spdlog::debug("[TWEENR] OPEN {}", buf);
                s_tweenRPost = 70;
            } else {
                s_tweenRRing[s_tweenRHead] = buf;
                s_tweenRHead = (s_tweenRHead + 1) % 10;
            }
        }

        // Fill the [TWEENU] unnamed-field snapshot. Declared as a template so
        // it can take the record's nested struct without hoisting the type.
        template <class UnkT>
        void TweenXFillUnk(RE::ThirdPersonState* a_tps, const RE::NiPoint3& a_pp,
                           float a_cy, float a_sy, UnkT& a_u)
        {
            if (!a_tps) return;
            if (a_tps->thirdPersonCameraObj) {
                const auto& w = a_tps->thirdPersonCameraObj->world.translate;
                a_u.camObjR   = (w.x - a_pp.x) * a_cy - (w.y - a_pp.y) * a_sy;
                a_u.haveCamObj = true;
            }
            a_u.u8C = a_tps->unk8C;
            const auto split = [](std::uint64_t v, float& lo, float& hi) {
                const std::uint32_t a = static_cast<std::uint32_t>(v & 0xFFFFFFFFull);
                const std::uint32_t b = static_cast<std::uint32_t>(v >> 32);
                std::memcpy(&lo, &a, sizeof(float));
                std::memcpy(&hi, &b, sizeof(float));
            };
            split(a_tps->unkA0, a_u.a0a, a_u.a0b);
            split(a_tps->unkC0, a_u.c0a, a_u.c0b);
            split(a_tps->unkC8, a_u.c8a, a_u.c8b);
            a_u.d0 = a_tps->unkD0;
            const auto& q = a_tps->animationRotation;
            a_u.animYaw = std::atan2(2.0f * (q.x * q.y - q.w * q.z),
                                     1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.2957795f;
            a_u.flags = (a_tps->applyOffsets ? 1 : 0) | (a_tps->toggleAnimCam ? 2 : 0) |
                        (a_tps->stateNotActive ? 4 : 0) | (a_tps->freeRotationEnabled ? 8 : 0);
        }

        // (camera - player) resolved in the CAMERA'S OWN horizontal frame.
        //
        // a_shoulder is the projection on the camera's right axis — the same
        // quantity ThirdPersonState::posOffsetActual.x is supposed to set, so
        // the two are directly comparable on one line. a_back is the forward
        // projection (negative = behind the player), which is what separates
        // a shoulder change from a zoom change: a shoulder move leaves `back`
        // alone, a zoom move leaves `shoulder` alone.
        //
        // The right axis is derived from the camera's forward column (col 0 of
        // the NiCamera world rotation, the same column the Flee Framing block
        // reads at ~9330) rotated 90 deg in the horizontal plane, rather than
        // guessing which of the other two columns is "right". If the sign
        // comes back negative the reading is unaffected — what matters is the
        // 30 -> 25 walk, not which way the axis points.
        void TweenXShoulder(RE::PlayerCamera* a_cam, const RE::NiPoint3& a_playerPos,
                            float& a_shoulder, float& a_back)
        {
            a_shoulder = 0.0f;
            a_back     = 0.0f;
            if (!a_cam || !a_cam->cameraRoot) return;
            RE::NiAVObject* src = a_cam->cameraRoot.get();
            if (auto* nd = a_cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) src = ni;
            }
            const float fx = src->world.rotate.entry[0][0];
            const float fy = src->world.rotate.entry[1][0];
            const float fl = std::sqrt(fx * fx + fy * fy);
            if (fl < 1.0e-4f) return;
            const float ux = fx / fl, uy = fy / fl;      // forward, horizontal
            const float rx = uy,      ry = -ux;          // right, horizontal
            const RE::NiPoint3 cw = src->world.translate;
            const float dx = cw.x - a_playerPos.x;
            const float dy = cw.y - a_playerPos.y;
            a_shoulder = dx * rx + dy * ry;
            a_back     = dx * ux + dy * uy;
        }

        // Lateral projection shared by every TWEENX sample point, so the
        // numbers are directly comparable to [SNAP]'s columns.
        bool TweenXSampleLat(float& a_outRoot, float& a_outTrans)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (!cam || !cam->cameraRoot || !ply || !cam->currentState) return false;
            if (cam->currentState.get() !=
                cam->cameraStates[RE::CameraState::kThirdPerson].get()) return false;
            const RE::NiPoint3 pp   = ply->GetPosition();
            const float        byaw = ply->data.angle.z;
            const float sy = std::sin(byaw), cy = std::cos(byaw);
            const auto  latOf = [&](const RE::NiPoint3& w) {
                return (w.x - pp.x) * cy - (w.y - pp.y) * sy;
            };
            a_outRoot  = latOf(cam->cameraRoot->world.translate);
            a_outTrans = latOf(
                static_cast<RE::ThirdPersonState*>(cam->currentState.get())->translation);
            return true;
        }

        void TweenXPrint(const char* a_tag, int a_frame, const TweenXRec& r)
        {
            spdlog::debug("[TWEENX] {} f={:+d} menu={} cam={} | transR {:.2f}->{:.2f} "
                         "dTrans={:+.2f} | SHOULDER {:.2f}->{:.2f} back {:.2f}->{:.2f} | "
                         "actX {:.1f}->{:.1f} expX {:.1f}->{:.1f} expZ {:.1f}->{:.1f} | "
                         "rotYaw {:.2f}->{:.2f} dYaw={:+.2f} | "
                         "zoom {:.1f}->{:.1f} | frX {:.3f}->{:.3f} | "
                         "collR {:.1f}->{:.1f} | rootR {:.2f}->{:.2f} | bodyYaw={:.2f} "
                         "|| prevEnd3p root={:.2f} trans={:.2f} | prevUcp root={:.2f} trans={:.2f} "
                         "| prevEngPost root={:.2f} trans={:.2f}",
                         a_tag, a_frame, r.menuOpen ? 1 : 0, r.camState,
                         r.inTransR, r.outTransR, r.outTransR - r.inTransR,
                         r.inShoulder, r.outShoulder, r.inBack, r.outBack,
                         r.inSide, r.outSide, r.inExpX, r.outExpX, r.inExpZ, r.outExpZ,
                         r.inRotYaw, r.outRotYaw, r.outRotYaw - r.inRotYaw,
                         r.inZoom, r.outZoom,
                         r.inFrX, r.outFrX, r.inCollR, r.outCollR,
                         r.inRootR, r.outRootR, r.bodyYaw,
                         r.end3pRoot, r.end3pTrans, r.ucpRoot, r.ucpTrans,
                         r.engRoot, r.engTrans);

            // The unnamed half of the struct, same frame, same brackets.
            spdlog::debug("[TWEENU] {} f={:+d} | camObj r={:.2f}->{:.2f}{} | u8C {:.3f}->{:.3f} "
                         "| A0 ({:.3f},{:.3f})->({:.3f},{:.3f}) "
                         "| C0 ({:.3f},{:.3f})->({:.3f},{:.3f}) "
                         "| C8 ({:.3f},{:.3f})->({:.3f},{:.3f}) "
                         "| D0 {}->{} | animYaw {:.2f}->{:.2f} | flags {}->{}",
                         a_tag, a_frame,
                         r.inU.camObjR, r.outU.camObjR,
                         (r.inU.haveCamObj && r.outU.haveCamObj) ? "" : "(none)",
                         r.inU.u8C, r.outU.u8C,
                         r.inU.a0a, r.inU.a0b, r.outU.a0a, r.outU.a0b,
                         r.inU.c0a, r.inU.c0b, r.outU.c0a, r.outU.c0b,
                         r.inU.c8a, r.inU.c8b, r.outU.c8a, r.outU.c8b,
                         r.inU.d0, r.outU.d0,
                         r.inU.animYaw, r.outU.animYaw,
                         r.inU.flags, r.outU.flags);
        }

        void SnapPrintFrame(const char* a_tag, const SnapFrame& s)
        {
            // rootR/niR/haveNi and shortfall are the 2026-08-20 additions: if
            // `shortfall` (side - rendered lateral) is a steady 5.0 and goes to
            // 0 exactly when haveNi flips, the jump is this probe changing which
            // node it reads. If haveNi never changes, the camera really moved
            // and animCam/drawn say whether the engine was applying the offset
            // in full.
            spdlog::debug("[SNAP]   {} t={:+.3f} cam=(r{:.2f},f{:.2f},u{:.2f}) dCam={:.2f} "
                         "dPly={:.2f} ask=(side{:.1f},h{:.1f}) zoom={:.3f}/{:.3f} "
                         "freeRotX={:.3f} camState={} spim={} | root_r={:.2f} ni_r={:.2f} "
                         "haveNi={} shortfall={:+.2f} animCam={} drawn={} preX={:.2f} "
                         "noise_r={:+.2f}{} eng_r={:+.2f} loose_r={:+.2f}{} | "
                         "cam3_r={:+.2f}{} head_r={:+.2f}{} trans_r={:+.2f} coll_r={:+.2f}{} rootLocal_r={:+.2f} | "
                         "rotYaw={:.2f} curYaw={:.2f} tgtYaw={:.2f} bodyYaw={:.2f} camYaw={:.2f}",
                         a_tag, s.t, s.r, s.f, s.u, s.dCam, s.dPlayer,
                         s.side, s.height, s.zoomCur, s.zoomTgt, s.freeRotX,
                         s.camState, s.spim ? 1 : 0,
                         s.rootR, s.niR, s.haveNi ? 1 : 0,
                         s.side - s.r, s.animCam ? 1 : 0, s.drawn ? 1 : 0, s.preX,
                         s.n3pR, s.n3pAny ? "" : "(none)",
                         s.engR, s.looseR, s.looseValid ? "" : "(off)",
                         s.cam3R, s.haveCam3 ? "" : "(none)",
                         s.headR, s.haveHead ? "" : "(none)",
                         s.transR, s.collR, s.haveTps ? "" : "(no tps)",
                         s.rootLocalR,
                         s.rotYaw, s.curYaw, s.tgtYaw, s.bodyYaw, s.camYaw);
        }

        // ---- [PITCHX] â€” does HEIGHT move the AIM? --------------------------
        //
        // User report 2026-08-20: "our pitch implementation is messed up, it's
        // fighting against getting a good angle set upâ€¦ I shouldn't have to
        // touch pitch in order to make the camera look straight ahead." Their
        // [weapons.melee] is height -35 WITH pitch_offset 20, while every other
        // entry in the same preset sits at height -15 with no pitch at all â€” so
        // dropping the camera 20 units further cost them 20 units of pitch to
        // undo.
        //
        // DDC's own pitch path cannot explain that: ApplyPitchOffset rotates the
        // camera quaternion about its local X and touches nothing else, and
        // effectiveHeight goes to posOffsetActual/Expected.z and touches no
        // rotation. So either the ENGINE's compose couples the two (it rotates
        // posOffset by the player's pitch â€” DDC already cancels that inside
        // dialogue via compY/compZ, and nowhere else), or the coupling is
        // perceptual and there is nothing to fix.
        //
        // That is one measurement, not an argument: sample the camera's aim and
        // the height channel together, and print only while HEIGHT IS MOVING
        // (i.e. while the user drags the slider). If the aim numbers move with
        // height, the channels are coupled and the dialogue fix generalises. If
        // they don't, the fight is somewhere else and this probe says so.
        int   s_pxLines = 0;
        int   s_pxBurst = 0;
        float s_pxPrevZ = 0.0f;
        bool  s_pxPrimed = false;
        constexpr int kPxMaxLines = 240;

        void PitchXTick()
        {
            if (s_pxLines >= kPxMaxLines) return;
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (!cam || !ply || !cam->currentState || !cam->cameraRoot) return;
            if (cam->currentState.get() != cam->cameraStates[RE::CameraState::kThirdPerson].get())
                return;
            auto* tps = static_cast<RE::ThirdPersonState*>(cam->currentState.get());

            const float z = tps->posOffsetActual.z;
            if (!s_pxPrimed) { s_pxPrevZ = z; s_pxPrimed = true; }
            // Arm on a real height move (slider drag or a state switch), then
            // keep printing for half a second so the settle is visible too.
            if (std::abs(z - s_pxPrevZ) > 0.35f) s_pxBurst = 30;
            s_pxPrevZ = z;
            if (s_pxBurst <= 0) return;
            --s_pxBurst;
            ++s_pxLines;

            // Aim, read three ways so no single convention can mislead:
            //   bodyX/frY  â€” the two pitch CHANNELS (rendered = bodyX - frY)
            //   pitchOff   â€” what DDC's own Pitch Offset is adding this frame
            //   fwd        â€” the rendered camera's forward vector, straight off
            //                the NiCamera world matrix. fwd.z IS the aim: 0 is
            //                dead level, and it is convention-free.
            float fwdX = 0.0f, fwdY = 0.0f, fwdZ = 0.0f;
            RE::NiPoint3 cw = cam->cameraRoot->world.translate;
            if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                    cw = ni->world.translate;
                    // Column 0 is the camera FORWARD â€” same extraction the
                    // Flee Framing approach test uses, which is the one place
                    // in this file that is proven against real behaviour. Do
                    // not "fix" this to column 1 without re-deriving it there
                    // too; the axis conventions on this node have burned this
                    // project before (see the Update3DPosition Euler-order
                    // note in the 2026-08-14 session).
                    fwdX = ni->world.rotate.entry[0][0];
                    fwdY = ni->world.rotate.entry[1][0];
                    fwdZ = ni->world.rotate.entry[2][0];
                }
            }
            const RE::NiPoint3 pp = ply->GetPosition();
            const float byaw = ply->data.angle.z;
            const float sy = std::sin(byaw), cy = std::cos(byaw);
            const float ex = cw.x - pp.x, ey = cw.y - pp.y;

            spdlog::debug("[PITCHX] posAct=({:.1f},{:.1f},{:.1f}) posExp=({:.1f},{:.1f},{:.1f}) "
                         "zoom={:.3f} | bodyX={:+.4f} frY={:+.4f} rendered={:+.4f} "
                         "pitchOff={:.2f} | fwd=({:+.3f},{:+.3f},{:+.3f}) "
                         "cam=(r{:.1f},f{:.1f},u{:.1f}) | line {}/{}",
                         tps->posOffsetActual.x, tps->posOffsetActual.y, tps->posOffsetActual.z,
                         tps->posOffsetExpected.x, tps->posOffsetExpected.y, tps->posOffsetExpected.z,
                         tps->currentZoomOffset,
                         ply->data.angle.x, tps->freeRotation.y,
                         ply->data.angle.x - tps->freeRotation.y,
                         CameraController::GetSingleton().LivePitchOffset(),
                         fwdX, fwdY, fwdZ,
                         ex * cy - ey * sy, ex * sy + ey * cy, cw.z - pp.z,
                         s_pxLines, kPxMaxLines);
        }

        // One sample per frame, from the universal camera hook so it runs in
        // EVERY camera state (the old probe lived in the 3p hook, which does
        // not tick while some menus are up â€” precisely the frames in question).
        void SnapTick(float a_dt)
        {
            if (!spdlog::should_log(spdlog::level::debug)) {
                s_snapPrimed = false;
                s_snapWindowT = -1.0f;
                s_snapPost = 0;
                return;
            }
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* ply = RE::PlayerCharacter::GetSingleton();
            if (!cam || !cam->cameraRoot || !ply) { s_snapPrimed = false; return; }

            // Same read as [MENUCAM]: root translate, overridden by the player
            // NiCamera child when present, expressed in the player-yaw frame so
            // ordinary following motion cancels out.
            const RE::NiPoint3 rootW = cam->cameraRoot->world.translate;
            RE::NiPoint3 cw     = rootW;
            bool         haveNi = false;
            if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
                if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                    cw     = ni->world.translate;
                    haveNi = true;
                }
            }
            const RE::NiPoint3 pp   = ply->GetPosition();
            const float        byaw = ply->data.angle.z;
            const float sy = std::sin(byaw), cy = std::cos(byaw);
            const float ex = cw.x - pp.x, ey = cw.y - pp.y;

            SnapFrame s;
            s.r = ex * cy - ey * sy;
            s.f = ex * sy + ey * cy;
            s.u = cw.z - pp.z;
            // Both readings, every frame, so a node swap is visible as a step in
            // ONE of them rather than a mystery step in the combined number.
            s.haveNi = haveNi;
            s.rootR  = (rootW.x - pp.x) * cy - (rootW.y - pp.y) * sy;
            s.niR    = s.r;

            static RE::NiPoint3 sPrevPly{};
            static bool         sPrevPlyOk = false;
            if (sPrevPlyOk) {
                const float pdx = pp.x - sPrevPly.x, pdy = pp.y - sPrevPly.y;
                s.dPlayer = std::sqrt(pdx * pdx + pdy * pdy);
            }
            sPrevPly   = pp;
            sPrevPlyOk = true;

            // "Menu-ish" rather than strictly SPIM-active: a menu whose
            // customization toggle is OFF never activates the framing
            // controller, and the user's snap would then fall outside every
            // window. Game-paused-by-menu costs two lines per journal/console
            // open and buys coverage of every menu that can move the camera.
            const auto& spim = ShowPlayerInMenusController::GetSingleton();
            auto*       uiS  = RE::UI::GetSingleton();
            s.spim     = spim.IsActive() || (uiS && uiS->GameIsPaused());
            s.camState = cam->currentState ? static_cast<int>(cam->currentState->id) : -1;
            if (cam->currentState &&
                cam->currentState.get() == cam->cameraStates[RE::CameraState::kThirdPerson].get()) {
                auto* tps = static_cast<RE::ThirdPersonState*>(cam->currentState.get());
                s.side     = tps->posOffsetActual.x;
                s.height   = tps->posOffsetActual.z;
                s.zoomCur  = tps->currentZoomOffset;
                s.zoomTgt  = tps->targetZoomOffset;
                s.freeRotX = tps->freeRotation.x;
                s.animCam  = tps->toggleAnimCam;
            }
            s.preX = s_snapPreUpdSideX;
            {
                // The engine's own terms (2026-09-05), all lateral in the
                // player-yaw frame relative to the player root.
                const auto latOf = [&](const RE::NiPoint3& w) {
                    return (w.x - pp.x) * cy - (w.y - pp.y) * sy;
                };
                s.rootLocalR = latOf(cam->cameraRoot->local.translate);
                if (cam->currentState &&
                    cam->currentState.get() == cam->cameraStates[RE::CameraState::kThirdPerson].get()) {
                    auto* tps = static_cast<RE::ThirdPersonState*>(cam->currentState.get());
                    s.haveTps = true;
                    s.transR  = latOf(tps->translation);
                    s.collR   = latOf(tps->collisionPos);
                    const auto& q = tps->rotation;
                    const float qfx = 2.0f * (q.x * q.y - q.w * q.z);
                    const float qfy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
                    s.rotYaw  = std::atan2(qfx, qfy) * 57.2957795f;
                    s.curYaw  = tps->currentYaw * 57.2957795f;
                    s.tgtYaw  = tps->targetYaw  * 57.2957795f;
                }
                s.bodyYaw = byaw * 57.2957795f;
                {
                    const RE::NiPoint3 fwdW = cam->cameraRoot->world.rotate * RE::NiPoint3(0.0f, 1.0f, 0.0f);
                    s.camYaw = std::atan2(fwdW.x, fwdW.y) * 57.2957795f;
                }
                if (auto* root3d = ply->Get3D(false)) {
                    if (auto* c3 = root3d->GetObjectByName("Camera3rd [Cam3]")) {
                        s.haveCam3 = true;
                        s.cam3R    = latOf(c3->world.translate);
                    }
                    if (auto* hd = root3d->GetObjectByName("NPC Head [Head]")) {
                        s.haveHead = true;
                        s.headR    = latOf(hd->world.translate);
                    }
                }
            }
            {
                // Same world->player-yaw projection `r` uses, applied to the
                // noise controller's total applied 3p translation.
                RE::NiMatrix3 nRot{};
                RE::NiPoint3  nTrans{};
                if (CameraNoiseController::GetAppliedCameraOffset3p(nRot, nTrans)) {
                    s.n3pAny = true;
                    s.n3pR   = nTrans.x * cy - nTrans.y * sy;
                }
                if (s_looseValid) {
                    s.looseValid = true;
                    s.engR   = (s_looseEngineCam.x - pp.x) * cy - (s_looseEngineCam.y - pp.y) * sy;
                    s.looseR = s_looseApplied.x * cy - s_looseApplied.y * sy;
                }
            }
            if (auto* aSt = ply->AsActorState()) s.drawn = aSt->IsWeaponDrawn();

            if (s_snapPrimed) {
                const float dr = s.r - s_snapPrevR;
                const float df = s.f - s_snapPrevF;
                const float du = s.u - s_snapPrevU;
                s.dCam = std::sqrt(dr * dr + df * df + du * du);
            }
            s_snapPrevR = s.r; s_snapPrevF = s.f; s_snapPrevU = s.u;
            s_snapPrimed = true;
            // [TWEENR] cross-check: what THIS sample recorded, published so
            // the raw dump below can print it on the same line.
            s_snapLastRootR = s.rootR;
            s_snapLastR     = s.r;
            s_snapLastF     = s.f;

            // Window: from a SPIM edge (either direction) until 1.5s after the
            // LAST edge. Both edges matter â€” the reported snap is "opening the
            // tween menu", but the restore on CLOSE runs the same pinning code
            // in reverse and was never once inside the old probe's window.
            const bool edge = (s.spim != s_snapPrevSpim);
            if (edge) {
                if (s_snapWindowT < 0.0f) {
                    s_snapPeak    = 0.0f;
                    s_snapTripped = false;
                }
                s_snapWindowT = 0.0f;
                spdlog::debug("[SNAP] window {} (spim={})",
                             s.spim ? "OPEN â€” menu framing activated"
                                    : "CLOSE â€” menu framing released",
                             s.spim ? 1 : 0);
            }
            s_snapPrevSpim = s.spim;

            const bool inWindow = (s_snapWindowT >= 0.0f);
            s.t = inWindow ? s_snapWindowT : 0.0f;

            // Ring buffer always records; nothing is printed from it unless a
            // jump trips the detector.
            s_snapRing[s_snapRingN % kSnapRing] = s;
            ++s_snapRingN;

            if (!inWindow) return;
            s_snapWindowT += a_dt;
            s_snapPeak = (std::max)(s_snapPeak, s.dCam);

            // A load door / fast travel / coc moves the player hundreds of
            // units in one frame and takes the camera with it. That is not a
            // snap, and printing it would bury the ones that are.
            const bool teleport = s.dPlayer > 60.0f || s.dCam > 400.0f;
            // ORBITING IS NOT A SNAP (2026-08-20). The measurement frame is the
            // player's BODY yaw, so swinging the camera around with the stick
            // moves r/f by tens of units a frame while nothing is wrong â€” and
            // "menu-ish" includes GameIsPaused, so the window is open a lot.
            // The 19:30 session spent 10 of its 12 dumps on frames with the
            // orbit visibly winding (freeRotX 1.43 -> 0.35 across one pre-roll)
            // and never had a dump left for a still-camera menu open. Excuse a
            // frame whose orbit moved enough to explain the motion: at a ~150u
            // arm, 0.01 rad is already 1.5u, the whole threshold.
            const float dFreeRot = std::abs(s.freeRotX - s_snapPrevFreeRotX);
            s_snapPrevFreeRotX   = s.freeRotX;
            const bool orbiting  = dFreeRot > 0.004f;
            constexpr float kJump = 1.5f;

            if (!teleport && !orbiting && s.dCam > kJump && s_snapDumps < kSnapMaxDumps) {
                if (!s_snapTripped) {
                    s_snapTripped = true;
                    ++s_snapDumps;
                    spdlog::debug("[SNAP] *** JUMP {:.2f}u in one frame at t={:+.3f}s "
                                 "(threshold {:.1f}u, dump {}/{}) â€” pre-roll follows",
                                 s.dCam, s.t, kJump, s_snapDumps, kSnapMaxDumps);
                    const int have = (std::min)(s_snapRingN, kSnapRing);
                    for (int i = have; i >= 1; --i)
                        SnapPrintFrame("pre ", s_snapRing[(s_snapRingN - i) % kSnapRing]);
                    s_snapPost = 16;
                } else {
                    SnapPrintFrame("JUMP", s);
                }
            } else if (s_snapPost > 0) {
                --s_snapPost;
                SnapPrintFrame("post", s);
            }

            // Close the window 1.5s after the last edge and report the peak,
            // so a clean open still leaves ONE line saying it was clean.
            if (s_snapWindowT > 1.5f) {
                s_snapWindowT = -1.0f;
                s_snapPost    = 0;
                spdlog::debug("[SNAP] window closed â€” peak per-frame camera move {:.2f}u ({})",
                             s_snapPeak, s_snapPeak > kJump ? "SNAPPED" : "clean");
            }
        }
    }

    // ----- [DRAWZ] â€” "did my CHARACTER rise, or did the camera drop?" --------
    //
    // The user reports the character's height in the world stepping up on
    // unsheathe and back down on sheathe, and can't tell whether it is an
    // animation or DDC. Those two produce opposite readings, so one sample
    // window settles it without any more theorising:
    //
    //   headZ steps, camZ flat        -> the CHARACTER genuinely moves. Skeleton
    //                                    or animation; nothing DDC can own.
    //   headZ flat, camZ steps        -> the CAMERA moved and the character only
    //                                    appears to. Ours.
    //   both step by the same amount  -> the camera is faithfully following a
    //                                    real character move (also not ours).
    //
    // camZ-headZ is the number the eye actually reads as "how high my character
    // sits in frame", so it is printed directly rather than left to subtraction.
    // Armed on the weapon-draw edge, one line per frame for ~40 frames.
    // ANSWERED 2026-08-22: plyZ was flat to 2dp across every edge, headZ dipped
    // ~1.5u (the combat idle is a slightly lower posture) and the CAMERA rose
    // 22.6u as posOffZ walked -7.2 -> +13.9. So the character never moved; the
    // camera did, because the sheathed and unsheathed profiles were the shipped
    // defaults and genuinely differ. Demoted to debug â€” kept because it is the
    // only probe that separates "character moved" from "camera moved", and that
    // question recurs.
    int s_drawZFrames = 0;

    void ArmDrawZProbe(bool a_drawn)
    {
        if (!spdlog::should_log(spdlog::level::debug)) return;
        s_drawZFrames = 40;
        spdlog::debug("[DRAWZ] armed â€” weapon {}", a_drawn ? "DRAWN" : "SHEATHED");
    }

    void DrawZTick()
    {
        if (s_drawZFrames <= 0) return;
        --s_drawZFrames;

        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!cam || !ply || !cam->cameraRoot) return;

        RE::NiPoint3 cw = cam->cameraRoot->world.translate;
        if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
            if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                cw = ni->world.translate;
            }
        }

        const RE::NiPoint3 pp = ply->GetPosition();
        // Head bone, not the root: the root is the collision capsule's floor
        // contact and barely moves, while the head is what the framing is
        // about and what a posture change actually shifts.
        float headZ = std::numeric_limits<float>::quiet_NaN();
        if (auto* p3 = ply->Get3D())
            if (auto* h = p3->GetObjectByName("NPC Head [Head]"))
                headZ = h->world.translate.z;

        float offZ = std::numeric_limits<float>::quiet_NaN();
        if (cam->currentState &&
            cam->currentState.get() == cam->cameraStates[RE::CameraState::kThirdPerson].get()) {
            offZ = static_cast<RE::ThirdPersonState*>(cam->currentState.get())->posOffsetActual.z;
        }

        bool drawn = false;
        if (auto* aSt = ply->AsActorState()) drawn = aSt->IsWeaponDrawn();

        spdlog::debug("[DRAWZ] f={:2d} drawn={} | plyZ={:8.2f} headZ={:8.2f} camZ={:8.2f} "
                     "| head-ply={:6.2f} cam-head={:7.2f} | posOffZ={:6.2f}",
                     40 - s_drawZFrames, drawn, pp.z, headZ, cw.z,
                     headZ - pp.z, cw.z - headZ, offZ);
    }

    // ----- [TWEENCAM] â€” the menu-camera distance step ------------------------
    //
    // "The tween menu zooms out in a snap when accessing inventory or magic."
    // Distance, not FOV â€” so it is the half of the Tween problem the FOV hold
    // deliberately did not take, and it needs naming before it can be fixed.
    // Prints on every entry to / exit from kTween and for a short window after,
    // with the menu name so a Tween->Inventory hand-off is distinguishable from
    // the Tween open itself.
    // Round 1 of this probe armed only on a CAMERA-STATE change, and caught
    // nothing: Tween -> Inventory keeps the camera in kTween the whole time, so
    // the hand-off the user is actually reporting never re-armed the window. It
    // now arms on a MENU change as well, and simply keeps logging for as long as
    // one of these menus is up (capped), because that whole span is short,
    // paused, and is the only span that matters.
    int         s_tweenCamFrames    = 0;     // trailing window after the last edge
    int         s_tweenCamSession   = 0;     // cap while a menu is up
    int         s_tweenCamTotal     = 0;     // whole-session cap: 3901 of the 8668 lines in the
                                             // 2026-09-05 log were this probe, burying everything else
    int         s_tweenCamPrevState = -1;
    const char* s_tweenCamPrevMenu  = "-";

    // ROUND 2 ANSWERED 2026-08-22, and the answer was "not us". Across a full
    // Tween -> Inventory -> close sequence, distRoot and rootZ never left
    // 73.06 / -5556.76 â€” the game camera node does not move. And this hook did
    // not tick AT ALL while InventoryMenu / MagicMenu were up: samples ran
    // continuously through the Tween window, then stopped dead from the
    // Inventory open to its close, ~1.2s with no camera pass.
    //
    // RE-ARMED AT INFO 2026-08-23. The user reports the zoom again â€” but the
    // premise of that answer has since changed underneath it. Round 2 measured
    // an install where Show Player In Menus did not handle InventoryMenu at all
    // ("OnMenuOpenChange(InventoryMenu,open) not-handled" is in that same log);
    // the preset now has inventory / magic / tween each carrying
    // allow_camera_control + unpause_game, so the game world RUNS through these
    // menus and DDC's camera pipeline is live on exactly the frames Round 2
    // found empty. Whether the node still holds still is therefore an open
    // question again, and it is one measurement away from being closed.
    // The release build runs this probe only with Verbose Logging enabled.
    void TweenCamTick()
    {
        if (!spdlog::should_log(spdlog::level::debug)) return;

        // ONE SAMPLE PER RENDERED FRAME. Two callers now (UpdateCameraPost,
        // which stops during Inventory / Magic, and NiCamera::UpdateWorldData,
        // which does not) â€” whichever fires first owns the frame. 4ms is well
        // under a 60fps frame and well over the gap between the two calls
        // within one, so this collapses the duplicate without ever dropping a
        // real frame.
        {
            static auto sLast = std::chrono::steady_clock::now();
            const auto  now   = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(now - sLast).count() < 0.004f) return;
            sLast = now;
        }

        auto* cam = RE::PlayerCamera::GetSingleton();
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!cam || !ply || !cam->cameraRoot) return;

        // Which menu is up, read straight from the UI â€” the controller's own
        // topmost-menu helper is private, and this probe wants the raw truth
        // anyway, including menus the controller does not handle.
        const char* menu = "-";
        if (auto* ui = RE::UI::GetSingleton()) {
            if (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME))      menu = "Inventory";
            else if (ui->IsMenuOpen(RE::MagicMenu::MENU_NAME))     menu = "Magic";
            else if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) menu = "Favorites";
            else if (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) menu = "Container";
            else if (ui->IsMenuOpen(RE::TweenMenu::MENU_NAME))     menu = "Tween";
        }
        const bool menuUp   = (menu[0] != '-');
        const int  stateNow = cam->currentState ? static_cast<int>(cam->currentState->id) : -1;

        // Distance to the MOUNT when there is one. distRoot exists to say "did
        // the camera stay with the player", and PlayerCharacter::GetPosition
        // does not track the rider usefully while mounted â€” the 14:36 dragon
        // samples read a pinned 3960.00 for frames on end while rootZ drifted,
        // which is a stale reference point, not a camera 56 metres behind.
        RE::NiPointer<RE::Actor> mountRef;
        const bool  haveMount = ply->GetMount(mountRef) && mountRef;
        RE::TESObjectREFR* anchor = haveMount ? static_cast<RE::TESObjectREFR*>(mountRef.get())
                                              : static_cast<RE::TESObjectREFR*>(ply);

        const bool menuEdge  = (menu != s_tweenCamPrevMenu);
        const bool stateEdge = (stateNow != s_tweenCamPrevState);
        if (menuEdge || stateEdge) {
            spdlog::debug("[TWEENCAM] EDGE menu {} -> {} | camState {} -> {}",
                          s_tweenCamPrevMenu, menu, s_tweenCamPrevState, stateNow);
            s_tweenCamFrames = 30;
            // A menu WINDOW is one open..close span, not one menu: Tween ->
            // Inventory -> close is a single window, and the hand-off inside it
            // is the thing being measured.
            //
            if (menuEdge && !menuUp) {
                s_tweenCamSession = 0;
            }
        }
        s_tweenCamPrevMenu  = menu;
        s_tweenCamPrevState = stateNow;

        // Read BOTH nodes rather than only the NiCamera child. If the two
        // disagree the step is a node swap, not a camera move â€” the same trap
        // [SNAP] documents, and worth ruling out before chasing a zoom channel.
        const RE::NiPoint3 rootW = cam->cameraRoot->world.translate;
        RE::NiPoint3       niW     = rootW;
        RE::NiMatrix3      viewRot = cam->cameraRoot->world.rotate;
        bool               haveNi  = false;
        if (auto* nd = cam->cameraRoot->AsNode(); nd && !nd->GetChildren().empty()) {
            if (auto* ni = skyrim_cast<RE::NiCamera*>(nd->GetChildren()[0].get())) {
                niW     = ni->world.translate;
                viewRot = ni->world.rotate;
                haveNi  = true;
            }
        }
        const RE::NiPoint3 pp = anchor->GetPosition();
        const auto planar = [&](const RE::NiPoint3& c) {
            const float dx = c.x - pp.x, dy = c.y - pp.y;
            return std::sqrt(dx * dx + dy * dy);
        };
        // Horizontal distance from the player is the "zoomed out" the user sees;
        // the zoom offsets below say whether the engine's own zoom channel is
        // what moved it, or whether the menu camera composed a different
        // position outright. Those need different fixes.
        const float distRoot = planar(rootW);
        const float distNi   = planar(niW);

        // RENDERED camera yaw — the subject the position-only probes are
        // blind to (user 2026-09-03: Tween open "snaps to the right then moves
        // back to the original angle"). Heading of the camera's world forward
        // projected to horizontal; the absolute convention does not matter,
        // only that a snap shows as a jump and the ease-back as it walks back.
        const RE::NiPoint3 fwd = viewRot * RE::NiPoint3(0.0f, 1.0f, 0.0f);
        const float camYaw = std::atan2(fwd.x, fwd.y) * 57.2957795f;

        float zc  = std::numeric_limits<float>::quiet_NaN();
        float zt  = zc;
        float frY = zc;   // freeRotation.y — the DATA channel ActivateForMenu pins
        if (auto* tps3p = skyrim_cast<RE::ThirdPersonState*>(
                cam->cameraStates[RE::CameraState::kThirdPerson].get())) {
            zc  = tps3p->currentZoomOffset;
            zt  = tps3p->targetZoomOffset;
            frY = tps3p->freeRotation.y;
        }
        const float bodyZ = ply->data.angle.z * 57.2957795f;

        // DELTA-GATE. This used to log every frame a menu was up (~60/s), which
        // floods the log and buries the one frame that moved. Log only when
        // yaw / distance / height / fov / zoom actually change past a small
        // threshold, plus the short post-edge settle window. A steady open is
        // now a few lines; a snap-and-return is exactly the lines that moved.
        // If camYaw jumps while frY and bodyZ hold, the pin is not reaching the
        // render path; if frY moves, a writer re-reset it after activation.
        static float sLastYaw = 1e9f, sLastDist = 1e9f, sLastZ = 1e9f,
                     sLastFov = 1e9f, sLastZc = 1e9f;
        const bool moved =
            std::abs(camYaw        - sLastYaw)  > 0.10f ||
            std::abs(distRoot      - sLastDist) > 0.05f ||
            std::abs(rootW.z       - sLastZ)    > 0.05f ||
            std::abs(cam->worldFOV - sLastFov)  > 0.05f ||
            std::abs(zc            - sLastZc)   > 0.001f;
        bool log = false;
        constexpr int kTweenCamTotalCap = 600;
        if (s_tweenCamFrames > 0 && s_tweenCamTotal < kTweenCamTotalCap) { --s_tweenCamFrames; ++s_tweenCamTotal; log = true; }
        if (menuUp && moved && s_tweenCamSession < 400 && s_tweenCamTotal < kTweenCamTotalCap) { ++s_tweenCamSession; ++s_tweenCamTotal; log = true; }
        if (!log) return;
        sLastYaw = camYaw; sLastDist = distRoot; sLastZ = rootW.z;
        sLastFov = cam->worldFOV; sLastZc = zc;

        const auto& spim = ShowPlayerInMenusController::GetSingleton();
        spdlog::debug("[TWEENCAM] camState={} menu={} spim={} ni={} anchor={} | "
                    "camYaw={:8.2f} frY={:7.2f} bodyZ={:8.2f} | "
                    "distRoot={:7.2f} distNi={:7.2f} "
                    "rootZ={:8.2f} niZ={:8.2f} fov={:6.2f} | 3p zoom c={:.3f} t={:.3f}",
                    stateNow, menu, spim.IsActive(), haveNi, haveMount ? "mount" : "player",
                    camYaw, frY, bodyZ,
                    distRoot, distNi, rootW.z, niW.z, cam->worldFOV, zc, zt);
    }

    // ----- Per-vtable original dispatch -------------------------------------
    //
    // One hook function is installed into three different vtables
    // (ThirdPersonState, HorseCameraState, DragonCameraState). Which original
    // to chain to depends on which vtable THIS instance came from, and
    // TESCameraState::id says exactly that. Getting this wrong is not a
    // subtle bug: calling ThirdPersonState::Update for the horse camera
    // bypasses both the horse's own logic and any other mod hooked there.
    void HookManager::CallOriginalUpdate(RE::ThirdPersonState* a_this,
                                         RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        if (a_this) {
            if (a_this->id == RE::CameraState::kMount && _originalHorseUpdate.address()) {
                _originalHorseUpdate(a_this, a_nextState);
                return;
            }
            if (a_this->id == RE::CameraState::kDragon && _originalDragonUpdate.address()) {
                _originalDragonUpdate(a_this, a_nextState);
                return;
            }
        }
        _originalThirdPersonUpdate(a_this, a_nextState);
    }

    void HookManager::CallOriginalGetRotation(RE::ThirdPersonState* a_this, RE::NiQuaternion& a_rotation)
    {
        if (a_this) {
            if (a_this->id == RE::CameraState::kMount && _originalHorseGetRotation.address()) {
                _originalHorseGetRotation(a_this, a_rotation);
                return;
            }
            if (a_this->id == RE::CameraState::kDragon && _originalDragonGetRotation.address()) {
                _originalDragonGetRotation(a_this, a_rotation);
                return;
            }
        }
        _originalGetRotation(a_this, a_rotation);
    }

    void HookManager::CallOriginalGetTranslation(RE::ThirdPersonState* a_this, RE::NiPoint3& a_translation)
    {
        if (a_this && a_this->id == RE::CameraState::kMount &&
            _originalHorseGetTranslation.address()) {
            _originalHorseGetTranslation(a_this, a_translation);
            return;
        }
        _originalGetTranslation(a_this, a_translation);
    }

    void HookManager::CallOriginalProcessWeaponDrawnChange(RE::ThirdPersonState* a_this, bool a_drawn)
    {
        if (a_this && a_this->id == RE::CameraState::kMount &&
            _originalHorseProcessWeaponDrawnChange.address()) {
            _originalHorseProcessWeaponDrawnChange(a_this, a_drawn);
            return;
        }
        _originalProcessWeaponDrawnChange(a_this, a_drawn);
    }

    namespace
    {
        // Raw PlayerCamera::SetCameraState — non-virtual, takes the camera and
        // a CameraState id. Used by BOTH the furniture kill and [HORSEKILL] to
        // force a state through the normal state machine without going through
        // the engine's own entry path. Declared here because the horse kill
        // needs it in HookedThirdPersonUpdate, well above the furniture code.
        using SetCameraState_t = void(*)(RE::PlayerCamera*, RE::CameraState);
        REL::Relocation<SetCameraState_t> sPlayerCameraSetState{ REL::RelocationID(49947, 50880) };
    }

    void HookManager::HookedThirdPersonUpdate(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        TweenCameraTrace::BeginCameraPass();
        TweenCameraTrace::StageSample thirdPersonTrace("3p-exit");
        // Single SettingsManager singleton lookup for the whole hook â€”
        // this function previously called GetSingleton several separate
        // times (suspend flag, indoorMode write, sCfg, settings). Reusing
        // one reference is observably identical and trims a few function
        // calls + cache lookups from the per-frame hot path.
        auto& settings = SettingsManager::GetSingleton();

        // Advance at the state-update boundary, before its rotation and position
        // calculation. The later TESCamera trampoline must not reset this state.
        ++s_mountedLockFrame;
        s_mountedLockSteady = false;
        s_mountedLockOwnsYaw = false;
        static auto mountedLastFrame = std::chrono::steady_clock::now();
        const auto mountedNow = std::chrono::steady_clock::now();
        s_mountedLockDt = std::chrono::duration<float>(mountedNow - mountedLastFrame).count();
        mountedLastFrame = mountedNow;
        // The engine delta excludes time spent outside camera updates (e.g. a
        // paused menu). Keep wall time only as a fallback before the timer exists.
        if (const auto* timer = RE::BSTimer::GetSingleton()) s_mountedLockDt = timer->realTimeDelta;
        if (!std::isfinite(s_mountedLockDt) || s_mountedLockDt < 0.0f) s_mountedLockDt = 0.0f;
        auto* mountedUi = RE::UI::GetSingleton();
        const auto& mountedMenus = ShowPlayerInMenusController::GetSingleton();
        const bool mountedMenuFraming = mountedMenus.IsActive() &&
                                       !mountedMenus.IsActiveCameraControlEnabled();
        const bool mountedDialogue = mountedUi && mountedUi->IsMenuOpen("Dialogue Menu");
        const bool mountedCameraState = a_this &&
            (a_this->id == RE::CameraState::kMount ||
             a_this->id == RE::CameraState::kThirdPerson);
        s_mountedLockOutputAllowed = mountedCameraState && !settings.diagnosticSuspendOverrides &&
                                    !mountedDialogue && !mountedMenuFraming && !VanityCamera::IsActive();
        if (!s_mountedLockOutputAllowed) s_mountedLockPitch.Reset();
        const auto* mountedMain = RE::Main::GetSingleton();
        if ((mountedUi && mountedUi->GameIsPaused()) || (mountedMain && mountedMain->freezeTime))
            s_mountedLockDt = 0.0f;


        // ----- [CAMANIM] ANIMATIONS DO NOT DRIVE THE CAMERA ----------------
        //
        // Fresh investigation, 2026-08-27. The complaint: shield sprinting
        // pitches the camera down even though every profile sits at
        // pitchOffset 0 — and the user's framing is the design statement:
        // "why can't this function like normal sprinting entries?"
        //
        // The engine has a documented channel for exactly this: behaviour
        // graphs can animate a camera bone — FixedStrings 0x1E8 is
        // "Camera3rd[Cam3]" — and the [Camera] INI switch
        // bApplyCameraNodeAnimations (default TRUE, absent from this user's
        // INIs) tells the third-person camera to compose that animation data
        // on top of its own framing. Ordinary sprint has no camera track, so
        // it looks fine; the shield-charge animation ships one, so it
        // doesn't. Under DDC the switch is wrong in principle, not just for
        // this one animation: this mod owns the third-person framing, and an
        // animation shoving the camera is the same class of intrusion as the
        // furniture camera (killed) and the engine's transition lerps
        // (covered). So the switch goes off — in memory only, the user's INI
        // files are never written.
        // (An attempt to disable animation-driven camera via the
        // bApplyCameraNodeAnimations INI switch lived here for one build.
        // Measured 2026-08-27 20:15: the setting DOES NOT EXIST in either
        // INI collection at runtime. The real mechanism is the
        // StartAnimatedCameraDelta anim event — see [ANIMCAM].)

        // [ANIMCAM-HOOK] recent-block sampler: the charge's Start event fires
        // on the sprint's first frame with IsBlocking() already false, so the
        // suppress predicate is "was blocking within the last half second" —
        // sampled here every 3p frame.
        if (auto* plyBlk = RE::PlayerCharacter::GetSingleton();
            plyBlk && plyBlk->IsBlocking()) {
            sLastBlockingMs.store(NowMs(), std::memory_order_relaxed);
        }

        // [ANIMCAM] bone hold — kept for its window/confirm logging, though
        // the hold itself is proven inert (the mode reads the graph's own
        // camera track, not the scene bone; measured 21:49 — bone held at
        // rest, full tilt rendered anyway). The real fix is [ANIMCAM-HOOK].
        ApplyAnimCamBoneHold(RE::PlayerCamera::GetSingleton());

        // ----- [SSPRINT] the verification for the above, from scratch ------
        //
        // One line per sprinting frame, PLAIN vs BLOCKING distinguished, with
        // every candidate on it and no convention assumed anywhere:
        //   - the animated camera bone's LOCAL transform (translate + the z
        //     row of its rotation) — if the charge animates the bone, it
        //     shows here, and with [CAMANIM] off it must stop reaching the
        //     render;
        //   - the rendered NiCamera's rotation z-row, raw (one frame stale —
        //     this hook runs before this frame's render);
        //   - bodyX and freeRotation.y, the two inputs the camera's pitch is
        //     normally built from.
        // If the rendered row still diverges between plain and blocking after
        // [CAMANIM], the mechanism is NOT the camera bone and these columns
        // say which input moved instead.
        // First capture (20:15) taught three things: IsBlocking() is FALSE
        // during the charge (useless as a gate or a label), the flat 150-line
        // cap filled with plain sprint before the charge ever happened, and
        // the tilt IS visible in the rendered z-row (0.39 rad vs bodyX
        // 0.133, freeRotY 0). So: log on DEVIATION, not on a flat cap, and
        // record the two upstream stages (camera bone WORLD, cameraRoot
        // LOCAL) so the line that diverges names the injection stage.
        if (a_this && a_this->id == RE::CameraState::kThirdPerson) {
            static int sSpLines = 0;
            auto* plSp = RE::PlayerCharacter::GetSingleton();
            // PROBE HOLE FIXED 2026-08-29: sprint-flag-only gating meant the
            // charge-STOP animation — which runs AFTER IsSprinting() drops —
            // was invisible to this probe, so an exit-side jerk could never
            // appear in the log. Keep logging for a 1s tail past the flag.
            static long long sLastSprintMs = 0;
            const bool sprintingNow = plSp && plSp->AsActorState() &&
                                      plSp->AsActorState()->IsSprinting();
            if (sprintingNow) sLastSprintMs = NowMs();
            const bool inExitTail = !sprintingNow && sLastSprintMs != 0 &&
                                    (NowMs() - sLastSprintMs) < 1000;
            if (sSpLines < 600 && plSp && (sprintingNow || inExitTail)) {
                float bz0 = 0, bz1 = 0, bz2 = 0, nly = 0;
                if (auto* obj = a_this->thirdPersonCameraObj) {
                    nly = obj->local.translate.y;
                    bz0 = obj->world.rotate.entry[2][0];
                    bz1 = obj->world.rotate.entry[2][1];
                    bz2 = obj->world.rotate.entry[2][2];
                }
                float rz0 = 0, rz1 = 0, cz0 = 0, cz1 = 0, cz2 = 0;
                RE::NiPoint3 camPos{};
                bool havePos = false;
                if (auto* pcSp = RE::PlayerCamera::GetSingleton();
                    pcSp && pcSp->cameraRoot) {
                    rz0 = pcSp->cameraRoot->local.rotate.entry[2][0];
                    rz1 = pcSp->cameraRoot->local.rotate.entry[2][1];
                    if (auto* rootSp = pcSp->cameraRoot->AsNode()) {
                        for (auto& ch : rootSp->GetChildren()) {
                            if (auto* nc = skyrim_cast<RE::NiCamera*>(ch.get())) {
                                cz0 = nc->world.rotate.entry[2][0];
                                cz1 = nc->world.rotate.entry[2][1];
                                cz2 = nc->world.rotate.entry[2][2];
                                camPos  = nc->world.translate;
                                havePos = true;
                                break;
                            }
                        }
                    }
                }
                // Rendered pitch vs the clean-frame identity, both from the
                // 20:15 capture's own numbers: clean rows obey
                // niRz = (-sin(bodyX - freeRotY), cos(..), 0) exactly.
                const float ident = plSp->data.angle.x - a_this->freeRotation.y;
                const float rendP = std::atan2(-cz0, cz1);
                const float dev   = rendP - ident;
                // POSITION + ZOOM channels (2026-08-29): the pitch channel
                // measured FLAT through charges the user still felt, so the
                // jerk lives elsewhere — and these are the remaining movers.
                // dRel = per-frame change of (camera − player root), which
                // subtracts sprint's forward motion; if the user sees a jerk
                // while dRel AND relZ stay flat, the camera is innocent and
                // the BODY animation itself is what lurches on screen.
                static RE::NiPoint3 sPrevRel{};
                static long long    sPrevRelMs = 0;
                float dRel = 0.0f, relZ = 0.0f;
                if (havePos) {
                    const RE::NiPoint3 rel = camPos - plSp->GetPosition();
                    relZ = rel.z;
                    const long long nowP = NowMs();
                    if (nowP - sPrevRelMs < 250)
                        dRel = (rel - sPrevRel).Length();
                    sPrevRel   = rel;
                    sPrevRelMs = nowP;
                }
                static int sEvery = 0;
                const bool interesting = std::abs(dev) > 0.02f || dRel > 1.5f ||
                                         (++sEvery % 15) == 0;
                if (interesting) {
                    ++sSpLines;
                    spdlog::debug("[SSPRINT] dev={:+.4f} ({:+.2f} deg) dRel={:.2f} relZ={:+.1f} "
                                 "zoom={:+.3f}/{:+.3f} pzo={:+.2f} | rootLz=({:+.3f},{:+.3f}) "
                                 "niRz=({:+.3f},{:+.3f},{:+.3f}) | boneWz=({:+.3f},{:+.3f},{:+.3f}) "
                                 "boneLY={:+.1f} | bodyX={:+.4f} freeRotY={:+.4f}",
                                 dev, dev * 57.2958f, dRel, relZ,
                                 a_this->currentZoomOffset, a_this->targetZoomOffset,
                                 a_this->pitchZoomOffset,
                                 rz0, rz1, cz0, cz1, cz2,
                                 bz0, bz1, bz2, nly,
                                 plSp->data.angle.x, a_this->freeRotation.y);
                }
            }
        }

        // ([SNAPXRAY]'s three per-frame stage samples lived here and at the
        // NiCamera hook until 2026-08-19. Every stage agreed to 0.1u across
        // an activation, so the decomposition had nothing left to say; the
        // replacement, [SNAP], takes one sample per frame from
        // HookedUpdateCameraPost â€” which unlike this hook ticks in every
        // camera state â€” and prints only when the camera actually jumps.)

        // ----- DEAD-MOUNT GUARD -------------------------------------------
        //
        // True Directional Movement dereferences the horse WITHOUT a null
        // check any time the camera state id reads kMount:
        //
        //   DirectionalMovementHandler.cpp:252
        //     auto horseCameraState = static_cast<RE::HorseCameraState*>(tps);
        //     cameraTarget = horseCameraState->horseRefHandle.get().get();
        //     ... cameraTarget->data.angle.z          // <- rdi = 0 here
        //   and again at :772 via horseRefHandle.get()->As<RE::Actor>()
        //
        // (Its OTHER mounted branch, :1430, does check. Two out of three.)
        //
        // So the fatal condition is simply "camera still in kMount after the
        // horse handle has died", and dismount is exactly when that window
        // exists. Crashed three times in a row — 18:04, 18:19, 18:36 — every
        // one on getting off a horse, every one the same instruction.
        //
        // DDC cannot fix TDM, but it can make sure that window is never open
        // on a frame TDM gets to run: this ticks inside the mount camera's own
        // Update, so leaving the state here happens before anything else that
        // frame reads the id. Gated on the PLAYER also being off the mount, so
        // a merely-transient handle while genuinely riding is left alone.
        if (a_this && a_this->id == RE::CameraState::kMount) {
            auto* pcMount = RE::PlayerCamera::GetSingleton();
            auto* plMount = RE::PlayerCharacter::GetSingleton();
            if (pcMount && plMount && pcMount->currentState.get() == a_this) {
                auto* horseSt = static_cast<RE::HorseCameraState*>(a_this);
                const bool handleDead = !horseSt->horseRefHandle ||
                                        !horseSt->horseRefHandle.get();
                if (handleDead && !plMount->IsOnMount()) {
                    static int sDeadMountKicks = 0;
                    if (sDeadMountKicks < 10) {
                        ++sDeadMountKicks;
                        spdlog::warn("[MOUNTGUARD] mount camera is current with a dead horse "
                                     "handle and the player is off the mount — forcing third "
                                     "person before TDM dereferences it (kick {}/10)",
                                     sDeadMountKicks);
                    }
                    pcMount->ForceThirdPerson();
                    return;
                }
            }
        }

        s_lastFrameWasFirstPerson.store(false, std::memory_order_relaxed);

        // Before the early-outs below, and before _original runs: the engine
        // reads fPitchZoomOutMaxDist inside its own Update, so this has to be
        // settled first. Suspend-overrides restores the vanilla value along
        // with everything else it hands back.
        ApplyPitchZoomOut(a_this, settings.diagnosticSuspendOverrides);

        // Diagnostic short-circuit: pure-vanilla mode for capturing
        // engine-natural camera values via the snapshot button.
        if (settings.diagnosticSuspendOverrides) {
            ResetMenuCameraAnimation();
            CallOriginalUpdate(a_this, a_nextState);
            return;
        }

        // Being in the 3p update at all means the player is NOT in first person,
        // so the 1p dialogue face-lock's IsNPC / dialogue-idle anim override
        // (installed per-frame ONLY by the 1p hook) must not still be active. If
        // the user switched 1p->3p mid-dialogue it IS still set; release it here
        // on the first 3p frame, so the pose transition is masked by the POV
        // switch instead of firing the whole graph reset in one frame on the 3p
        // EXIT â€” that one-frame reset is the reported exit snap. Self-gated, so a
        // no-op for plain 3p dialogue (never sets it) and for non-dialogue 3p.
        // Covers NPC and statue speakers alike (the Actor-gated close-edge
        // restore below misses activator speakers like the Statue of Mara).
        RestoreDialogueNpcOverride();

        // Magic release-edge detection. Runs at the START of camera
        // update, BEFORE niCam matrix is finalized for this frame. At
        // this point, in Skyrim's standard actor-before-camera order,
        // the actor update has already transitioned the magic caster
        // state from kReady â†’ kCasting (release). niCam->world.rotate
        // still has LAST frame's final value â€” which is exactly what
        // the user saw on screen when they decided to click. Capturing
        // niCam here gives us the gun-sights direction at trigger pull,
        // not the spawn-time direction K frames later.
        {
            static bool sPrevChargingHostile[4] = { false, false, false, false };
            static bool sSawFiringInCycle[4]    = { false, false, false, false };
            auto* plyRel = RE::PlayerCharacter::GetSingleton();
            if (plyRel) {
                int srcIdx = 0;
                for (auto src : { RE::MagicSystem::CastingSource::kRightHand,
                                  RE::MagicSystem::CastingSource::kLeftHand,
                                  RE::MagicSystem::CastingSource::kOther,
                                  RE::MagicSystem::CastingSource::kInstant })
                {
                    auto* caster = plyRel->GetMagicCaster(src);
                    auto chargingHostile = [](RE::MagicCaster* c) -> bool {
                        if (!c) return false;
                        using CState = RE::MagicCaster::State;
                        const auto cs = c->state.get();
                        if (cs == CState::kNone)    return false;
                        if (cs == CState::kCasting) return false;
                        if (cs == CState::kUnk07 || cs == CState::kUnk08 || cs == CState::kUnk09)
                            return false;
                        auto* mi = c->currentSpell;
                        if (!mi) return false;
                        if (mi->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget)
                            return false;
                        for (auto* e : mi->effects) {
                            if (e && e->baseEffect && e->baseEffect->IsHostile())
                                return true;
                        }
                        return false;
                    };
                    const bool isCharging = chargingHostile(caster);
                    if (caster) {
                        const auto cs = caster->state.get();
                        if (cs == RE::MagicCaster::State::kReady ||
                            cs == RE::MagicCaster::State::kCasting)
                        {
                            sSawFiringInCycle[srcIdx] = true;
                        }
                    }
                    if (sPrevChargingHostile[srcIdx] && !isCharging && sSawFiringInCycle[srcIdx]) {
                        // Capture niCam at this moment (= last frame's
                        // final = trigger-pull view direction).
                        auto* pc = RE::PlayerCamera::GetSingleton();
                        if (pc && pc->cameraRoot) {
                            auto* asNode = pc->cameraRoot->AsNode();
                            if (asNode && !asNode->GetChildren().empty()) {
                                if (auto* niCam = skyrim_cast<RE::NiCamera*>(
                                        asNode->GetChildren()[0].get()))
                                {
                                    auto mm = niCam->world.rotate;
                                    RE::NiPoint3 relPos = niCam->world.translate;
                                    // The charging preview removes camera noise
                                    // and Repulse. Capture the same aiming basis
                                    // here so a visual kick cannot lift the shot
                                    // away from the line the player just saw.
                                    if (!pc->IsInFirstPerson()) {
                                        RE::NiMatrix3 noiseRotation;
                                        RE::NiPoint3 noiseTranslation;
                                        if (CameraNoiseController::GetAppliedCameraOffset3p(noiseRotation, noiseTranslation)) {
                                            mm = mm * noiseRotation.Transpose();
                                            relPos = relPos - noiseTranslation;
                                        }
                                    }
                                    const RE::NiPoint3 relFwd{
                                        mm.entry[0][0], mm.entry[1][0], mm.entry[2][0] };
                                    const RE::NiPoint3 relUp{
                                        mm.entry[0][1], mm.entry[1][1], mm.entry[2][1] };
                                    constexpr float kRayLen = 8000.0f;
                                    RE::NiPoint3 relTarget{
                                        relPos.x + relFwd.x * kRayLen,
                                        relPos.y + relFwd.y * kRayLen,
                                        relPos.z + relFwd.z * kRayLen,
                                    };
                                    if (auto* cell = plyRel->GetParentCell()) {
                                        if (auto* bhkW = cell->GetbhkWorld()) {
                                            if (auto* hkW = bhkW->GetWorld1()) {
                                                const float ws = RE::bhkWorld::GetWorldScale();
                                                RE::hkpWorldRayCastInput  in;
                                                RE::hkpWorldRayCastOutput out;
                                                in.from.quad = _mm_setr_ps(
                                                    relPos.x*ws, relPos.y*ws, relPos.z*ws, 0.0f);
                                                in.to.quad = _mm_setr_ps(
                                                    relTarget.x*ws, relTarget.y*ws, relTarget.z*ws, 0.0f);
                                                in.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kCameraSphere);
                                                in.enableShapeCollectionFilter = false;
                                                hkW->CastRay(in, out);
                                                if (out.HasHit()) {
                                                    relTarget = RE::NiPoint3{
                                                        relPos.x + relFwd.x * (kRayLen * out.hitFraction),
                                                        relPos.y + relFwd.y * (kRayLen * out.hitFraction),
                                                        relPos.z + relFwd.z * (kRayLen * out.hitFraction),
                                                    };
                                                }
                                            }
                                        }
                                    }
                                    DietDrCamera::MissileProjectileDetour::PublishReleaseCapture(
                                        relFwd, relUp, relPos, relTarget);
                                }
                            }
                        }
                    }
                    if (sPrevChargingHostile[srcIdx] && !isCharging) {
                        sSawFiringInCycle[srcIdx] = false;
                    }
                    sPrevChargingHostile[srcIdx] = isCharging;
                    ++srcIdx;
                }
            }
        }

        // Indoor / Outdoor cell detection. Sets settings.indoorMode each
        // frame from the player's parent cell. ResolveByEnv reads this
        // flag at the picker boundary (CameraController + the rotation
        // pickup below) to substitute the indoor variant when applicable.
        // Loading screens gate most interiorâ†”exterior transitions, so the
        // user never sees the in-flight switch â€” they're already in the
        // new cell when the screen lifts and the spring just settles.
        if (auto* cellPlayer = RE::PlayerCharacter::GetSingleton()) {
            if (auto* parentCell = cellPlayer->GetParentCell()) {
                settings.indoorMode = parentCell->IsInteriorCell();
            }
        }
        // Location-override detection rides alongside it â€” see
        // LocationDetector for why membership comes from the location record's
        // parent chain rather than a distance from the map marker.
        LocationDetector::GetSingleton().Update();

        // Reset the dialogue face-lock blend state on open/close edges.
        // Must run before the face-lock block below so s_dialogueAimInit
        // reflects the current frame's menu state.
        TickDialogueAimInit();

        // Player is alive again â€” clear the death free-look collision
        // override so UpdateCameraCasterPatch below reverts to the settings
        // value.
        s_bleedoutFreeLook.Reset();
        s_forceCasterPatchForDeathFreeLook = false;

        // Safety: if a reload happened mid-slow-mo (BleedoutEnd often doesn't
        // fire on death->reload), make sure normal time is restored now that
        // we're back in third person.
        RestoreDeathSlowmo();

        // Keep fPlayerDeathReloadTime in sync with the slider while alive, so
        // the engine doesn't start the bleedout countdown with a stale value
        // after a mid-gameplay adjustment. Writes are cheap and guarded so
        // unchanged values don't touch the settings map.
        //
        // Infinite Duration overrides the slider with a day's worth of
        // seconds â€” the engine's countdown effectively never expires
        // and the user must use the skip hotkey (or ESC menu â†’ Load)
        // to leave bleedout.
        {
            const float desired = settings.deathCameraInfiniteDuration
                ? 86400.0f
                : settings.deathCameraHoldDuration;
            // Compare against the LIVE setting (not a cached last-written) so the
            // value self-corrects: after a ragdoll temporarily forced it high to
            // suppress the reload prompt, this restores the slider value the moment
            // the player is back on their feet (and in third person again).
            if (auto* gsc = RE::GameSettingCollection::GetSingleton()) {
                if (auto* setting = gsc->GetSetting("fPlayerDeathReloadTime"))
                    if (std::abs(setting->data.f - desired) > 0.01f) setting->data.f = desired;
            }
            if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                if (auto* setting = ini->GetSetting("fPlayerDeathReloadTime:GamePlay"))
                    if (std::abs(setting->data.f - desired) > 0.01f) setting->data.f = desired;
            }
        }

        // Skip during killcam, VATS, etc. We're the "current" camera state only
        // when a_this matches playerCam->currentState â€” true for either the
        // third-person or mounted state via our dual-vtable hook.
        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (playerCam && playerCam->currentState.get() != a_this) {
            CallOriginalUpdate(a_this, a_nextState);
            return;
        }

        // If R3 is currently held and the user pressed from 1p, the engine
        // has auto-transitioned to 3p behind our back. Force back to 1p â€”
        // and starve the engine's re-trigger by zeroing zoomInput and the
        // ThirdPersonState zoom fields so the auto-transition logic can't
        // keep pulling us back into 3p. Skip _originalThirdPersonUpdate
        // because running it gives the engine another chance to drive.
        if (s_r3Held && s_r3PressWasFirst && playerCam) {
            static int sTick = 0;
            if ((++sTick % 15) == 0) {
                auto* cur = playerCam->currentState.get();
                int stateId = cur ? static_cast<int>(cur->id) : -1;
                spdlog::info("TPStateDiag: r3Held currentStateId={} reverting to 1p", stateId);
            }
            playerCam->zoomInput         = 0.0f;
            a_this->targetZoomOffset     = 0.0f;
            a_this->currentZoomOffset    = 0.0f;
            playerCam->ForceFirstPerson();
            return;
        }
        // F-hold reactive revert via direct state-pointer swap. Earlier
        // attempt used playerCam->ForceFirstPerson() here and that
        // triggered head-bone resets per frame, making the 1p camera
        // wobble (since it's attached to the head bone). Bypass
        // Force* by writing currentState directly â€” same end state,
        // no engine-side reset hooks fire.
        if (s_fKeyHeld && s_fKeyPressWasFirst && playerCam) {
            auto& fpSlot = playerCam->cameraStates[RE::CameraState::kFirstPerson];
            if (fpSlot.get() && playerCam->currentState != fpSlot) {
                playerCam->currentState   = fpSlot;
            }
            playerCam->zoomInput      = 0.0f;
            a_this->targetZoomOffset  = 0.0f;
            a_this->currentZoomOffset = 0.0f;
            return;
        }

        // Pick the profile matching the current resolved state. ProcessEvent
        // sets state directly on transformation, so GetState() is current.
        // `settings` reference was hoisted to the top of this function.

        // Vanity disabler: the engine transitions to AutoVanityState when the
        // idleTimer exceeds autoVanityIdleTime. Keeping allowAutoVanityMode
        // false blocks that transition.
        if (playerCam && settings.disableVanityCamera) {
            playerCam->allowAutoVanityMode = false;
            playerCam->idleTimer = 0.0f;
        }
        // (A ~480-line mirror of CameraController's profile picker used to live
        // here - state switch, sub-states, shouts, target lock, dialogue, the
        // [CLIPCAM] splice, env substitution and the enemy-override splice -
        // ending in `auto& profile = localProfile;` that nothing below ever
        // read. Rotation comes from CameraController::cachedRotation, the one
        // picker that is actually applied. Deleted 2026-09-04; it had already
        // drifted from the real picker twice (no bindings, a different Base
        // rule for shouts) and every "must match CameraController" edit to it
        // was wasted work.)

        // Inject yaw orbit offset before the engine computes position.
        // Always read the spring-smoothed rotation from CameraController â€”
        // cachedRotation is set every frame to the springed rotation
        // (with the adaptive-collision blend when adaptive is active).
        // Was: conditional read that bypassed the spring when adaptive was
        // off, causing rotation slider drags + preset loads to snap.
        constexpr float kDegToRad = 3.14159265f / 180.0f;
        float rot = CameraController::GetSingleton().cachedRotation;
        float rotOffset = rot * kDegToRad;
        const bool castLocked  = StateResolver::GetSingleton().IsCastLocked();
        // Poll TDM directly. StateResolver's cache is updated in
        // CameraController::Update which runs at the END of this hook â€”
        // reading the cache here lags by one frame, so on the lock-acquire
        // frame we'd see false and skip our freeRotation.x = 0 absolute
        // write. The engine then folds the still-non-zero freeRotation.x
        // into data.angle.z (camera-offset â†’ body), snapping the body
        // by the user's pre-lock free-orbit amount. Direct poll avoids
        // the lag.
        // Suppress target-lock camera yaw tracking while a Show-Player-In-Menus
        // override is FRAMING the camera: that menu pins freeRotation.x to a
        // fixed framing value, and our own target-lock spring would otherwise
        // compete for that field every frame. The lock state itself stays
        // alive â€” TDM resumes when the menu closes and ApplyFraming releases.
        //
        // But only when it actually frames. A menu with allowCameraControl on
        // (Tween is one) never calls ApplyFraming â€” see the skip at the SPIM
        // block below â€” so there is nothing to compete with, and suppressing
        // WAS the Tween camera movement: dropping targetLocked to false with a
        // live lockAimYaw sets springingOutLock, which swaps the absolute write
        // (freeRotation.x = rotOffset + lockAimYaw) for the additive release
        // path and arms the relExtra release fade. The camera eases out of the
        // lock on open and re-acquires on close. [MENUCAM] measured it as a
        // ~0.007 rad yaw swing â€” ~7 units of camera travel at the 960-unit
        // zoom it was captured at, oscillating with the menu:
        //   tween open r33.9->34.9 | closed r34.7->29.0 | open r33.9->35.3
        // Matching this gate to the framing gate keeps the lock continuous
        // across the boundary, so nothing releases and nothing re-acquires.
        const auto& spimLock    = ShowPlayerInMenusController::GetSingleton();
        const bool  spimFraming = spimLock.IsActive() &&
                                  !spimLock.IsActiveCameraControlEnabled();
        const bool targetLocked =
            TDMIntegration::GetSingleton().IsTargetLocked() && !spimFraming;

        // Target Lock is an absolute yaw anchor: freeRotation.x is forced to
        // the profile's rotation offset every frame, so mouse input can't
        // orbit the camera around the player. Combined with TDM's
        // SetPlayerYaw, the player and camera rotate as one rigid body.
        //
        // Biased-aim: instead of the camera facing the same direction as
        // the player, we aim it at P + bias*(T-P). The per-frame yaw
        // offset for that aim is spring-smoothed in CameraController
        // so acquiring AND releasing a lock transitions instead of snapping.
        const float lockAimYaw = CameraController::GetSingleton().GetLockAimYaw();
        const bool  springingOutLock = !targetLocked && std::abs(lockAimYaw) > 0.0005f;

        // Own-system camera-yaw spring. We own freeRotation.x entirely while
        // locked. Each frame:
        //   1. Compute target heading (atan2 to TDM's current target).
        //   2. Spring our smoothed camera yaw toward that target.
        //   3. Write freeRot.x = smoothYaw - data.z so the rendered camera
        //      points at smoothYaw regardless of what TDM did to freeRot.x
        //      or data.z between our calls.
        // On the lock-acquire edge we initialise smoothYaw at the rendered
        // camera direction (data.z + freeRot.x at hook entry), so there is
        // no first-frame flash â€” the spring starts where the user was
        // looking. Target switches just change the spring's target value;
        // smooth transition from one target to the next.
        static float s_camYaw       = 0.0f;
        static float s_camYawVel    = 0.0f;
        static bool  s_camYawInited = false;
        // Switched-target latch: when the locked target changes, the camera-yaw
        // spring uses Target Switch Time (not Target Looseness) until the switch
        // duration elapses, so the slider controls the visible swing speed onto
        // the new target. A fresh acquire (no valid previous target) is NOT a
        // switch â€” it keeps the normal acquire/looseness feel.
        static RE::ActorHandle s_camYawPrevTgt;
        static bool  s_camYawSwitching = false;
        static float s_camYawSwitchT   = 0.0f;
        // Acquire latch â€” the twin of the switch latch above, for the FIRST
        // swing onto a target. Paced by the Transitions Rotation Speed slider
        // (see the acquire edge below) rather than by Target Looseness, which
        // now only governs steady tracking.
        static bool  s_camYawAcquiring  = false;
        static float s_camYawAcquireT   = 0.0f;
        static float s_camYawAcquireDur = 0.20f;
        // Distance-driven switch duration for this spring, set on the switch edge
        // from the bearing gap old->new target / Target Switch Speed (wider swing
        // = longer). Mirrors CameraController's m_lockSwitchDur for the camera-only
        // (sprint) path where the body-ease is skipped.
        static float s_camYawSwitchDur = 0.30f;
        // Vertical (pitch) switch ease. Rendered lock pitch = data.angle.x
        // (body pitch, TDM sets DIRECTLY = instant) + freeRotation.y (offset,
        // TDM InterpAngleTo-eases). On a switch the body pitch SNAPS to the new
        // target; DDC masks the snap by CAPTURING it as a per-field residual and
        // decaying that residual to zero on TOP of TDM's live pitch with a
        // smoothstep weight (1 -> 0) â€” the same fixed-residual idiom proven for
        // the yaw release ([s_relExtra] above). render = liveTDM + snap*weight,
        // NOT a lerp toward live: TDM's live value carries at FULL weight every
        // frame, and the smoothstep has ZERO slope at both ends, so the render
        // velocity equals TDM's own velocity at the START (no kick) and at the
        // hand-back (no end "fall into place"). Splitting the residual across
        // body+offset (vs all-in-offset) keeps each field within the engine's
        // pitch clamp; both land on TDM's live values -> seamless hand-back.
        // Done post-Update because pre-Update freeRotation.y is clobbered by
        // UpdateRotation.
        //
        // The vertical ease detects the target change with its OWN prev-target
        // handle, INDEPENDENT of the yaw switch latch. The yaw latch only arms
        // once CameraController's body-ease finishes (tracking resumes), but TDM
        // snaps the pitch the instant it swaps targets â€” DURING the body-ease.
        // Gating the pitch off the yaw latch left the front of the vertical move
        // unmasked (the leftover micro-jerk); detecting the swap here captures
        // the snap immediately so the WHOLE vertical move is eased.
        static float s_camPitchBody       = 0.0f;   // last rendered/steady body pitch (residual base)
        static float s_camPitchOff        = 0.0f;   // last rendered/steady offset    (residual base)
        static float s_camPitchBodyRes    = 0.0f;   // body-pitch snap captured at switch start
        static float s_camPitchOffRes     = 0.0f;   // offset snap captured at switch start
        static RE::ActorHandle s_camPitchPrevTgt;   // prev target for the vertical-ease switch detector
        static bool  s_camPitchSwitching  = false;  // vertical switch ease active
        static float s_camPitchSwitchClk  = 0.0f;   // elapsed switch time (own clock, independent of yaw)
        static float s_camPitchSwitchDur  = 0.30f;  // distance-driven switch duration (yaw gap / Switch Speed)
        // Release-extra decay: the lock-spring contribution fr = (s_camYaw -
        // body) is nonzero whenever our tracked yaw diverged from the body
        // (target off to the side / moving / just-switched). The old "clean
        // drop" let it vanish in ONE frame on unlock -> the [YAWDIAG] 'extra'
        // snap (the sideways whip). Capture it on the drop edge and decay it to
        // 0 with a smoothstep TIMER (fixed 0 endpoint -> settles; not a
        // body-chasing spring, so it can't feed the historical moving-spin).
        static float s_relExtra0  = 0.0f;
        static float s_relExtraT  = 0.0f;
        static bool  s_relExtraOn = false;
        constexpr float kRelExtraDur = 0.30f;   // seconds to fade the residual
        // Lock-release smoothing: when targetLocked flips false while the
        // yaw spring is alive (most commonly when the locked enemy dies),
        // KEEP the spring alive and retarget it to the natural rest
        // (curBody + rotOffset + lockAimYaw). The owned freeRotation.x
        // contribution `fr = s_camYaw - curBody` then decays smoothly to
        // (rotOffset + lockAimYaw), which is exactly what the unowned
        // branch writes â€” so by the time we drop ownership the visible
        // yaw is already at the no-lock value. No snap.
        static bool  s_camYawReleasing = false;

        // Lock-off: DROP our camera-yaw ownership cleanly and let the engine
        // hold the view (you mouse-look normally). TDM does not auto-rotate the
        // camera to follow movement, so ANY auto-follow we impose fights the body
        // (which TDM turns to face movement) and JERKS. The held view + working
        // mouse is the no-jerk, no-feedback answer; the looseness reset below
        // stops the position from lurching on the way out.
        if (targetLocked) {
            s_camYawReleasing = false;
        } else if (s_camYawInited) {
            s_camYawInited    = false;
            s_camYawReleasing = false;
            s_camYawAcquiring = false;
            // Ownership-drop edge: HOLD THE VIEW. While locked we own
            // freeRotation.x and hold it at the biased-aim offset (s_camYaw -
            // body), so the rendered camera world yaw = s_camYaw (framing the
            // target, off to the side). The user wants lock-off to NOT move the
            // camera at all â€” running right then unlocking shouldn't throw the
            // view to a new direction. So instead of zeroing the field (which
            // snaps the camera from the side-framed view to behind-the-body and
            // then needed a fade to mask the snap), bake the current offset
            // straight into freeRotation.x. World yaw is preserved exactly: no
            // snap, no settle. TDM then holds this view while it turns the body
            // to face movement underneath (it does not auto-rotate the camera),
            // and the player mouse-looks normally from here. No additive release
            // path runs below (rotOffset/lockAimYaw are 0 once unlocked), so
            // there's no double-bias to worry about.
            // Mounted UpdateRotation already stored the complete visible yaw
            // against TDM's last horse heading. Leave that offset for its normal
            // turn compensation; rebasing here would subtract the turn twice
            // and discard the profile/aim bias on the unlock edge.
            if (a_this->id != RE::CameraState::kMount) {
                if (auto* plDrop = RE::PlayerCharacter::GetSingleton()) {
                    float fr0 = s_camYaw - plDrop->data.angle.z;
                    while (fr0 >  3.14159265f) fr0 -= 6.28318530f;
                    while (fr0 < -3.14159265f) fr0 += 6.28318530f;
                    a_this->freeRotation.x = fr0;
                } else {
                    a_this->freeRotation.x = 0.0f;
                }
            }
            // No release fade: the held view above means there is nothing to
            // decay (the old fr0->0 fade WAS the unwanted camera motion).
            s_relExtraOn = false;
        }

        bool ownsCamera = false;
        const bool yawSpringAlive = targetLocked || (s_camYawInited && s_camYawReleasing);
        if (yawSpringAlive) {
            float targetYaw = 0.0f;
            float curBody   = 0.0f;
            bool  haveTarget = false;
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                curBody = TrackingYawReference(a_this, pl);
                float curCamera = curBody + a_this->freeRotation.x;
                RE::NiPoint3 trackingOrigin = pl->GetPosition();
                if (a_this->id == RE::CameraState::kMount) {
                    // Seed acquire from the actual previous camera, not the
                    // rider's animated yaw plus TDM's mounted offset.
                    const auto& q = a_this->rotation;
                    if (const auto heading = MountedTargetLock::Heading({q.w, q.x, q.y, q.z}))
                        curCamera = *heading - rotOffset - lockAimYaw;
                    if (auto mount = static_cast<RE::HorseCameraState*>(a_this)->horseRefHandle.get())
                        trackingOrigin = mount->GetPosition();
                }
                if (targetLocked) {
                    // Spring the camera toward the smooth target BEARING â€” during
                    // a SWITCH too. We used to GLUE the camera to the eased body
                    // while CameraController drove it (an IsLockBodyEasing branch
                    // here), but the body WOBBLES frame-to-frame during the swing
                    // (TDM plus our eased drive chasing a still-settling target),
                    // and the glue piped that wobble straight to the camera
                    // ([SWDIAG] caught body 77->63->74->... with the camera riding
                    // it â€” the "switch isn't smooth"). The bearing is smooth, and a
                    // critically-damped spring toward it rejects the body/target
                    // transients. The body still eases underneath (CameraController,
                    // for attack facing); body and camera home on the SAME target,
                    // so freeRotation.x stays bounded â€” no spin, no end-twitch.
                    {
                        auto& tdmI = TDMIntegration::GetSingleton();
                        if (auto h = tdmI.GetCurrentTarget()) {
                            // Detect a target SWITCH (previous target valid and
                            // different) and arm the switch-speed latch so the
                            // spring swings at Target Switch Speed, not looseness.
                            if (s_camYawPrevTgt && h != s_camYawPrevTgt) {
                                s_camYawSwitching = true;
                                s_camYawSwitchT   = 0.0f;
                                // Distance-driven duration: bearing gap between the
                                // old and new target as seen from the player.
                                float swAng = 0.0f;
                                if (auto op = s_camYawPrevTgt.get()) {
                                    if (auto np = h.get()) {
                                        const auto& pp2 = pl->GetPosition();
                                        const auto& o = op->GetPosition();
                                        const auto& n = np->GetPosition();
                                        const float ob = std::atan2(o.x - pp2.x, o.y - pp2.y);
                                        const float nb = std::atan2(n.x - pp2.x, n.y - pp2.y);
                                        float d = nb - ob;
                                        while (d >  3.14159265f) d -= 6.28318530f;
                                        while (d < -3.14159265f) d += 6.28318530f;
                                        swAng = std::abs(d);
                                    }
                                }
                                s_camYawSwitchDur = settings.SwitchDurationForAngle(swAng);
                            }
                            s_camYawPrevTgt = h;
                            if (auto tp = h.get()) {
                                const auto& pp = trackingOrigin;
                                const auto  tpos = tp->GetPosition();
                                const float dx = tpos.x - pp.x;
                                const float dy = tpos.y - pp.y;
                                if (dx * dx + dy * dy > 1.0f) {
                                    targetYaw  = std::atan2(dx, dy);
                                    haveTarget = true;
                                }
                            }
                        }
                    }
                } else {
                    // Lock-release path: aim the spring at the natural rest
                    // (the value freeRotation.x would receive without our
                    // ownership) so the decay lands exactly on the no-lock
                    // value with no discontinuity.
                    targetYaw  = curBody + rotOffset + lockAimYaw;
                    haveTarget = true;
                    // Clear the switch/acquire latches on release so neither
                    // paces the release decay, and so the next acquire isn't
                    // mistaken for a switch.
                    s_camYawSwitching = false;
                    s_camYawAcquiring = false;
                    s_camYawPrevTgt   = {};
                }
                if (haveTarget) {
                    if (!s_camYawInited) {
                        s_camYaw       = curCamera;
                        s_camYawVel    = 0.0f;
                        s_camYawInited = true;
                        // Fresh acquire: disarm BOTH the yaw and the pitch switch
                        // state. Their release-time clears live inside blocks that
                        // a normal lock-off skips (the yaw clear is in this
                        // yawSpringAlive block, which the spring teardown above
                        // bypasses; the pitch clear is under `if (ownsCamera)`,
                        // which is false once unlocked) â€” so a stale prev-target /
                        // latch leaks into the next lock-on. The pitch detector then
                        // fires a FALSE switch (curTgt != stale prevTgt) and snaps
                        // the camera to a stale pitch -> the upward jerk on lock-on
                        // while sprinting. A clean acquire is NEVER a switch.
                        s_camYawSwitching   = false;
                        s_camYawSwitchT     = 0.0f;
                        s_camYawPrevTgt     = {};
                        s_camPitchSwitching = false;
                        s_camPitchPrevTgt   = {};

                        // ---- ACQUIRE LATCH (2026-08-17) ----------------
                        // The swing onto a freshly locked target gets its OWN
                        // dial (Target Acquire Speed), instead of the hardcoded
                        // 0.20s it used whenever Target Looseness sat at its 0
                        // default. ("It seems hardcoded to be fast no matter
                        // what": it was. Looseness only ever reached the
                        // acquire by doubling as the TRACKING duration, which
                        // is why slowing the swing also made tracking mushy â€”
                        // one number doing two jobs.)
                        //
                        // Latched for the swing and held to ~2x the duration â€”
                        // the same shape as the switch latch below, and for
                        // the same reason: changing spring stiffness while the
                        // spring still carries velocity yanks. Once settled,
                        // reverting to the tracking duration is a no-op.
                        //
                        // (Briefly sourced from the Transitions Rotation Speed
                        // slider the same day; reverted at user request in
                        // favour of this dedicated dial. Sharing that slider
                        // also meant the acquire silently inherited whatever
                        // the rotation channel was tuned to.)
                        s_camYawAcquireDur = std::clamp(
                            SettingsManager::GetSingleton().targetLockAcquireSwingSeconds,
                            0.06f, 1.20f);
                        s_camYawAcquiring = true;
                        s_camYawAcquireT  = 0.0f;
                    }

                    const auto& settings = SettingsManager::GetSingleton();
                    // TRACKING duration â€” how tightly the camera holds a
                    // moving target once the acquire swing is done. This is
                    // all Target Looseness governs now; the acquire has its
                    // own latch above. (Field named ...AcquireSeconds until
                    // 2026-08-17, when the real acquire dial arrived and took
                    // the name it had been misusing.)
                    float sliderDur = settings.targetLockTrackSeconds;
                    if (sliderDur < 0.0f) sliderDur = 0.0f;
                    const bool mountedTracking = targetLocked && a_this->id == RE::CameraState::kMount;
                    const float duration = MountedTargetLock::TrackingDuration(
                        sliderDur > 0.0001f ? sliderDur : 0.20f, mountedTracking);
                    // Mounted tracking must have the same response at 30/60/144
                    // FPS. Acquire/switch still select their own durations below.
                    const float dt = a_this->id == RE::CameraState::kMount
                        ? std::clamp(s_mountedLockDt, 0.0f, 0.10f) : 1.0f / 60.0f;

                    // Release state (declared at block scope so the locked
                    // branch can re-arm the capture edge).
                    static bool  s_relPrev         = false;
                    static float s_releaseWorldYaw = 0.0f;
                    static float s_releaseT        = 0.0f;
                    // Switch free-look-offset ease-out state (switches only now â€”
                    // acquire uses the plain spring, not the body-ease/glue).
                    static bool  s_ezPrev = false;
                    static float s_ezFr0  = 0.0f;

                    const bool releasing = !targetLocked && s_camYawReleasing;

                    float fr;
                    if (releasing) {
                        // LOCK-OFF auto-follow: swing the camera to behind the
                        // player's MOVEMENT so it doesn't freeze on the old target.
                        // Target the MEASURED world-velocity direction (position
                        // delta) â€” NOT the body: targeting the body feeds a
                        // camera-relative-movement loop and spins, whereas the
                        // measured velocity is physics-grounded and breaks it.
                        // Starts from the locked camera yaw (no snap), eases to the
                        // movement direction, then drops. Stationary -> drop right
                        // away (vanilla: the camera stays, you mouse-look).
                        static RE::NiPoint3 s_relPrevPos{};
                        const RE::NiPoint3 ppos = pl->GetPosition();
                        if (!s_relPrev) {
                            s_relPrevPos = ppos;
                            s_releaseT   = 0.0f;
                            s_camYawVel  = 0.0f;
                            // Start the release at the ACTUAL current camera world
                            // yaw (curBody + the incoming freeRotation.x, which
                            // still carries the aim-bias offset) so frame 1 is
                            // perfectly continuous. Previously we started at the
                            // bare bearing, dropping the aim bias in one frame = the
                            // bizarre 1-frame snap.
                            float w0 = curBody + a_this->freeRotation.x;
                            while (w0 >  3.14159265f) w0 -= 6.28318530f;
                            while (w0 < -3.14159265f) w0 += 6.28318530f;
                            s_releaseWorldYaw = w0;
                        }
                        const float vx = ppos.x - s_relPrevPos.x;
                        const float vy = ppos.y - s_relPrevPos.y;
                        s_relPrevPos = ppos;
                        s_releaseT += dt;

                        bool       drop   = false;
                        const bool moving = (vx * vx + vy * vy) > 0.25f;   // >~0.5 u/frame
                        if (moving) {
                            const float velYaw = std::atan2(vx, vy);
                            float diff = velYaw - s_releaseWorldYaw;
                            while (diff >  3.14159265f) diff -= 6.28318530f;
                            while (diff < -3.14159265f) diff += 6.28318530f;
                            CriticalDampedSpringExact(s_releaseWorldYaw, s_camYawVel, s_releaseWorldYaw + diff, 4.0f / 0.45f, dt);
                            while (s_releaseWorldYaw >  3.14159265f) s_releaseWorldYaw -= 6.28318530f;
                            while (s_releaseWorldYaw < -3.14159265f) s_releaseWorldYaw += 6.28318530f;
                            if (std::abs(diff) < 0.05f || s_releaseT > 0.7f) drop = true;
                        } else if (s_relPrev) {
                            drop = true;   // genuinely stationary (not just frame 1)
                        }

                        // Drive the camera to exactly s_releaseWorldYaw (offsets
                        // cancel through the common write). Frame 1 == the incoming
                        // camera yaw -> no snap; then it eases to the move direction.
                        s_camYaw = s_releaseWorldYaw;
                        fr = (s_releaseWorldYaw - curBody) - rotOffset - lockAimYaw;
                        while (fr >  3.14159265f) fr -= 6.28318530f;
                        while (fr < -3.14159265f) fr += 6.28318530f;
                        if (drop) {
                            s_camYawInited    = false;
                            s_camYawReleasing = false;
                        }
                    } else {
                        // LOCKED: spring the camera yaw toward the target bearing.
                        // Shortest-arc unwrap so it takes the short way (170->-170).
                        float diff = targetYaw - s_camYaw;
                        while (diff >  3.14159265f) diff -= 6.28318530f;
                        while (diff < -3.14159265f) diff += 6.28318530f;
                        const float unwrappedTarget = s_camYaw + diff;

                        // Floor the spring duration: CriticalDampedSpring is
                        // explicit (forward) Euler, stable only for omega*dt < ~2.
                        // A near-zero slider gave omega = 4000 -> diverge -> the
                        // +/-pi wrap loop spun billions of times -> hard freeze.
                        // 0.08s -> omega 50, omega*dt 0.83: near-instant, stable.
                        // While swinging onto a just-switched target, drive the
                        // spring with Target Switch Time instead of Target
                        // Looseness so the slider controls the visible switch
                        // speed. Hold the switch duration for most of the swing,
                        // then SMOOTHLY blend the spring duration back to the
                        // looseness duration over the last 30% of the window. A
                        // hard revert jumped omega (spring stiffness) in one frame
                        // while the spring still had velocity -> a yank at the end
                        // of the swing. Smoothstep-blending omega keeps the
                        // acceleration continuous = no end jerk.
                        float effDur = duration;
                        // Acquire wins while it runs â€” a switch cannot be
                        // armed at the same time (a fresh acquire disarms the
                        // switch latch explicitly), so the order is only about
                        // which test reads first.
                        if (s_camYawAcquiring) {
                            const float acqDur = s_camYawAcquireDur > 0.08f
                                                     ? s_camYawAcquireDur
                                                     : 0.08f;
                            effDur = acqDur;
                            s_camYawAcquireT += dt;
                            // Hold to full settle (~2x), where reverting to the
                            // tracking stiffness is a no-op â€” no late speed-up
                            // at the end of the swing.
                            if (s_camYawAcquireT >= 2.0f * acqDur)
                                s_camYawAcquiring = false;
                        } else if (s_camYawSwitching) {
                            const float swDur = s_camYawSwitchDur > 0.08f
                                                    ? s_camYawSwitchDur
                                                    : 0.08f;
                            effDur = swDur;                 // constant stiffness during the swing
                            s_camYawSwitchT += dt;
                            // Hold the switch stiffness until the spring has fully
                            // settled (~2*swDur); reverting to the tracking
                            // stiffness then is a no-op, so there's no late
                            // speed-up or yank at the end of the swing.
                            if (s_camYawSwitchT >= 2.0f * swDur)
                                s_camYawSwitching = false;
                        }
                        const float springDur = effDur > 0.08f ? effDur : 0.08f;
                        const float omega     = 4.0f / springDur;
                        // EXACT integrator: at switch stiffness omega = 4/swDur can
                        // hit ~50, where explicit Euler (omega*dt up to 0.83) rings.
                        // The exact form is unconditionally stable, same feel.
                        CriticalDampedSpringExact(s_camYaw, s_camYawVel, unwrappedTarget, omega, dt);
                        while (s_camYaw >  3.14159265f) s_camYaw -= 6.28318530f;
                        while (s_camYaw < -3.14159265f) s_camYaw += 6.28318530f;

                        // (Removed: the switch-time body-GLUE override that hard-set
                        // s_camYaw = curBody + ezFr every frame. It piped the eased
                        // body's per-frame wobble straight into the camera â€” the
                        // "switch isn't smooth". The spring above now tracks the
                        // smooth bearing instead. IsLockBodyEasing is switch-only,
                        // so this never affected acquire.)
                        s_ezPrev = false;

                        fr = s_camYaw - curBody;
                        while (fr >  3.14159265f) fr -= 6.28318530f;
                        while (fr < -3.14159265f) fr += 6.28318530f;
                    }
                    s_relPrev = releasing;
                    if (releasing) s_ezPrev = false;   // re-arm the ease capture for the next lock

                    a_this->freeRotation.x = rotOffset + lockAimYaw + fr;

                    a_this->freeRotationEnabled = true;
                    ownsCamera = true;

                    // During release, drop ownership once fr has settled to
                    // (rotOffset + lockAimYaw). At that point the owned
                    // write equals the unowned write, so handing off is
                    // visually identical. Threshold = ~0.6Â° + ~3Â°/s.
                    if (!targetLocked) {
                        // Shortest-arc unwrap the gap, same as the spring uses.
                        // Without it, when s_camYaw and targetYaw straddle the
                        // +/-pi wrap (e.g. -0.69 vs 5.55, which are the SAME
                        // heading) the raw difference reads ~2*pi and the settle
                        // never triggers -> ownership is held forever and the
                        // camera follows the body after a kill. (Intermittent:
                        // only when the release lands across the wrap.)
                        float settleDiff = s_camYaw - targetYaw;
                        while (settleDiff >  3.14159265f) settleDiff -= 6.28318530f;
                        while (settleDiff < -3.14159265f) settleDiff += 6.28318530f;
                        const float settledPos = std::abs(settleDiff);
                        const float settledVel = std::abs(s_camYawVel);
                        if (settledPos < 0.01f && settledVel < 0.05f) {
                            s_camYawInited   = false;
                            s_camYawReleasing = false;
                        }
                    }
                }
            }
        }

        // Cleanup: if the spring never had a chance to run (no player /
        // no rest target / etc.) AND we're not locked, make sure the
        // release path isn't latched on a stale inited flag.
        if (!targetLocked && !ownsCamera) {
            s_camYawInited    = false;
            s_camYawReleasing = false;
        }

        // Release-extra decay value for THIS frame (advanced once here; the
        // post-Update block reuses the same value to balance the field).
        float relExtra = 0.0f;
        if (s_relExtraOn && !targetLocked && !ownsCamera) {
            s_relExtraT += (1.0f / 60.0f) / kRelExtraDur;
            if (s_relExtraT >= 1.0f) { s_relExtraT = 1.0f; s_relExtraOn = false; }
            const float t     = s_relExtraT;
            const float blend = t * t * (3.0f - 2.0f * t);   // smoothstep
            relExtra = s_relExtra0 * (1.0f - blend);
        } else if (targetLocked || ownsCamera) {
            s_relExtraOn = false;   // a (re)lock cancels any pending release fade
        }
        const bool applyRelExtra = relExtra != 0.0f;

        if (!ownsCamera && (rotOffset != 0.0f || targetLocked || springingOutLock || applyRelExtra)) {
            if (castLocked || targetLocked) {
                a_this->freeRotation.x = rotOffset + lockAimYaw;
            } else {
                a_this->freeRotation.x += rotOffset + lockAimYaw + relExtra;
            }
            a_this->freeRotationEnabled = true;
        }

        s_mountedLockOwnsYaw = a_this->id == RE::CameraState::kMount && targetLocked && ownsCamera;
        s_mountedLockWorldYaw = s_camYaw + rotOffset + lockAimYaw;
        s_mountedLockSteady = s_mountedLockOwnsYaw && !s_camYawAcquiring && !s_camYawSwitching;

        // Dialogue face-lock (3p) â€” runs AFTER the original Update below.
        // See the post-Update block for the implementation. ACC-style: write
        // freeRotation.y = atan2(-dz, xy) - player.angle.x, then set
        // freeRotationEnabled. Engine builds the final camera quaternion
        // from freeRotation + actor angle, so this handles pitch cleanly
        // without touching player.angle.x (which would bend the body).

        // Patch/unpatch CameraCaster â€” only for disableCameraCollision.
        // Adaptive collision keeps engine collision active.
        UpdateCameraCasterPatch();

        // Sneak+staff cast body rotation. Engine routes mouse-x to
        // freeRotation.x for staff (EnchantmentItem) casters but to
        // data.angle.z for spell (SpellItem) casters â€” see
        // [[reference-staff-vs-spell-rotation]]. The longshot fix:
        // force iState=10 (the graph variable TDM uses to detect
        // staff-firing) so TDM's existing face-crosshair path fires
        // even in sneak posture, which gives smooth rotation with
        // proper turn-in-place if the animation pack supports it.
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* main = RE::Main::GetSingleton();
            if (player && player->IsSneaking() && (!main || !main->freezeTime) &&
                StateResolver::GetSingleton().GetLiveState() == CameraState::Staves) {
                for (const bool leftHand : { true, false }) {
                    auto* equipped = player->GetEquippedObject(leftHand);
                    auto* staff = equipped ? equipped->As<RE::TESObjectWEAP>() : nullptr;
                    auto* caster = player->GetMagicCaster(leftHand
                        ? RE::MagicSystem::CastingSource::kLeftHand
                        : RE::MagicSystem::CastingSource::kRightHand);
                    if (!staff || !staff->IsStaff() || !caster || !caster->currentSpell) continue;
                    const auto state = caster->state.get();
                    if (state == RE::MagicCaster::State::kCharging ||
                        state == RE::MagicCaster::State::kReady ||
                        state == RE::MagicCaster::State::kCasting) {
                        player->SetGraphVariableInt("iState", 10);
                        break;
                    }
                }
            }
        }

        // [DLG-EXIT-ZOOM] Ease the engine zoom from the (close) dialogue value to
        // the gameplay zoom on dialogue EXIT, set BEFORE the original Update so the
        // engine composes THIS frame's camera distance from the eased value (writing
        // it in the render hook was too late). The dialogue holds the engine zoom at
        // its close value so the parent state's zoom does not leak into the framing;
        // on exit ApplyZoom jumps it to gameplay. Eased over the user's dialogue
        // Position blend duration so it stays smooth at every transition speed.
        {
            const bool dlgOpenZ = RE::UI::GetSingleton() &&
                                  RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
            static bool  sZWasOpen  = false;
            static bool  sZActive   = false;
            static float sZFrom     = 0.0f;
            static float sZTo       = 0.0f;
            static float sZDur      = 0.3f;
            static float sZElapsed  = 0.0f;
            static float sZInDlg    = 0.0f;
            static std::chrono::steady_clock::time_point sZLast{};
            static bool  sZLastInit = false;
            if (dlgOpenZ) {
                sZInDlg = a_this->currentZoomOffset;   // last in-dialogue zoom (held close)
            }
            if (sZWasOpen && !dlgOpenZ) {
                // Seed the endpoint from the Categories profile; the loop below
                // then re-reads the ACTUAL applied zoom every frame (see there).
                auto* gp = CameraController::GetSingleton().GetLastCategoriesProfile();
                sZTo    = -0.2f + (gp ? gp->zoom : 11.0f) * 0.01f;
                sZFrom  = sZInDlg;
                const float mul = (std::max)(
                    0.05f, SettingsManager::GetSingleton().dialogueMulPosition);
                sZDur   = (std::max)(0.05f, SettingsManager::DialogueBlendDuration(mul));
                sZElapsed  = 0.0f;
                sZLastInit = false;
                // Always arm. The old gate compared the from/to pair computed
                // HERE and skipped the ease when they looked equal â€” but the
                // "to" it compared against was a guess (see below), so a wrong
                // guess that happened to match the dialogue zoom skipped the
                // ease entirely and let ApplyZoom slam the real value in one
                // frame. Arming unconditionally costs nothing when there is
                // genuinely no distance to travel: the lerp is a no-op.
                sZActive   = true;
            }
            sZWasOpen = dlgOpenZ;
            if (sZActive) {
                // RE-TARGET EVERY FRAME on the zoom the camera is actually
                // sitting at.
                //
                // This used to aim at a value derived from GetLastCategoriesProfile()
                // â€” the raw Categories entry. ApplyZoom does not use that: it
                // uses the RESOLVED profile, which is a different pointer the
                // moment the player is locked on (Target Lock owns the frame),
                // standing somewhere with a Location Override, indoors, or in
                // range of an enemy override. Whenever those disagreed the ease
                // ran to the wrong number and ApplyZoom's real value took over
                // the instant it finished â€” a step at the END of the exit, which
                // is exactly "it still snaps when exiting dialogue".
                //
                // The published value is one frame stale (this hook runs before
                // CameraController::Update), which does not matter: the exit
                // window holds the zoom channel constant, and on the single
                // frame it could differ the ease weight is still ~0.
                {
                    bool zoomValid = false;
                    const float live = CameraController::GetLastAppliedEngineZoom(zoomValid);
                    if (zoomValid && std::isfinite(live)) sZTo = live;
                }
                const auto now = std::chrono::steady_clock::now();
                float dt = sZLastInit
                    ? std::chrono::duration<float>(now - sZLast).count() : (1.0f / 60.0f);
                sZLast = now; sZLastInit = true;
                dt = std::clamp(dt, 0.0001f, 0.1f);
                // Proximity-paced like every other dialogue clock: the exit
                // starts right next to the speaker's face, so the pull-out
                // begins gently and picks up as the camera clears them
                // (dlgPaceMul relaxes back toward 1 in CameraController).
                sZElapsed += dt * std::clamp(
                    SettingsManager::GetSingleton().dlgPaceMul, 0.2f, 1.0f);
                const float t = std::clamp(sZElapsed / sZDur, 0.0f, 1.0f);
                const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                const float ez = std::lerp(sZFrom, sZTo, s);
                // Pin BOTH current AND target to the eased value. The engine eases
                // currentZoomOffset toward targetZoomOffset INSIDE the original
                // Update; with target left at the gameplay value (slammed there by
                // the previous frame's ApplyZoom), that internal lerp pulled current
                // back off our ramp every frame -> the residual 1p->3p exit snap.
                // Writing both gives the original Update nothing to chase, so it
                // renders exactly the eased value. ApplyZoom (CameraController::Update,
                // which runs AFTER _original) re-asserts the gameplay zoom for storage,
                // but _original has already consumed the eased value this frame, so
                // that write never reaches the screen.
                a_this->currentZoomOffset = ez;
                a_this->targetZoomOffset  = ez;
                if (t >= 1.0f) sZActive = false;
            }
        }

        // ----- THE DISMOUNT EDGE GUARD IS GONE. DO NOT REBUILD IT. ---------
        //
        // It stashed the mounted framing on every mounted Update and wrote it
        // back onto the FIRST third-person frame, pre-Update. Built to close a
        // measured one-frame 275-unit lunge — but that lunge was the gap
        // between the engine composing at posOffset.y ~0 and CameraController's
        // posOffset.y CARRY landing a frame later. The carry was deleted; the
        // guard was not, and it kept running for a day with a comment that
        // said "REMOVED" directly above it.
        //
        // What it cost, measured 2026-08-25 17:35:12:
        //
        //   engine's first 3p frame   distRoot=122.88  zoom c=-0.100
        //                             act=(50.0, 0.0, -20.0)   <- ALREADY RIGHT
        //   after the guard fired     distRoot=361.91  zoom c=-0.200
        //   DDC then travelled back   h = 361.9 -> 318.9 -> 240.4 -> 185.0
        //                                 -> 158.1 -> 143.6 -> 134.5 -> 123.2
        //   with fov riding along      80.0 ->  95.7 -> 103.4 -> ... -> 110.2
        //
        // A 240-unit rush at the player in ~0.15s with a 30 degree FOV
        // widening on top of it — a dolly zoom. User: "it goes right up to the
        // back of the player's head, then it re-zooms to the correct settings."
        // Every attempt to smooth this edge was smoothing a move that only
        // existed because of this block.
        //
        // THE ENGINE HANDS THE CAMERA BACK CORRECTLY. Read posOffsetActual on
        // the first third-person frame before writing anything to this edge.
        // See [[dismount-transition-skyward]].

        // ===== [HORSEKILL] SKYRIM'S HORSE CAMERA NEVER RUNS =================
        //
        // User, 2026-08-27: *"i dont understand why you can't just make
        // horseback use our camera for transitions. Its as easy as making
        // skyrims camera not touch it at all."*
        //
        // They are right, they proposed it days ago, and I argued them out of
        // it on TDM-compatibility grounds. Everything since has been an attempt
        // to reconcile DDC's camera with a vanilla horse camera that did not
        // need to be in the picture:
        //
        //   - kMount pivots on the HORSE, so the dismount swaps anchors and no
        //     framing channel can cover that (measured 105u + 81u, 78 vertical);
        //   - kMount carries a ~-300 posOffset.y addend DDC has to animate;
        //   - the state change drags in a kPCTransition lerp with its own clock.
        //
        // ALL THREE ARE PROPERTIES OF THAT STATE. Delete the state from the
        // picture and all three stop existing — there is nothing left to
        // transition except the profile, which is the ordinary channel path.
        // Mounting and dismounting become exactly sheathed->melee.
        //
        // This is the same shape as InstallFurnitureCameraKill below (a port of
        // Ersh's NoFurnitureCamera), which keeps the player in kThirdPerson
        // rather than letting the engine pose a furniture camera. Furniture had
        // a single call site to patch; the mount does not, but it does not need
        // one — the horse camera composes through THIS hook, so forcing the
        // third-person state before CallOriginalUpdate means HorseCameraState
        // never composes a frame at all.
        //
        // The state resolver keys off player->IsOnMount(), NOT the camera
        // state, so the Horseback profile still resolves and every horseback
        // setting still applies. The camera is simply DDC's the whole time.
        //
        // WHAT THIS DELIBERATELY GIVES UP, so it is a decision and not a
        // surprise: the engine's mounted pull-back is gone with the state that
        // owned it, so mounted distance now comes from the Horseback profile's
        // own zoom — a setting, which is the entire point of the request. It
        // will read CLOSE until that slider is raised, because that profile is
        // currently all zeros.
        // ===== [MOUNTANCHOR] KEEP THE CAMERA ON THE PLAYER =================
        //
        // THE FIRST FIX ON THIS EDGE BUILT FROM A MEASURED CORRECTION RATHER
        // THAN AN ASSUMED ONE. 2026-08-27 17:40 capture, dismount edge:
        //
        //   horse  (18134.3, -11108.8)      player (18112.1, -11218.6)
        //   player - horse = (-22.2, -109.8)   |112.0|
        //   camera delta at +1 = (-26.7, -107.9)   |111.1|
        //
        // The camera translates by EXACTLY (player - horse) on the frame the
        // state swaps. That is the anchor coming off the horse and landing on
        // the player, and it is 112 units in one frame. The user said this
        // three rounds ago; every mechanism built before this one was animating
        // offsets measured FROM the thing that was moving.
        //
        // So pre-apply it: while mounted, shift the composed camera by
        // (player - mount) every frame. The camera is then already
        // player-anchored, and when kThirdPerson takes over and composes around
        // the player natively the position is unchanged. Nothing to cover.
        //
        // WHY THE PREVIOUS ATTEMPT'S VECTOR WAS WRONG: it used
        // Get3D()->world.translate for BOTH actors, which gave
        // (-4.8, -62.8, -0.9) len 63 — the wrong pair, and it showed, taking
        // the edge from 112 down to only 83. The pair that matches the measured
        // camera delta is the PLAYER'S POSITION against the MOUNT'S NODE, which
        // is exactly what the probe columns that matched were reading.
        //
        // AND WHY `DISTM` DID NOT CATCH IT: camera-to-horse distance stayed
        // flat across the edge (405.4 -> 407.2) even though the anchor moved,
        // because the displacement was nearly perpendicular to the horse->camera
        // line. **A distance test cannot see a tangential anchor move.** Only
        // the vector comparison is decisive. Do not re-derive this from DISTM.
        //
        // THE DELTA IS LATCHED, and it has to be: GetMount() stops answering a
        // couple of frames BEFORE the camera state swaps (visible in the probe
        // as the mnt= column freezing at -2). A live-only delta would go stale
        // to zero exactly at the edge and hand the jump straight back.
        // ===== DISABLED 2026-08-27 — NOT DEMONSTRABLY DOING ANYTHING ========
        //
        // The delta is finally computed correctly (logged growing 63.9 -> 79.7
        // -> 96.8 -> 111.8, exactly the predicted 112 at the edge) and the edge
        // jump did NOT move: dW=110.52 at +1, against 111-113 before the shift
        // existed. A 112-unit pre-cancellation that changes the edge by ~1 is
        // not cancelling anything, so either the write is not reaching the
        // rendered camera or the composition is not what this assumes.
        //
        // It also DRAGS THE MOUNTED CAMERA: the shift ramps 63 -> 112 across
        // the dismount animation, which is an uneased camera move of its own —
        // exactly the "bullshit transition on top" this whole saga is about.
        //
        // THE METHOD ERROR THAT LET THIS RUN FOUR ROUNDS: dW was being compared
        // ACROSS DIFFERENT DISMOUNTS. Mounted DIST at frame -1 over four
        // captures was 377.4 / 429.2 / 426.7 / 485.7 — different spots,
        // different framing, different horse speed. "112 became 83, so it
        // helped" was two unrelated dismounts, not an effect.
        //
        // NEXT STEP IS A CONTROLLED TEST, NOT ANOTHER MECHANISM: log
        // cameraRoot->world immediately before and after this write, on the
        // same frame, and compare against the probe's rendered position. That
        // answers "does writing local here move the rendered camera at all"
        // — which has been ASSUMED since the first attempt and never checked.
        if (false && a_this && a_this->id == RE::CameraState::kMount) {
            auto* playerCam = RE::PlayerCamera::GetSingleton();
            auto* player    = RE::PlayerCharacter::GetSingleton();
            if (playerCam && playerCam->cameraRoot && player) {
                // LATCH THE MOUNT'S POSITION, NOT THE DELTA. Latching the delta
                // was measured wrong on 2026-08-27 17:45: it logged len=63.5,
                // the value while SEATED, and the edge stayed at dW=113.49.
                //
                // The delta is not a constant. It is ~63 while riding and grows
                // to ~112 as the dismount animation slides the player off the
                // horse, and 112 is the number that has to be cancelled at the
                // swap. GetMount() stops answering about two frames BEFORE the
                // state changes, so a latched delta freezes at the seated value
                // and never sees the part that matters — whereas the PLAYER's
                // position stays live the whole way through the animation.
                //
                // So cache only the horse's position and recompute against the
                // live player every frame. The mounted camera then follows the
                // player as they step off, and the state swap has nothing left
                // to jump across.
                static RE::NiPoint3 sMountPos{};
                static bool         sMountPosValid = false;
                RE::NiPointer<RE::Actor> mountRef;
                if (player->GetMount(mountRef) && mountRef) {
                    if (auto* mt3d = mountRef->Get3D()) {
                        sMountPos      = mt3d->world.translate;
                        sMountPosValid = true;
                    }
                }
                RE::NiPoint3 sDelta{};
                bool         sDeltaValid = false;
                if (sMountPosValid) {
                    const RE::NiPoint3 d = player->GetPosition() - sMountPos;
                    if (d.Length() < 400.0f) { sDelta = d; sDeltaValid = true; }
                }
                if (sDeltaValid) {
                    auto& local = playerCam->cameraRoot->local;
                    local.translate.x += sDelta.x;
                    local.translate.y += sDelta.y;
                    local.translate.z += sDelta.z;
                    // Log the SHIFT AS IT CHANGES, not once. A single line at
                    // mount time reports the seated value and hides exactly the
                    // growth that matters — that is how the previous build
                    // looked correct in the log while doing nothing at the edge.
                    static float sLastLogged = -1000.0f;
                    static int   sLogCount   = 0;
                    const float  len = sDelta.Length();
                    if (sLogCount < 40 && std::abs(len - sLastLogged) > 15.0f) {
                        sLastLogged = len;
                        ++sLogCount;
                        spdlog::debug("[MOUNTANCHOR] shift=({:.1f},{:.1f},{:.1f}) len={:.1f}",
                                     sDelta.x, sDelta.y, sDelta.z, len);
                    }
                }
            }
        }

        // ===== [HORSEKILL] BROKE DIRECTIONAL MOVEMENT. REVERTED. ============
        //
        // User: *"it broke directional movement."*
        //
        // **kMount IS NOT ONLY A CAMERA.** The movement controller reads the
        // active camera state to decide how to map stick/WASD input while
        // mounted, so removing the state removed the horse's steering mapping
        // with it. That is a GAMEPLAY regression and strictly worse than the
        // framing artifact it was trying to remove.
        //
        // This does NOT invalidate the user's instruction ("make Skyrim's
        // camera not touch it at all") — it invalidates deleting the STATE as
        // the way to do it. The state has to keep running for movement; what
        // must stop is its CAMERA OUTPUT. Those are separable and this attempt
        // conflated them.
        //
        // BEFORE THE NEXT ATTEMPT: get `dPiv` vs `dOff` off a real dismount
        // (the [MOUNTEXIT] row prints both now). If the edge jump is in dPiv
        // the pivot has to be held continuous across the swap — and held on
        // BOTH sides, because an offset that exists only while mounted is
        // itself an edge jump (that was the [MOUNTANCHOR] failure). If it is in
        // dOff it was never an anchor problem at all. Do not build either fix
        // until that column has been read.

        // ----- [MOUNTENTER-Y] ARM THE ENTRY EASE BEFORE THE ENGINE COMPOSES --
        //
        // HorseCameraState's Update (vfunc 0x3) routes here too, so this is the
        // one place DDC gets to touch the mount state's framing BEFORE
        // CallOriginalUpdate builds the first mounted frame.
        //
        // Mounting cuts posOffset.y from 0 to the engine's ~-300 addend in a
        // single frame while side, height, zoom and FOV all ease — a ~300-unit
        // outward jump the instant the state swaps, and the reason the ~158
        // degree entry orbit starts far enough out to keep catching collision.
        // Zeroing it here means the first composed mounted frame sits where the
        // camera already was on foot, and CameraController's mMountY channel
        // carries it out on the same spring as every other channel.
        //
        // Doing this from CameraController::Update instead does NOT work — that
        // runs after the engine's Update, so one frame at the full -300 renders
        // first and the ease then reads as a 282-unit collapse behind it.
        // Measured 19:10; it was strictly worse than the jump it replaced.
        // ----- AND THE SAME SEED ON THE WAY OUT ----------------------------
        //
        // Both horse edges need exactly one special thing and it is the same
        // thing: the frame the state swaps, the engine composes with the NEW
        // state's framing before any DDC write can land, so one frame renders
        // at the wrong distance. On the mount that read as a 300-unit outward
        // slam; on the dismount it is the "right up to the back of the
        // player's head" frame, because the third-person state's posOffset.y
        // is 0 and the mounted pull-back has simply vanished.
        //
        // THIS IS A SEED, NOT A GUARD, AND THE DIFFERENCE IS THE WHOLE POINT.
        // The deleted [MOUNTEXIT-GUARD] wrote the mounted framing back and
        // there was no channel to carry it out again, so DDC's zoom spring
        // dollied 240 units inward behind it. mMountY is that channel now — it
        // holds the mounted pull-back and eases to 0 in the same Weight group,
        // on the same sliders, as side/height/zoom/fov. So this write only
        // decides WHERE THE MOVE STARTS. It cannot become a move of its own,
        // and per [[horse-edges-use-the-normal-machinery]] the seed and the
        // frame it lands on are the one legitimate non-clock exception.
        // (The DISMOUNT half of this seed is NOT here. It cannot be: this hook
        // is not called for the third-person state until the frame after
        // ForceThirdPerson, so a seed written here is always one frame late and
        // the engine has already composed at y=0. It lives at the abort site in
        // HookedTransitionUpdate, where transitionFrom still holds the mounted
        // framing. Two builds proved the point before it was believed.)
        {
            static bool sPrevWasMountUpd = false;
            const bool  isMountUpd = a_this && (a_this->id == RE::CameraState::kMount);
            if (isMountUpd && !sPrevWasMountUpd) {
                auto* mountTps = static_cast<RE::ThirdPersonState*>(a_this);
                CameraController::NotifyMountEntry(mountTps->posOffsetExpected.y);
                mountTps->posOffsetExpected.y = 0.0f;
                mountTps->posOffsetActual.y   = 0.0f;
            }
            sPrevWasMountUpd = isMountUpd;
        }

        // [TWEENX] capture straddling the engine's own pass. Third person
        // only — the Tween open the user is reproducing keeps camState 9
        // (SPIM holds 3p), and mount/dragon compose from a different anchor.
        TweenXRec txr{};
        bool      txHave = false;
        {
            auto* txCam = RE::PlayerCamera::GetSingleton();
            auto* txPly = RE::PlayerCharacter::GetSingleton();
            if (txCam && txCam->cameraRoot && txPly && a_this &&
                a_this->id == RE::CameraState::kThirdPerson) {
                const RE::NiPoint3 pp   = txPly->GetPosition();
                const float        byaw = txPly->data.angle.z;
                const float sy = std::sin(byaw), cy = std::cos(byaw);
                const auto  latOf = [&](const RE::NiPoint3& w) {
                    return (w.x - pp.x) * cy - (w.y - pp.y) * sy;
                };
                const auto  yawOf = [](const RE::NiQuaternion& q) {
                    const float qfx = 2.0f * (q.x * q.y - q.w * q.z);
                    const float qfy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
                    return std::atan2(qfx, qfy) * 57.2957795f;
                };
                txr.inTransR = latOf(a_this->translation);
                txr.inCollR  = latOf(a_this->collisionPos);
                txr.inRootR  = latOf(txCam->cameraRoot->world.translate);
                txr.inRotYaw = yawOf(a_this->rotation);
                txr.inSide   = a_this->posOffsetActual.x;
                txr.inExpX   = a_this->posOffsetExpected.x;
                txr.inExpZ   = a_this->posOffsetExpected.z;
                TweenXShoulder(txCam, pp, txr.inShoulder, txr.inBack);
                TweenXFillUnk(a_this, pp, cy, sy, txr.inU);
                txr.inZoom   = a_this->currentZoomOffset;
                txr.inFrX    = a_this->freeRotation.x;
                txr.bodyYaw  = byaw * 57.2957795f;
                txr.camState = static_cast<int>(a_this->id);
                txHave = true;
            }
        }

        const bool restoredMenuAnimation = RestorePendingMenuAnimation(a_this);
        TweenCameraTrace::Sample("3p-engine-before");
        s_mountedRotationState = a_this;
        CallOriginalUpdate(a_this, a_nextState);
        s_mountedRotationState = nullptr;
        TweenCameraTrace::Sample("3p-engine-after");
        RememberCameraAnimation(a_this);
        TweenCameraTrace::SampleGuard(restoredMenuAnimation, 0);

        if (txHave) {
            auto* txCam = RE::PlayerCamera::GetSingleton();
            auto* txPly = RE::PlayerCharacter::GetSingleton();
            if (txCam && txCam->cameraRoot && txPly) {
                const RE::NiPoint3 pp   = txPly->GetPosition();
                const float        byaw = txPly->data.angle.z;
                const float sy = std::sin(byaw), cy = std::cos(byaw);
                const auto  latOf = [&](const RE::NiPoint3& w) {
                    return (w.x - pp.x) * cy - (w.y - pp.y) * sy;
                };
                const auto  yawOf = [](const RE::NiQuaternion& q) {
                    const float qfx = 2.0f * (q.x * q.y - q.w * q.z);
                    const float qfy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
                    return std::atan2(qfx, qfy) * 57.2957795f;
                };
                txr.outTransR = latOf(a_this->translation);
                txr.outCollR  = latOf(a_this->collisionPos);
                txr.outRootR  = latOf(txCam->cameraRoot->world.translate);
                txr.outRotYaw = yawOf(a_this->rotation);
                txr.outSide   = a_this->posOffsetActual.x;
                txr.outExpX   = a_this->posOffsetExpected.x;
                txr.outExpZ   = a_this->posOffsetExpected.z;
                TweenXShoulder(txCam, pp, txr.outShoulder, txr.outBack);
                TweenXFillUnk(a_this, pp, cy, sy, txr.outU);
                txr.outZoom   = a_this->currentZoomOffset;
                txr.outFrX    = a_this->freeRotation.x;
                txr.valid      = true;
                txr.end3pRoot  = s_tweenXEnd3pRoot;
                txr.end3pTrans = s_tweenXEnd3pTrans;
                txr.ucpRoot    = s_tweenXUcpRoot;
                txr.ucpTrans   = s_tweenXUcpTrans;
                txr.engRoot    = s_tweenXEngRoot;
                txr.engTrans   = s_tweenXEngTrans;

                auto* txUi = RE::UI::GetSingleton();
                const bool tweenOpen = txUi && txUi->IsMenuOpen("TweenMenu");
                txr.menuOpen = tweenOpen;

                static bool sTweenXPrevOpen = false;
                const bool  rising = tweenOpen && !sTweenXPrevOpen;
                sTweenXPrevOpen = tweenOpen;

                if (s_tweenXPost > 0) {
                    // Live tail of an armed window.
                    TweenXPrint("post", s_tweenXFrame++, txr);
                    --s_tweenXPost;
                } else if (rising && s_tweenXWindows < kTweenXMaxWindows) {
                    // The open frame. Flush the pre-roll oldest-first so the
                    // steady baseline sits directly above it, then this frame,
                    // then a short tail for the walk-back.
                    ++s_tweenXWindows;
                    spdlog::debug("[TWEENX] ==== TweenMenu OPEN (window {}/{}) ====",
                                 s_tweenXWindows, kTweenXMaxWindows);
                    // The live shoulder pair sits on the [SHOULDER] line at
                    // load now — these are INI settings ("...:Camera"), not
                    // GameSettings, which is why the first attempt to read
                    // them here printed nan for all five.
                    for (int i = 0; i < kTweenXRing; ++i) {
                        const auto& pr = s_tweenXRing[(s_tweenXHead + i) % kTweenXRing];
                        if (pr.valid) TweenXPrint("pre ", i - kTweenXRing, pr);
                    }
                    s_tweenXFrame = 0;
                    TweenXPrint("OPEN", s_tweenXFrame++, txr);
                    s_tweenXPost = 45;   // the walk is ~37 frames — cover all of it
                } else {
                    // Steady: keep the ring warm.
                    s_tweenXRing[s_tweenXHead] = txr;
                    s_tweenXHead = (s_tweenXHead + 1) % kTweenXRing;
                }
            }
        }

        // ----- [MOUNTANCHOR] THE CAMERA STAYS ON THE PLAYER ----------------
        //
        // User, 2026-08-27, after four builds that all missed it: *"I can only
        // assume that its because horseback unattaches the camera from the
        // player. it shouldn't."* They are right, and DDC's own probe has been
        // printing it the whole time — `camState=10 anchor=mount`.
        //
        // While mounted the engine pivots the camera on the HORSE. Every
        // framing quantity this mod animates — posOffset, zoom, mMountY, FOV —
        // is an offset RELATIVE TO THAT PIVOT, so none of them can cover the
        // pivot itself moving. That is why five days of work on the framing
        // channels kept removing a real mover and leaving the artifact at the
        // same size.
        //
        // MEASURED 2026-08-27 16:58, camera world position across a dismount:
        //
        //   -1  cam=(18440.7, -11017.9, -4441.2)  dW=  0.00
        //   +1  cam=(18471.5, -11118.5, -4440.9)  dW=105.21  <- pivot leaves the horse
        //   +2  cam=(18451.5, -11129.9, -4363.0)  dW= 81.27  <- lands on the player, +78 in Z
        //   +3  dW=26.21, then 21, 18, 15, 14, 13 ...        <- the channels, already smooth
        //
        // The player moved 7 units on the first of those frames and 0.1 on the
        // second: ~186 units of camera travel in two frames, 78 of it straight
        // up. THAT is the snap.
        //
        // So do not unattach it. The horse's root and the rider's root differ
        // by that offset, and the third-person state pivots on the rider's, so
        // adding (player - mount) while mounted puts the pivot where it already
        // is on foot and where it will still be after the dismount. The edge
        // then has nothing left to discontinue, and both horse edges become
        // pure profile changes carried by the ordinary channels — which is what
        // "the same mechanism as the rest of the transitions" has meant every
        // time it was said.
        //
        // cameraRoot->local is the channel every other 3p translation layer in
        // this mod writes (noise texture, head bob, Repulse) and the engine
        // rebuilds it during composition each frame, so this is additive within
        // a frame and cannot accumulate across frames.
        //
        // HORSE ONLY. Dragon riding keeps the engine's anchor — that camera has
        // eleven sub-states of its own and is not part of this complaint.
        // ----- IT WAS BUILT, MEASURED AND REVERTED THE SAME HOUR ------------
        //
        // The attempt added (player->Get3D() - mount->Get3D()) to
        // cameraRoot->local while mounted. The log answered it in one line:
        //
        //   [MOUNTANCHOR] pivot shifted by (-4.8, -62.8, -0.9) len=63.0
        //
        // **Z is -0.9.** The discontinuity being chased is ~78 units VERTICAL,
        // so those two nodes are not the pivot pair — they are both actor roots
        // at ground level and the 62.8 is just the rider sitting fore/aft of
        // the horse's origin.
        //
        // And the SHAPE was wrong even if the vector had been right: a constant
        // offset applied only while mounted is removed in one frame at the
        // dismount, so it manufactures its own edge jump. Measured immediately —
        // frame +1 moved 83 units, 76 of them in +Y, i.e. mostly this offset
        // being handed back. A snap added on top of the snap.
        //
        // DO NOT RE-ADD AN ANCHOR SHIFT UNTIL THE REAL PIVOT PAIR IS IDENTIFIED
        // by measurement rather than assumed from node names. The user's
        // diagnosis ("horseback unattaches the camera from the player") is not
        // in question — this attempt at implementing it was.

        // Suppress the engine's built-in sprint FOV pulse. Originally we
        // only suppressed for sprint+weapons-drawn (matching SmoothCam's
        // comment), but the user reported the brief-zoom-in artifact on
        // SHEATHED sprintâ†’stop too. Either the engine writes the same
        // pulse during sheathed sprint (just less obvious for SmoothCam
        // users with default profile values), or there's a related write
        // that mirrors it. Suppress on any sprint to cover both cases.
        // Grace period catches the engine's ramp-down after sprint exit.
        {
            static auto sLastSprintTime = std::chrono::steady_clock::time_point{};
            const auto  now             = std::chrono::steady_clock::now();
            bool        sprinting       = false;
            bool        weaponsDrawn    = false;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* actorState = player->AsActorState();
                    actorState && actorState->IsSprinting()) {
                    sprinting        = true;
                    weaponsDrawn     = actorState->IsWeaponDrawn();
                    sLastSprintTime  = now;
                }
            }
            const float graceSec = std::chrono::duration<float>(now - sLastSprintTime).count();
            const bool  inGrace  = graceSec < 1.5f;
            // Beast forms (2026-09-05, "werewolf sheathed has some weird fov
            // movement ... adjusts when I move while sheathed"): the werewolf's
            // sheathed locomotion runs at sprint speed (the 12:11 log has the
            // character controller at spd=1.22 with sprint=false), so the
            // engine's anim-driven FOV pulse plays without IsSprinting() ever
            // going true, and the gate here never zeroed it. Suppress it for
            // the whole beast form; the INFO line below says whether it was
            // ever non-zero there.
            const bool beast = StateResolver::GetSingleton().IsWerewolf() ||
                               StateResolver::GetSingleton().IsVampireLord();
            if (sprinting || inGrace || beast) {
                // Two writes are needed:
                //   1. thirdPersonFOVControl->local.translate.z â€” what the
                //      engine writes during sprint+weapons.
                //   2. The GLOBAL FOV offset float â€” what the renderer
                //      actually reads when applying FOV. SmoothCam writes
                //      both; we were only writing #1, hence the residual
                //      pulse remained visible. Address Library ID 527997
                //      (SE) / 414942 (AE), same as SmoothCam.
                static REL::Relocation<float*> sFovOffsetPtr{ RELOCATION_ID(527997, 414942) };

                float preGlobal = 0.0f;
                if (auto* g = sFovOffsetPtr.get()) {
                    preGlobal = *g;
                    *g = 0.0f;
                }
                float preZ = 0.0f;
                if (auto* fovNode = a_this->thirdPersonFOVControl) {
                    preZ = fovNode->local.translate.z;
                    fovNode->local.translate.z = 0.0f;
                }
                static int sLogCount = 0;
                if ((preZ != 0.0f || preGlobal != 0.0f) && sLogCount < 60) {
                    ++sLogCount;
                    spdlog::debug(
                                "[FOVPULSE] sprint={} weapons={} beast={} grace={:.2f}s nodeZ={:.3f} global={:.3f} (zeroed)",
                                sprinting ? 1 : 0, weaponsDrawn ? 1 : 0, beast ? 1 : 0,
                                graceSec, preZ, preGlobal);
                }
            }
        }

        if (ownsCamera) {
            // Own-system: leave freeRot.x at the smoothed value we wrote
            // pre-Update so the engine's camera matrix calc and any later
            // GetRotation/GetTranslation calls render at our smooth yaw.
            // No subtract â€” we OWN the field while locked.

            // Vertical switch ease (POST-Update â€” pre-Update freeRotation.y is
            // clobbered by the engine's UpdateRotation, same as dialogue). On a
            // target change we capture the body/offset SNAP as a fixed residual
            // and add it back on top of TDM's live pitch, decaying it to zero
            // with a zero-slope smoothstep (1 -> 0): no start kick, no end
            // fall-into-place, no finite-difference velocity to saturate. See
            // the static-decl comment for why the switch is detected HERE (own
            // prev-target) rather than off the yaw latch.
            if (auto* plyP = RE::PlayerCharacter::GetSingleton()) {
                const float bodyTdm = plyP->data.angle.x;     // TDM's instant body pitch
                const float offTdm  = a_this->freeRotation.y; // TDM's eased camera offset

                // Real inter-frame time for the vertical switch clock below.
                // It used to advance by a hardcoded 1/60 per frame, so at any
                // framerate != 60 the pitch ease ran on a different wall-clock
                // than the (real-dt) horizontal swing â€” vertical finished early
                // (>60fps) or lagged (<60fps), settling out of step with the
                // turn. Measured per-frame here so the two stay locked together.
                static std::chrono::steady_clock::time_point s_pitchLastTp{};
                const auto  pitchNowTp = std::chrono::steady_clock::now();
                float pitchDt = 1.0f / 60.0f;
                if (s_pitchLastTp.time_since_epoch().count() != 0)
                    pitchDt = std::chrono::duration<float>(pitchNowTp - s_pitchLastTp).count();
                s_pitchLastTp = pitchNowTp;
                pitchDt = std::clamp(pitchDt, 0.0001f, 0.1f);

                RE::ActorHandle curTgt;
                if (targetLocked) {
                    if (auto h = TDMIntegration::GetSingleton().GetCurrentTarget()) curTgt = h;
                }
                const auto& settingsP = SettingsManager::GetSingleton();

                if (curTgt && s_camPitchPrevTgt && curTgt != s_camPitchPrevTgt) {
                    // Switch (valid previous target, changed): capture the snap.
                    // s_camPitchBody/Off hold the last rendered pitch (the
                    // pre-switch view), so the difference from TDM's just-snapped
                    // value IS the snap. Re-capturing from the current rendered
                    // pitch makes a mid-ease re-target ease from where we are.
                    s_camPitchBodyRes   = s_camPitchBody - bodyTdm;
                    s_camPitchOffRes    = s_camPitchOff  - offTdm;
                    s_camPitchSwitchClk = 0.0f;
                    s_camPitchSwitching = true;
                    // Distance-driven duration: same yaw bearing gap old->new
                    // target as the horizontal swing, so the vertical ease settles
                    // in step with it instead of over a fixed time.
                    float pAng = 0.0f;
                    if (auto op = s_camPitchPrevTgt.get()) {
                        if (auto np = curTgt.get()) {
                            const auto& pp3 = plyP->GetPosition();
                            const auto& o = op->GetPosition();
                            const auto& n = np->GetPosition();
                            const float ob = std::atan2(o.x - pp3.x, o.y - pp3.y);
                            const float nb = std::atan2(n.x - pp3.x, n.y - pp3.y);
                            float d = nb - ob;
                            while (d >  3.14159265f) d -= 6.28318530f;
                            while (d < -3.14159265f) d += 6.28318530f;
                            pAng = std::abs(d);
                        }
                    }
                    s_camPitchSwitchDur = settingsP.SwitchDurationForAngle(pAng);
                }
                if (curTgt) s_camPitchPrevTgt = curTgt;
                if (!targetLocked) {            // unlock: the next acquire is not a switch
                    s_camPitchPrevTgt   = {};
                    s_camPitchSwitching = false;
                }

                if (s_camPitchSwitching) {
                    s_camPitchSwitchClk += pitchDt;   // real frame time (was fixed 1/60)
                    const float swDurP = s_camPitchSwitchDur > 0.08f ? s_camPitchSwitchDur : 0.08f;
                    const float u = std::clamp(s_camPitchSwitchClk / swDurP, 0.0f, 1.0f);
                    const float w = 1.0f - (u * u * (3.0f - 2.0f * u));
                    plyP->data.angle.x     = bodyTdm + s_camPitchBodyRes * w;
                    a_this->freeRotation.y = offTdm  + s_camPitchOffRes  * w;
                    s_camPitchBody = plyP->data.angle.x;   // base for a mid-ease re-switch
                    s_camPitchOff  = a_this->freeRotation.y;
                    if (u >= 1.0f) s_camPitchSwitching = false;
                } else {
                    // Not switching: track TDM's live pitch so the pre-switch
                    // value is primed for the next residual capture.
                    s_camPitchBody = bodyTdm;
                    s_camPitchOff  = offTdm;

                }
            }
        } else if (rotOffset != 0.0f || targetLocked || springingOutLock || applyRelExtra) {
            if (castLocked || targetLocked) {
                a_this->freeRotation.x = 0.0f;
            } else {
                a_this->freeRotation.x -= rotOffset + lockAimYaw + relExtra;
            }
        }

        // ----- DISMOUNT PITCH: MEASURE, NEVER WRITE ------------------------
        //
        // This block used to EASE the dismount pitch step by subtracting a
        // decaying carry from freeRotation.y. Added in 591e691 to hide an ~8
        // degree step. REMOVED 2026-08-24: it left the camera pointing at the
        // sky. User: "it instantly snapped skyward."
        //
        // The [MOUNTEXIT] dense trace of 17:20 is unambiguous:
        //
        //   +1        freeRot.y=-0.143  rend=+0.286   <- the ease's first write
        //   +2        freeRot.y=+0.036  rend=+0.107
        //   +4        freeRot.y=+0.428  rend=-0.285
        //   +8        freeRot.y=+1.072  rend=-0.929
        //   +18       freeRot.y=+1.747  rend=-1.604
        //   +19..+90  freeRot.y=+1.714  rend=-1.571   PINNED
        //
        // rend = bodyX - freeRotation.y, and it sits at exactly -pi/2 for the
        // rest of the capture: aimed STRAIGHT UP, and it never comes back.
        // 1.714 = bodyX + pi/2 — the engine's own pitch clamp, saturated.
        //
        // Three things pin this on the ease and not on the engine's handover:
        //   - the runaway starts the frame AFTER the ease's first write, and
        //     freeRotation.x never moves at all. Only the axis we touched.
        //   - it settles in ~18 frames = 4/omega at omega 15, which is the
        //     ease's own time constant and no transition slider's.
        //   - the symptom arrived WITH 591e691. Before it the complaint was a
        //     forward dive; after it, skyward.
        //
        // The mechanism is a FEEDBACK LOOP, not a sign error — I checked the
        // sign three times and it is right. Our subtraction totals at most
        // 0.143 rad and decays, yet freeRotation.y rose 1.857 rad, 13x our
        // whole contribution and in the OPPOSITE direction. The engine reads
        // this field back after we write it, so a write here is an injected
        // velocity that the engine's own smoothing integrates, and it runs
        // away until the clamp catches it. Same class as the
        // [[session-2026-08-14]] lesson: do not write a field an engine call
        // is going to read back and integrate.
        //
        // DO NOT REBUILD THIS as a "smaller" or "slower" ease. The loop gain
        // belongs to the engine, so a gentler kick only takes longer to
        // saturate. An 8 degree pitch step is worth living with; a camera
        // stuck facing the sky is not. If the step ever does need hiding, it
        // has to be done somewhere the engine does not read back — the body
        // angle, or a channel DDC already owns outright.
        //
        // The MEASUREMENT stays. It costs nothing, it never writes, and it
        // keeps reporting how big the raw engine step actually is.
        {
            static bool  sPrevWasMountCam = false;
            static float sPrevRendPitch   = 0.0f;

            const bool isMountCam = (a_this->id == RE::CameraState::kMount);
            float rendPitch = 0.0f;
            if (auto* plP = RE::PlayerCharacter::GetSingleton())
                rendPitch = plP->data.angle.x - a_this->freeRotation.y;

            if (sPrevWasMountCam && !isMountCam) {
                float d = sPrevRendPitch - rendPitch;
                while (d >  3.14159265f) d -= 6.28318530f;
                while (d < -3.14159265f) d += 6.28318530f;
                if (std::abs(d) > 0.02f) {          // ~1.1 degrees
                    spdlog::debug("[MOUNTEXIT] rendered pitch stepped {:.1f} deg on dismount "
                                 "({:.3f} -> {:.3f} rad) — measured only, NOT eased "
                                 "(easing it drove the camera to the pitch clamp)",
                                 d * 180.0f / 3.14159265f, sPrevRendPitch, rendPitch);
                }
            }
            sPrevWasMountCam = isMountCam;
            sPrevRendPitch   = rendPitch;
        }

        // Dialogue face-lock (3p) â€” written post-Update because the engine's
        // UpdateRotation (vtable slot 0x0E) runs inside _originalThirdPersonUpdate
        // and clobbers any freeRotation we set pre-Update.
        //
        // Every reference mod that does this (ACC RotateCamera, SmoothCam
        // FaceToFaceDialogue, TDM LookAtTarget) lerps the camera from its
        // pre-dialogue rotation to the aim target over ~0.6â€“1.25s. None of
        // them snap. Our previous "snap on frame 16" strategy was exposing
        // three frame-1 anomalies â€” stale cameraRoot world-translate, a
        // transient data.angle.x from the run-to-idle lean anim, and an
        // unsettled NPC head bone â€” which combined into the floor-flash on
        // moving dialogue entry. A time-based blend absorbs all three: on
        // the capture frame we skip writing entirely (scene graph settles
        // for one frame), then each subsequent frame scalar grows toward 1
        // and we interpolate from startYaw/startPitch to desiredYaw/desiredPitch.
        // Gate on MenuTopicManager's own state, not IsMenuOpen("Dialogue Menu").
        // When the player re-activates an NPC mid-goodbye the engine defers
        // menu construction until the voice line ends, but the speaker
        // handles are populated immediately â€” starting the face-lock now
        // makes the camera track through the delay instead of staying
        // "bugged out" at whatever angle the player was looking when they
        // hit Activate.
        // Show Player In Menus framing runs at the END of this hook
        // (after CameraController::Update) so it wins against the
        // per-state profile FOV/posOffset writes. See the call right
        // after CameraController::Update below.

        // (A mount exclusion was added here 2026-08-25 to stop "dialogue leaked
        // into horseback" and REVERTED the same evening: it did not stop a
        // leak, it removed dialogue on horseback altogether. User: "you ruined
        // dialogue on horseback." Dialogue from the saddle is supposed to work.
        // Whatever the leak was, it is not "the face-lock runs while mounted" —
        // that is the intended behaviour. Do not re-add a blanket mount gate.)
        if (settings.dialogueEnabled) {
            if (auto* speaker = GetActiveDialogueSpeaker()) {
                auto* player     = RE::PlayerCharacter::GetSingleton();
                auto* playerCam  = RE::PlayerCamera::GetSingleton();
                if (speaker && player && playerCam && playerCam->cameraRoot) {
                        // Compute the NPC-face aim target every frame. Uses
                        // the actual head bone when present (seated NPCs on
                        // thrones etc. have elevated heads that +120 misses).
                        RE::NiPoint3 facePos;
                        bool         haveFacePos = false;
                        auto* npcRoot  = speaker->Get3D();
                        auto* headNode = npcRoot ? npcRoot->GetObjectByName("NPC Head [Head]") : nullptr;
                        // Dragons/creatures lack a "NPC Head [Head]" bone; fall back
                        // to the engine's race-independent cached head node. That's
                        // an Actor-only call â€” talking-activator speakers (the Statue
                        // of Mara, shrines) aren't Actors, so guard it and let them
                        // fall through to the world-bound aim below. Mirrors the 1p
                        // face-lock so the statue locks the same in third person.
                        if (!headNode) {
                            if (auto* spkActor = speaker->As<RE::Actor>()) {
                                if (auto* mh = spkActor->GetMiddleHighProcess()) headNode = mh->headNode;
                            }
                        }
                        if (headNode) {
                            facePos     = headNode->world.translate;
                            haveFacePos = true;
                        } else if (npcRoot) {
                            // Headless target (statue): aim at the model's world-bound
                            // CENTER biased up, not a fixed 120u above the ref ORIGIN
                            // (which lands in empty air partway up a tall statue).
                            const auto& wb = npcRoot->worldBound;
                            if (wb.radius > 1.0f) {
                                facePos     = wb.center;
                                facePos.z  += wb.radius * 0.35f;
                                haveFacePos = true;
                            }
                        }
                        if (!haveFacePos) {
                            facePos    = speaker->GetPosition();
                            facePos.z += 120.0f;
                        }

                        // ---- "Look At The Player" (Dialogue) --------------
                        // For the player's half of the conversation, frame
                        // THEIR face instead of the speaker's.
                        //
                        // The aim is solved the ORDINARY way â€” point the
                        // camera at the target from wherever the camera
                        // actually is â€” because CameraController has already
                        // moved the rig to orbit the speaker. That is what
                        // makes the framing an exact mirror rather than an
                        // approximation of one: it is the same solve as the
                        // forward shot with the two subjects swapped, so the
                        // subject lands in the same place on screen, side
                        // offset and all.
                        //
                        // (Solving this way is only safe BECAUSE the rig
                        // orbits the speaker. Aiming a camera at the very
                        // point it orbits is a feedback loop â€” the solved yaw
                        // picks up a constant atan2(side, distance) every
                        // frame and the camera walks a circle forever. With
                        // the camera orbiting one person and aiming at the
                        // other, the two are a conversation-width apart and
                        // the solve converges instead.)
                        //
                        // The target sweeps through the same half-turn the
                        // anchor does, about the same midpoint, so the two
                        // stay that width apart for the whole move rather than
                        // meeting in the middle.
                        //
                        // The BODY keeps facing the speaker throughout â€” you
                        // are still talking to them; it's the camera that
                        // moved.
                        const float reverseBlend =
                            CameraController::GetSingleton().GetDialogueReverseBlend();
                        const bool   haveReverse = reverseBlend > 0.0001f;
                        RE::NiPoint3 aimPos      = facePos;
                        if (haveReverse) {
                            RE::NiPoint3 playerFace = player->GetPosition();
                            playerFace.z += 120.0f;
                            if (auto* pRoot = player->Get3D()) {
                                if (auto* pHead = pRoot->GetObjectByName("NPC Head [Head]")) {
                                    playerFace = pHead->world.translate;
                                }
                            }
                            // Swept between the two HEAD positions, not the two
                            // origins, so that at blend 0 the target is exactly
                            // the speaker's head â€” the same point the ordinary
                            // face-lock uses â€” and the sweep starts from where
                            // the camera was already looking instead of a few
                            // units off it.
                            const float mx  = (playerFace.x + facePos.x) * 0.5f;
                            const float my  = (playerFace.y + facePos.y) * 0.5f;
                            const float phi = reverseBlend * 3.14159265f;
                            const float cphi = std::cos(phi), sphi = std::sin(phi);
                            // Rotate the speaker's side of the midpoint round
                            // to the player's, mirroring the anchor sweep in
                            // CameraController.
                            const float vx = facePos.x - mx, vy = facePos.y - my;
                            aimPos.x = mx + vx * cphi - vy * sphi;
                            aimPos.y = my + vx * sphi + vy * cphi;
                            // Height crosses straight over, matching the
                            // anchor's vertical, so the aim ends exactly on
                            // the player's head rather than near it.
                            aimPos.z = facePos.z + (playerFace.z - facePos.z) * reverseBlend;
                        }

                        // Keep the BODY facing non-Actor speakers (the Statue of
                        // Mara and other talking ACTIVATORS). The engine keeps the
                        // player oriented toward an Actor dialogue partner every
                        // frame â€” you orbit an NPC with your chest toward them â€” but
                        // activators get none of that, so in 3p the body turns to
                        // follow movement and you stop facing the statue. Drive the
                        // body yaw through TDM (the same path as the combat face-
                        // assist) so it eases on and TDM keeps locomotion coherent;
                        // a bare SetRotationZ would moonwalk. Release while
                        // sprinting: a held facing that fights the movement line
                        // makes the character run in place (see the combat-assist
                        // note in CameraController). Dialogue-end release lives in
                        // TickDialogueAimInit â€” this block is skipped once the
                        // speaker clears.
                        {
                            auto&      tdmFace   = TDMIntegration::GetSingleton();
                            auto*      aState    = player->AsActorState();
                            const bool sprinting = aState && aState->IsSprinting();
                            const bool isActor   = speaker->As<RE::Actor>() != nullptr;
                            const bool wantBodyFace =
                                !isActor &&
                                tdmFace.IsAvailable() &&
                                !tdmFace.IsTargetLocked() &&
                                !sprinting;
                            const RE::NiPoint3 pPos = player->GetPosition();
                            const float bodyYaw =
                                std::atan2(facePos.x - pPos.x, facePos.y - pPos.y);
                            if (wantBodyFace) {
                                // Intent flag for the orphan watchdog â€” set BEFORE
                                // (re)acquiring so a one-frame gap between a release
                                // and our re-acquire isn't mistaken for a leak.
                                s_dlgFaceYawTaken = true;
                                // Source of truth is TDM's ACTUAL ownership, not a
                                // sticky latch: CameraController's unconditional
                                // lock/combat-off releases can drop the shared
                                // yawOwned out from under us (log showed acquire then
                                // release at dialogue start, then SetPlayerYaw no-ops
                                // forever). Re-acquire whenever we don't hold it.
                                if (!tdmFace.HasYawControl()) tdmFace.RequestYawControl(2.0f);
                                if (tdmFace.HasYawControl()) tdmFace.SetPlayerYaw(bodyYaw);
                            } else if (s_dlgFaceYawTaken) {
                                tdmFace.ReleaseYawControl();
                                s_dlgFaceYawTaken = false;
                            }
                        }

                        const auto& camPos = playerCam->cameraRoot->world.translate;

                        // DIALOGUE PROXIMITY PACE ("more gentle the closer it
                        // gets to the npc or the player"). One number, written
                        // every dialogue frame: how close the camera is to the
                        // nearest conversation subject, mapped to a clock
                        // multiplier. Every dialogue transition clock advances
                        // by dtÃ—this â€” the position blend in CameraController,
                        // the aim blend below, the exit-zoom ease â€” so a move
                        // sweeping past a face eases off and a wide move runs
                        // at the sliders' full speed. This is also what makes
                        // the speed sliders matter MORE for distant presets:
                        // a close-up preset spends its whole travel inside the
                        // gentle zone, so the slider's raw pace barely reaches
                        // it, while a wide preset runs the sliders untouched.
                        {
                            RE::NiPoint3 playerHead = player->GetPosition();
                            playerHead.z += 120.0f;
                            const auto d2 = [&](const RE::NiPoint3& p) {
                                const float ex = p.x - camPos.x;
                                const float ey = p.y - camPos.y;
                                const float ez = p.z - camPos.z;
                                return ex * ex + ey * ey + ez * ez;
                            };
                            const float dMin =
                                std::sqrt((std::min)(d2(facePos), d2(playerHead)));
                            // Full speed beyond 220u, floor 0.35Ã— inside 60u,
                            // smoothstep between â€” no seam at either edge.
                            float u = std::clamp((dMin - 60.0f) / 160.0f, 0.0f, 1.0f);
                            u = u * u * (3.0f - 2.0f * u);
                            settings.dlgPaceMul = 0.35f + 0.65f * u;
                        }

                        const float dx = aimPos.x - camPos.x;
                        const float dy = aimPos.y - camPos.y;
                        const float dz = aimPos.z - camPos.z;
                        const float xy = std::sqrt(dx * dx + dy * dy);
                        if (xy > 20.0f) {
                            float desiredYaw   = std::atan2(dx, dy);
                            float desiredPitch = std::atan2(-dz, xy);
                            constexpr float kMaxPitch = 0.785f;
                            if (desiredPitch >  kMaxPitch) desiredPitch =  kMaxPitch;
                            if (desiredPitch < -kMaxPitch) desiredPitch = -kMaxPitch;

                            // double through the subtraction â€” see comment
                            // on s_dialogueAimStartTime for the precision
                            // reason. Cast to float only at the point of
                            // dividing by blend duration below.
                            const double now = DialogueNowSeconds();

                            // Slew-rate limit on desiredPitch/Yaw to filter
                            // the ~10 Hz NPC head-bone / cameraRoot tick
                            // staircase out of the cubic blend's input.
                            // desiredPitch = atan2(-dz, xy) is recomputed
                            // from the live NPC head world.translate, which
                            // updates at the animation graph rate (~10 Hz),
                            // not per frame. Per-frame jumps can be 0.3-
                            // 0.5 rad â†’ blend output staircases â†’ compY â†’
                            // posOffset.y stutter the user reported
                            // 2026-05-27. EMA at alpha=0.20 only absorbed
                            // ~20% of each step; this slew limit caps the
                            // per-frame change so the input is a continuous
                            // ramp. Rate of 3 rad/s = ~172Â°/s â€” catches up
                            // 0.5 rad in ~165 ms, fast enough to keep face-
                            // lock responsive but slow enough that single-
                            // frame tick jumps don't propagate.
                            if (s_dialogueAimInit) {
                                // A SPRING, not a rate clamp.
                                //
                                // This used to be a hard per-frame step limit of
                                // 3 rad/s. It filtered the head-bone staircase,
                                // but a clamp is a zero-order hold on velocity:
                                // whenever the target moved faster than the cap
                                // â€” which is most of a dialogue entry, since the
                                // camera itself is travelling and desiredYaw is
                                // recomputed from the live camera position every
                                // frame â€” the aim ran at a flat 3 rad/s and then
                                // hit the target and stopped in ONE frame. A
                                // constant velocity dropping to zero with no
                                // deceleration is the definition of a snap, and
                                // it landed exactly at the end of the move: the
                                // camera glided in and then locked into place.
                                //
                                // It was worse mid-conversation. s_dialogueAimInit
                                // is only cleared on dialogue open/close, so on a
                                // preset switch the smootherstep below is already
                                // pinned at t=1 and this clamp is the ONLY thing
                                // shaping the aim â€” every mid-dialogue framing
                                // change was a pure linear pan that stopped dead.
                                //
                                // A critically damped spring low-passes the same
                                // ~10 Hz tick (that is all the clamp was ever for)
                                // while approaching asymptotically, so velocity
                                // goes to zero smoothly and there is no arrival
                                // frame to feel. Omega is chosen so the peak
                                // speed on a head-bone-sized step is comparable
                                // to the old cap; the reverse shot keeps its
                                // faster setting for the same reason it had a
                                // higher ceiling before â€” the half-turn is our
                                // own deliberate sweep, not jitter to reject.
                                const bool sweeping = reverseBlend > 0.0001f &&
                                                      reverseBlend < 0.9999f;
                                constexpr float kAimOmega      = 6.5f;   // ~150 ms
                                constexpr float kAimOmegaSweep = 11.0f;  // ~90 ms
                                // Proximity gentling on the spring too: near a
                                // face the aim settles softer. Partial (not the
                                // full pace) so the spring keeps enough
                                // authority to filter the ~10 Hz head-bone tick.
                                const float omegaAim = (sweeping ? kAimOmegaSweep : kAimOmega) *
                                                       (0.55f + 0.45f * settings.dlgPaceMul);
                                // dt computed earlier in the else branch
                                // but only assigned to s_dialogueLastFrameTime.
                                // Recompute locally so the spring is real-time,
                                // not per-frame. Subtract in double, then cast â€”
                                // delta is <100ms, safely representable in float.
                                float slewDt = static_cast<float>(now - s_dialogueLastFrameTime);
                                if (slewDt < 0.0f)  slewDt = 0.0f;
                                if (slewDt > 0.1f)  slewDt = 0.1f;

                                // Spring on the UNWRAPPED target: take the short
                                // way round once, then let the spring work in a
                                // continuous space so it can never chase the
                                // long way through a wrap.
                                float yDiff = desiredYaw - s_dialogueSmoothedTargetYaw;
                                while (yDiff >  3.14159265f) yDiff -= 6.28318530f;
                                while (yDiff < -3.14159265f) yDiff += 6.28318530f;
                                CriticalDampedSpringExact(
                                    s_dialogueSmoothedTargetYaw, s_dialogueAimYawVel,
                                    s_dialogueSmoothedTargetYaw + yDiff, omegaAim, slewDt);
                                while (s_dialogueSmoothedTargetYaw >  3.14159265f)
                                    s_dialogueSmoothedTargetYaw -= 6.28318530f;
                                while (s_dialogueSmoothedTargetYaw < -3.14159265f)
                                    s_dialogueSmoothedTargetYaw += 6.28318530f;

                                CriticalDampedSpringExact(
                                    s_dialogueSmoothedTargetPitch, s_dialogueAimPitchVel,
                                    desiredPitch, kAimOmega, slewDt);

                                desiredYaw   = s_dialogueSmoothedTargetYaw;
                                desiredPitch = s_dialogueSmoothedTargetPitch;
                            }

                            {
                                if (!s_dialogueAimInit) {
                                    s_dialogueAimStartYaw           = player->data.angle.z + a_this->freeRotation.x;
                                    s_dialogueAimStartPitch         = player->data.angle.x;
                                    s_dialogueAimStartFreeY         = a_this->freeRotation.y;
                                    s_dialogueAimStartTime          = now;
                                    s_dialogueLastFrameTime         = now;
                                    s_dialogueSmoothedTargetYaw     = desiredYaw;
                                    s_dialogueSmoothedTargetPitch   = desiredPitch;
                                    s_dialogueAimYawVel             = 0.0f;
                                    s_dialogueAimPitchVel           = 0.0f;
                                    s_dialogueAimWarpT              = 0.0f;
                                    s_dialogueAimInit               = true;

                                    // Only rotate the player and force the default
                                    // idle pose when standing AND weapon sheathed.
                                    // IdleForceDefaultState unconditionally tears
                                    // the actor out of the sitting/mounted pose â€”
                                    // without the seated gate, initiating dialogue
                                    // while seated forces the player to instantly
                                    // stand up. The weapon-drawn gate is the same
                                    // idea: forcing default idle while a weapon is
                                    // equipped wipes the combat-ready anim state
                                    // (CombatIdle / drawn-weapon root) while the
                                    // equipment stays present, leaving the anim
                                    // graph in a state where attack/sheathe inputs
                                    // have no valid transition target â€” the
                                    // character is bricked. The SetRotationZ call
                                    // is paired with the IdleForce notification
                                    // (rotating without an animation refresh
                                    // causes the moonwalk bug â€” see
                                    // reference_dialogue_moonwalk_bug.md), so both
                                    // are gated together: when the conditions
                                    // don't allow IdleForce, skip rotation too
                                    // and let the camera face-lock alone carry
                                    // the conversation framing.
                                    bool playerSeated = false;
                                    if (auto* aState = player->AsActorState()) {
                                        const auto ss = aState->GetSitSleepState();
                                        playerSeated =
                                            ss != RE::SIT_SLEEP_STATE::kNormal;
                                    }
                                    bool weaponDrawn = false;
                                    if (auto* aState = player->AsActorState()) {
                                        weaponDrawn = aState->IsWeaponDrawn();
                                    }
                                    // Face the SPEAKER, never the solved aim.
                                    // With the reverse shot active the aim
                                    // yaw points back at the player, and
                                    // rotating the body to it would spin them
                                    // away from the person they're talking to.
                                    if (!playerSeated && !weaponDrawn) {
                                        const RE::NiPoint3 pOriginBody = player->GetPosition();
                                        const float bodyFaceYaw = std::atan2(
                                            facePos.x - pOriginBody.x,
                                            facePos.y - pOriginBody.y);
                                        player->SetHeading(haveReverse ? bodyFaceYaw : desiredYaw);
                                        player->NotifyAnimationGraph(
                                            RE::BSFixedString("IdleForceDefaultState"));
                                    }

                                    spdlog::info(
                                        "DialogueFaceLock (3p) capture: startYaw={:.3f} targetYaw={:.3f} startPitch={:.3f} targetPitch={:.3f}",
                                        s_dialogueAimStartYaw, desiredYaw,
                                        s_dialogueAimStartPitch, desiredPitch);
                                } else {
                                    float dt = static_cast<float>(now - s_dialogueLastFrameTime);
                                    if (dt < 0.0f)  dt = 0.0f;
                                    if (dt > 0.1f)  dt = 0.1f;
                                    s_dialogueLastFrameTime = now;
                                    // Advance the warped clock â€” see the static's
                                    // comment. Clamped so a stale pace can never
                                    // stall the blend outright.
                                    s_dialogueAimWarpT +=
                                        dt * std::clamp(settings.dlgPaceMul, 0.2f, 1.0f);
                                }

                                // Cubic blend with live target. Per-axis
                                // durations scaled by user's dialogue speed
                                // sliders â€” at default mul=1.0 the blend is
                                // 0.22s (PRE-NIGHTMARE baseline); at lowest
                                // mul=0.05 it stretches to 4.4s. Was
                                // hardcoded constant, which meant lowering
                                // the sliders only slowed the position
                                // springs while the face-lock pitch still
                                // snapped fast â€” the user's "snaps downward
                                // very fast even at lowest setting" complaint.
                                // Shared perceptual mapping -- must match the position
                                // lockstep in CameraController (SettingsManager::
                                // DialogueBlendDuration) so position + face-lock finish
                                // together; a desync reads as "fast then crawl".
                                const float pitchMul = (std::max)(0.05f, settings.dialogueMulPitch);
                                const float yawMul   = (std::max)(0.05f, settings.dialogueMulRotation);
                                // Aim uses the SAME mapping as Position so the pitch
                                // settles together with the position blend. The camera
                                // height (compZ in CameraController) composes this
                                // pitch, so a slower Aim left the height drifting after
                                // the position settled (the force-3p-from-1p snap).
                                const float kPitchBlendSecs = SettingsManager::DialogueBlendDuration(pitchMul);
                                const float kYawBlendSecs   = SettingsManager::DialogueBlendDuration(yawMul);

                                // Subtract in double (now and startTime are
                                // both double), divide in double (promotes
                                // float kBlendSecs), then cast â€” result is
                                // in [0,1] where float precision is fine.
                                // Warped clock, not wall-clock â€” paced by the
                                // camera's proximity to the subjects, exactly
                                // like the position blend's elapsed.
                                const float tPitchLin = std::clamp(
                                    s_dialogueAimWarpT / kPitchBlendSecs, 0.0f, 1.0f);
                                const float tYawLin = std::clamp(
                                    s_dialogueAimWarpT / kYawBlendSecs, 0.0f, 1.0f);
                                // Quintic smootherstep: 6t^5 - 15t^4 + 10t^3.
                                // Matches the position chanBlend curve in
                                // CameraController.cpp:1665 â€” both velocity AND
                                // acceleration are zero at the endpoints, so the
                                // camera enters dialogue as one coordinated motion
                                // rather than position-quintic + rotation-cubic
                                // with different start velocities. Cubic ease-out
                                // (1-(1-t)^3) had velocity = 3/duration at t=0,
                                // producing a brick-wall rotation start while
                                // position was barely moving â€” the entry "jerk".
                                // Quintic smootherstep, jerk-free at both endpoints.
                                // The fast-speed "crawl" is solved by the shorter
                                // fast-end blend duration, not the curve.
                                const float tPitch =
                                    tPitchLin * tPitchLin * tPitchLin *
                                    (tPitchLin * (tPitchLin * 6.0f - 15.0f) + 10.0f);
                                const float tYaw =
                                    tYawLin * tYawLin * tYawLin *
                                    (tYawLin * (tYawLin * 6.0f - 15.0f) + 10.0f);

                                float yawRange = desiredYaw - s_dialogueAimStartYaw;
                                while (yawRange >  3.14159265f) yawRange -= 6.28318530f;
                                while (yawRange < -3.14159265f) yawRange += 6.28318530f;
                                const float cameraYaw = s_dialogueAimStartYaw + yawRange * tYaw;

                                float yawOffset = cameraYaw - player->data.angle.z;
                                while (yawOffset >  3.14159265f) yawOffset -= 6.28318530f;
                                while (yawOffset < -3.14159265f) yawOffset += 6.28318530f;
                                a_this->freeRotation.x      = yawOffset;
                                a_this->freeRotationEnabled = true;

                                player->data.angle.x = s_dialogueAimStartPitch +
                                    (desiredPitch - s_dialogueAimStartPitch) * tPitch;

                                // freeRotation.y (mouse-pitch decay) follows
                                // pitch blend â€” it's pitch-axis input.
                                a_this->freeRotation.y = s_dialogueAimStartFreeY * (1.0f - tPitch);
                            }
                        }
                    }
                }
            }

        // Dialogue position override runs in CameraController::Update â€” it
        // writes posOffsetExpected.x/y/z (the ThirdPersonState "over-shoulder"
        // fields at 0x5C/0x60/0x64, the same channel ACC/IACC use).
        // Nothing to do here.


        static bool logged = false;
        if (!logged) {
            spdlog::info("Diet Dr Camera: ThirdPersonState::Update hook fired");
            logged = true;
        }

        auto* camera = RE::PlayerCamera::GetSingleton();
        if (camera) {
            CameraController::GetSingleton().Update(camera);
        }

        // Show Player In Menus â€” must run AFTER CameraController so its
        // FOV/posOffset writes don't get clobbered by the per-state
        // profile pipeline. ApplyFraming pins worldFOV, body yaw, and
        // freeRotation/posOffset to the user-configured menu values.
        if (auto& spim = ShowPlayerInMenusController::GetSingleton(); spim.IsActive() && camera) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                // Skip per-frame framing writes when the active menu
                // has allowCameraControl on â€” user input must own
                // freeRotation/posOffset. The initial framing position
                // was written once from OnMenuOpenChange on open/swap.
                if (!spim.IsActiveCameraControlEnabled()) {
                    spim.ApplyFraming(camera, a_this, player);
                }
            }
            // Resolve any deferred restore from a close event whose
            // sibling-menu open never showed up.
            spim.Tick();
        }

        // Quick Tune suspends TDM to preserve its target while the overlay
        // owns R3. A fixed pitch pair prevented drift on open, but could not
        // frame that target after editing Height/Zoom: closing restored TDM's
        // aim and visibly changed the result. Preserve the opening aim point,
        // then recompute its camera pitch as the live rig moves.
        {
            static TargetLockPreview preview;
            static bool held = false, preValid = false;
            static float body = 0.0f, pitch = 0.0f, preBody = 0.0f, preFreeY = 0.0f;
            static float openingPitch = 0.0f;
            static TargetLockPreview::Point preCamera{}, preTarget{};
            static TargetLockPreview::Point openingTarget{};
            static std::array<float, 7> preFraming{}, openingFraming{};
            static RE::ActorHandle heldTarget, preTargetHandle;
            static auto lastTick = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;
            if (elapsed > 0.25f) preValid = false;
            const float previewDt = std::clamp(elapsed, 0.0f, 0.05f);
            const bool qtClaim = MenuUI::HasQuickTuneDMClaim();
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto handle = TDMIntegration::GetSingleton().GetCurrentTarget();
            const auto target = handle.get();
            const bool valid = targetLocked && player && target && camera && camera->cameraRoot &&
                               a_this->id == RE::CameraState::kThirdPerson;
            const auto point = [](const RE::NiPoint3& p) {
                return TargetLockPreview::Point{p.x, p.y, p.z};
            };
            if (valid) {
                const auto cameraPoint = point(camera->cameraRoot->world.translate);
                const auto targetPoint = point(target->GetLookingAtLocation());
                std::array<float, 7> framing{};
                const bool haveFraming = CameraController::GetLastAppliedFraming(
                    framing[0], framing[1], framing[2], framing[3], framing[4]);
                framing[5] = CameraController::GetSingleton().cachedRotation;
                framing[6] = CameraController::GetSingleton().LivePitchOffset();
                if (qtClaim) {
                    if (!held || handle != heldTarget) {
                        const bool usePre = preValid && preTargetHandle == handle;
                        body = usePre ? preBody : player->data.angle.x;
                        pitch = body - (usePre ? preFreeY : a_this->freeRotation.y);
                        openingPitch = pitch;
                        openingTarget = usePre ? preTarget : targetPoint;
                        openingFraming = usePre ? preFraming : framing;
                        const bool calibrated = preview.Capture(usePre ? preCamera : cameraPoint,
                                                                usePre ? preTarget : targetPoint, pitch);
                        heldTarget = handle;
                        held = true;
                        spdlog::debug("[QTPITCH] preview opened pitch={:.4f} calibrated={} target={:08X}",
                                     pitch, calibrated, target->GetFormID());
                    }
                    const bool geometryChanged = haveFraming && framing != openingFraming;
                    const bool targetMoved = targetPoint.x != openingTarget.x ||
                        targetPoint.y != openingTarget.y || targetPoint.z != openingTarget.z;
                    // Before any edit, preserve the exact old hold. A camera
                    // sample taken while the engine zeroes freeRotation must
                    // not itself count as the user changing the framing.
                    const auto desired = geometryChanged || targetMoved
                        ? preview.Pitch(cameraPoint, targetPoint) : std::optional<float>{openingPitch};
                    if (desired) {
                        // Framing sliders already use the live camera springs;
                        // follow their geometry smoothly on wall time while the
                        // attack/noise envelopes remain held for tuning.
                        pitch += (1.0f - std::exp(-8.0f * previewDt)) * (*desired - pitch);
                    }
                    player->data.angle.x = body;
                    a_this->freeRotation.y = body - pitch;
                } else {
                    if (held)
                        spdlog::debug("[QTPITCH] preview closed held={:.4f} live={:.4f}",
                                     pitch, player->data.angle.x - a_this->freeRotation.y);
                    held = false;
                    preBody = player->data.angle.x;
                    preFreeY = a_this->freeRotation.y;
                    preCamera = cameraPoint;
                    preTarget = targetPoint;
                    preFraming = framing;
                    preTargetHandle = handle;
                    preValid = haveFraming;
                }
            } else {
                held = false;
                preValid = false;
            }
        }

        // ================= [MENUCAM] =====================================
        //
        // The probe that replaces [MENUDIAG]. That one logged ANGLES ONLY,
        // reported them stable across a Tween open, and on that basis I
        // called the camera stable â€” while the user was still watching it
        // move. A camera can orbit, dolly and change height without its
        // FACING changing at all, so angle-only data could never have seen
        // it. Everything that can move the view is captured here:
        //
        //   * ANGLES: body yaw/pitch, freeRotation, rendered NiCamera yaw/pitch
        //   * POSITION: posOffsetExpected + posOffsetActual (the shoulder
        //     offset channels), zoom current+target, worldFOV, the cameraRoot
        //     world translate, and the camera's offset FROM the player
        //     resolved into the player's own yaw frame (right/fwd/up) â€” that
        //     last one is what makes an orbit or a shoulder swap legible
        //     rather than three world numbers that all change when you walk
        //   * the noise stack's applied translation/rotation this frame, so
        //     a noise-driven move is attributable instead of mysterious
        //   * WHO is active: SPIM active/framing, unpaused count, Tween flag,
        //     QT claim + input-block, camera state id, TDM lock + claims
        //
        // Armed on BOTH edges (open and close) of any menu/overlay and runs
        // ~4s, because the previous window was ~0.5s and the reported motion
        // may well be a slow drift or land on the close.
        {
            auto* uiMc  = RE::UI::GetSingleton();
            auto* plyMc = RE::PlayerCharacter::GetSingleton();
            if (uiMc && plyMc && camera) {
                auto& spimMc = ShowPlayerInMenusController::GetSingleton();
                const std::uint32_t unpausedMc = UnpauseManager::GetUnpausedMenuCount();
                const bool qtMc   = MenuUI::IsGameInputBlocked() || MenuUI::HasQuickTuneDMClaim();
                const bool anyMc  = spimMc.IsActive() || unpausedMc > 0 || qtMc;

                static bool sMcWas    = false;
                static int  sMcBudget = 0;
                static int  sMcSeq    = 0;
                if (anyMc != sMcWas) { sMcBudget = 260; sMcSeq = 0; }
                sMcWas = anyMc;

                if (sMcBudget > 0) {
                    --sMcBudget;
                    ++sMcSeq;
                    // Rendered basis â€” ground truth for what the player sees.
                    float rYaw = 0.0f, rPitch = 0.0f;
                    RE::NiPoint3 camWorld{};
                    if (camera->cameraRoot) {
                        camWorld = camera->cameraRoot->world.translate;
                        if (auto* nodeMc = camera->cameraRoot->AsNode();
                            nodeMc && !nodeMc->GetChildren().empty()) {
                            if (auto* niMc = skyrim_cast<RE::NiCamera*>(nodeMc->GetChildren()[0].get())) {
                                const auto& mMc = niMc->world.rotate;
                                const RE::NiPoint3 fMc{ mMc.entry[0][0], mMc.entry[1][0], mMc.entry[2][0] };
                                rYaw   = std::atan2(fMc.x, fMc.y);
                                rPitch = std::atan2(-fMc.z, std::sqrt(fMc.x * fMc.x + fMc.y * fMc.y));
                                camWorld = niMc->world.translate;
                            }
                        }
                    }
                    // Camera position RELATIVE to the player, in the player's
                    // yaw frame: right / forward / up. An orbit or a shoulder
                    // swap shows up here as a clean sign change on `right`,
                    // where three world coordinates would just look like noise.
                    const RE::NiPoint3 pPosMc = plyMc->GetPosition();
                    const float byaw = plyMc->data.angle.z;
                    const float sy = std::sin(byaw), cy = std::cos(byaw);
                    const float ex = camWorld.x - pPosMc.x;
                    const float ey = camWorld.y - pPosMc.y;
                    const float relRight = ex * cy - ey * sy;
                    const float relFwd   = ex * sy + ey * cy;
                    const float relUp    = camWorld.z - pPosMc.z;

                    RE::NiMatrix3 nRotMc; RE::NiPoint3 nTrMc;
                    const bool noiseOn =
                        CameraNoiseController::GetAppliedCameraOffset3p(nRotMc, nTrMc);

                    auto& tdmMc = TDMIntegration::GetSingleton();
                    spdlog::debug(
                        "[MENUCAM] #{} tween={} unpaused={} spim={}/{} qt={}/{} camState={} | "
                        "bodyZ={:.3f} bodyX={:.3f} frX={:.3f} frY={:.3f} frEn={} "
                        "rYaw={:.3f} rPitch={:.3f} | "
                        "posExp=({:.1f},{:.1f},{:.1f}) posAct=({:.1f},{:.1f},{:.1f}) "
                        "zoom={:.3f}/{:.3f} fov={:.1f} | "
                        "camRel=(r{:.1f},f{:.1f},u{:.1f}) camW=({:.0f},{:.0f},{:.0f}) | "
                        "noise={} nTr=({:.2f},{:.2f},{:.2f}) | lock={} yawClaim={} dmDis={}",
                        sMcSeq,
                        uiMc->IsMenuOpen("TweenMenu") ? 1 : 0, unpausedMc,
                        spimMc.IsActive() ? 1 : 0, spimMc.IsActiveCameraControlEnabled() ? 1 : 0,
                        MenuUI::IsGameInputBlocked() ? 1 : 0,
                        MenuUI::HasQuickTuneDMClaim() ? 1 : 0,
                        camera->currentState ? static_cast<int>(camera->currentState->id) : -1,
                        plyMc->data.angle.z, plyMc->data.angle.x,
                        a_this->freeRotation.x, a_this->freeRotation.y,
                        a_this->freeRotationEnabled ? 1 : 0,
                        rYaw, rPitch,
                        a_this->posOffsetExpected.x, a_this->posOffsetExpected.y,
                        a_this->posOffsetExpected.z,
                        a_this->posOffsetActual.x, a_this->posOffsetActual.y,
                        a_this->posOffsetActual.z,
                        a_this->currentZoomOffset, a_this->targetZoomOffset,
                        camera->worldFOV,
                        relRight, relFwd, relUp,
                        camWorld.x, camWorld.y, camWorld.z,
                        noiseOn ? 1 : 0, nTrMc.x, nTrMc.y, nTrMc.z,
                        tdmMc.GetRawTargetLockState() ? 1 : 0,
                        tdmMc.HasYawControl() ? 1 : 0,
                        tdmMc.HasDirectionalMovementDisabled() ? 1 : 0);
                }
            }
        }

        SyncDialogueLookLock();

        // [SPINDIAG] One-shot forensic for the rare teleport/blink "inside-out"
        // flash (world briefly flips, then slowly unwinds â€” a spring chasing a
        // huge one-frame delta, NOT the latched FOV-NaN). Watch the rendered
        // camera world-yaw / pitch / FOV for an implausible single-frame jump;
        // when one fires, log a WINDOW of the following frames so the slow
        // unwind is captured too, with every value that could be the source.
        // Available through Verbose Logging, with a session-wide line cap.
        if (spdlog::should_log(spdlog::level::debug)) {
            static float        s_spinPrevYaw   = 0.0f;
            static float        s_spinPrevPitch = 0.0f;
            static RE::NiPoint3 s_spinPrevPos{};
            static bool         s_spinInit      = false;
            static int          s_spinWindow    = 0;     // frames left to log after a jump
            static int          s_spinLogged    = 0;     // session-wide line cap

            if (auto* plySpin = RE::PlayerCharacter::GetSingleton(); plySpin && camera) {
                const float        worldYaw   = plySpin->data.angle.z + a_this->freeRotation.x;
                const float        worldPitch = plySpin->data.angle.x + a_this->freeRotation.y;
                const RE::NiPoint3 pos        = plySpin->GetPosition();

                if (s_spinInit) {
                    float dYaw = worldYaw - s_spinPrevYaw;
                    while (dYaw >  3.14159265f) dYaw -= 6.28318530f;
                    while (dYaw < -3.14159265f) dYaw += 6.28318530f;
                    float dPitch = worldPitch - s_spinPrevPitch;
                    while (dPitch >  3.14159265f) dPitch -= 6.28318530f;
                    while (dPitch < -3.14159265f) dPitch += 6.28318530f;
                    const float dx      = pos.x - s_spinPrevPos.x;
                    const float dy      = pos.y - s_spinPrevPos.y;
                    const float dz      = pos.z - s_spinPrevPos.z;
                    const float posJump = std::sqrt(dx * dx + dy * dy + dz * dz);

                    constexpr float kYawJump   = 1.047f;   // ~60 deg / frame
                    constexpr float kPitchJump = 0.785f;   // ~45 deg / frame
                    // Arm on an out-of-range worldFOV too, not just a yaw/pitch
                    // spike: the negative-FOV "inside-out" overshoot drifts in
                    // gradually (no per-frame angular jump), so the angular
                    // tests alone caught only its TAIL when a later pitch flip
                    // happened to fire. <=0 inverts the projection; >180 is
                    // also nonsense.
                    const bool fovBad = !(camera->worldFOV > 0.5f && camera->worldFOV < 180.0f);
                    const bool spun = std::abs(dYaw) > kYawJump || std::abs(dPitch) > kPitchJump ||
                                      !std::isfinite(worldYaw) || !std::isfinite(worldPitch) ||
                                      !std::isfinite(camera->worldFOV) || fovBad;
                    if (spun && s_spinWindow == 0 && s_spinLogged < 240)
                        s_spinWindow = 45;   // this frame + ~0.75s of unwind at 60fps

                    if (s_spinWindow > 0 && s_spinLogged < 240) {
                        --s_spinWindow;
                        ++s_spinLogged;
                        const bool locked   = TDMIntegration::GetSingleton().IsTargetLocked();
                        auto*      curState = camera->currentState.get();
                        const int  camState = curState ? static_cast<int>(curState->id) : -1;
                        spdlog::debug(
                            "[SPINDIAG] dYaw={:+.1f} dPitch={:+.1f} posJump={:.0f}u | worldYaw={:.3f} "
                            "worldPitch={:.3f} freeRot=({:.3f},{:.3f}) bodyZ={:.3f} bodyX={:.3f} FOV={:.2f} | "
                            "locked={} owns={} camState={} lockAimYaw={:.3f}",
                            dYaw * 57.2958f, dPitch * 57.2958f, posJump,
                            worldYaw, worldPitch, a_this->freeRotation.x, a_this->freeRotation.y,
                            plySpin->data.angle.z, plySpin->data.angle.x, camera->worldFOV,
                            locked ? 1 : 0, ownsCamera ? 1 : 0, camState,
                            CameraController::GetSingleton().GetLockAimYaw());
                    }
                }

                s_spinPrevYaw   = worldYaw;
                s_spinPrevPitch = worldPitch;
                s_spinPrevPos   = pos;
                s_spinInit      = true;
            }
        }

        // [TWEENX] bracket A - the very end of this hook, so the next frame's
        // line can say whether the ~5u lateral was already applied by the
        // time we handed the frame back.
        TweenXSampleLat(s_tweenXEnd3pRoot, s_tweenXEnd3pTrans);
    }

    void HookManager::HookedTrackingUpdateRotation(RE::ThirdPersonState* a_this)
    {
        const bool mounted = a_this->id == RE::CameraState::kMount;
        if (mounted) _originalHorseTrackingUpdateRotation(a_this);
        else _originalTrackingUpdateRotation(a_this);

        auto* camera = RE::PlayerCamera::GetSingleton();
        if (s_mountedRotationState != a_this || !s_mountedLockOutputAllowed ||
            !camera || camera->currentState.get() != a_this) return;

        // The original has freshly rebuilt rotation from engine/TDM inputs.
        // Correct it ONCE before ThirdPersonState::Update calls GetRotation and
        // computes its orbit/collision position. GetRotation itself also runs
        // inside that calculation (AE 1.6.1170, RVA 8E797D), so applying a filter
        // there changed a solver input and then applied it again to the result.
        // Actor angles, horseCurrentDirection and freeRotation.y remain untouched.
        const auto& q = a_this->rotation;
        const MountedTargetLock::Quaternion raw{q.w, q.x, q.y, q.z};
        auto output = raw;
        const bool ownsYaw = mounted && s_mountedLockOwnsYaw;
        if (ownsYaw) {
            output = MountedTargetLock::AimYaw(output, s_mountedLockWorldYaw);
            // TDM has already applied (previous horse heading - current heading).
            // Rebase our owned offset to its NEW heading for next frame/input.
            if (auto* player = RE::PlayerCharacter::GetSingleton())
                a_this->freeRotation.x = MountedTargetLock::RelativeYaw(
                    s_mountedLockWorldYaw, TrackingYawReference(a_this, player));
        }

        float correction = 0.0f;
        const auto elevation = MountedTargetLock::Elevation(output);
        if (elevation) {
            correction = s_mountedLockPitch.Sample(s_mountedLockFrame, *elevation,
                mounted && s_mountedLockSteady, s_mountedLockDt);
            if (correction != 0.0f) output = MountedTargetLock::Apply(output, correction);
        } else {
            s_mountedLockPitch.Reset();
        }
        if (ownsYaw || correction != 0.0f)
            a_this->rotation = {output.w, output.x, output.y, output.z};

        // Bounded automatic evidence for the next riding test; no verbose toggle
        // needed. Include both sides of the chained rotation correction so a
        // remaining animation/pivot issue can be distinguished from yaw leakage.
        static unsigned traceFrames = 0, traceRows = 0;
        if (ownsYaw && traceRows < 240 && (++traceFrames % 30 == 1)) {
            ++traceRows;
            const auto rawYaw = MountedTargetLock::Heading(raw);
            const auto outYaw = MountedTargetLock::Heading(output);
            const auto* player = RE::PlayerCharacter::GetSingleton();
            spdlog::debug("[MOUNTTRACK2] frame={} dt={:.4f} steady={} yaw raw={:.3f} want={:.3f} out={:.3f} "
                         "pitch={:.3f} correction={:.3f} riderYaw={:.3f} freeYaw={:.3f}",
                s_mountedLockFrame, s_mountedLockDt, s_mountedLockSteady,
                rawYaw.value_or(0.0f) * 57.2957795f, s_mountedLockWorldYaw * 57.2957795f,
                outYaw.value_or(0.0f) * 57.2957795f, elevation.value_or(0.0f) * 57.2957795f,
                correction * 57.2957795f, player ? player->data.angle.z * 57.2957795f : 0.0f,
                a_this->freeRotation.x * 57.2957795f);
        }
    }

    void HookManager::HookedGetRotation(RE::ThirdPersonState* a_this, RE::NiQuaternion& a_rotation)
    {
        CallOriginalGetRotation(a_this, a_rotation);
        if (DietDrCamera::SettingsManager::GetSingleton().diagnosticSuspendOverrides) return;
        CameraController::GetSingleton().ApplyPitchOffset(a_rotation);
    }

    // Disable the engine's look handler (right-stick / mouse camera rotation)
    // while the Dialogue Menu is open, re-enable when it closes. Keeps the
    // camera anchored facing the NPC for the duration of the conversation.
    // The floor-flash on moving-entry is solved by the time-based blend in
    // the face-lock block (see TickDialogueAimInit + s_dialogueAim* state
    // at the top of this namespace), not by this handler.

    static void SyncDialogueLookLock()
    {
        auto* pc = RE::PlayerControls::GetSingleton();
        if (!pc || !pc->lookHandler) return;

        const auto& settings = SettingsManager::GetSingleton();
        // Mirror the face-lock gate: speaker OR (menu-open AND lastSpeaker
        // AND not goodbye).
        auto*      mtm = RE::MenuTopicManager::GetSingleton();
        RE::Actor* gatedSpeaker = nullptr;
        if (mtm) {
            if (auto p = mtm->speaker.get(); p) {
                gatedSpeaker = p->As<RE::Actor>();
            } else if (s_dialogueMenuOpen && !mtm->forceGoodbye) {
                if (auto p2 = mtm->lastSpeaker.get(); p2) {
                    gatedSpeaker = p2->As<RE::Actor>();
                }
            }
        }
        const bool dialogueOpen = (settings.dialogueEnabled || settings.dialogueFirstPersonEnabled) &&
                                   gatedSpeaker != nullptr;

        static bool sWasLocked = false;
        if (dialogueOpen) {
            pc->lookHandler->SetInputEventHandlingEnabled(false);
            sWasLocked = true;
        } else if (sWasLocked) {
            pc->lookHandler->SetInputEventHandlingEnabled(true);
            sWasLocked = false;
            // Reset the player pitch/yaw we wrote during face-lock so the
            // next dialogue entry starts from a clean state (no stale values
            // flashing on-screen during the brief before our face-lock
            // re-engages). Also restore IsNPC â€” the face-lock sets this to
            // true continuously to suppress the engine's dialogue head-track
            // anim; leaving it on outside dialogue breaks player-specific
            // animations.
            // Restore the 1p face-lock's anim-graph override (IsNPC + the
            // dialogue-idle pins). Self-gated on s_isNpcOverriddenByUs, so it's
            // a no-op for a plain 3p dialogue that never set it â€” writing these
            // vars triggers graph re-evaluation, which is why it's gated. The
            // DialogueIdle Unlock inside is what frees the player from the pinned
            // pose (without it, a weapon-drawn entry leaves the graph unable to
            // reach combat idle â€” bricked). Don't reset player->data.angle.x
            // here â€” forcing it to 0 on exit caused the "snap upwards looking
            // straight forward" on close; let the engine's look-handling resume.
            // (On a 1p->3p mid-dialogue switch this already ran at the switch,
            // so here it's a no-op and the exit pose snap is gone.)
            RestoreDialogueNpcOverride();
        }

    }

    // (The Kill Camera feature â€” vanilla-killcam disablers, Custom Angles,
    // the framing latch, the compass rescue and the guaranteed-execution
    // testing toggle â€” was REMOVED 2026-08-17 at user request. See the
    // killcam-module notes for why each half failed. Do not rebuild it
    // without reading them.)


    void HookManager::HookedTransitionUpdate(RE::PlayerCameraTransitionState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        if (DietDrCamera::SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            _originalTransitionUpdate(a_this, a_nextState);
            return;
        }

        // ----- DISMOUNT CRASH GUARD + TRANSITION VISIBILITY ----------------
        //
        // Five CTDs in one evening, all on dismount, all identical:
        //   subss xmm0,[rdi+0x50]  rdi = 0  in TDM's HorseCameraState entry.
        // TDM dereferences HorseCameraState::horseRefHandle unchecked whenever
        // the camera state id reads kMount, so the fatal shape is "enter the
        // mount camera with a dead horse handle". camState at the crash frame
        // is 6 = kPCTransition, i.e. it happens as a TRANSITION resolves — and
        // this hook is the only place DDC can see what that transition is
        // actually doing.
        //
        // Logged on change (cheap, bounded) so the next occurrence names the
        // from/to pair instead of costing another round trip, and guarded: a
        // transition INTO the mount camera with no live mount behind it is
        // redirected to third person, which is where a dismount is going
        // anyway.
        if (a_this) {
            auto* pcT = RE::PlayerCamera::GetSingleton();
            const int fromId = a_this->transitionFrom ? static_cast<int>(a_this->transitionFrom->id) : -1;
            const int toId   = a_this->transitionTo   ? static_cast<int>(a_this->transitionTo->id)   : -1;
            static int sLastFrom = -2, sLastTo = -2;
            static int sTransLogged = 0;
            if ((fromId != sLastFrom || toId != sLastTo) && sTransLogged < 24) {
                ++sTransLogged;
                sLastFrom = fromId; sLastTo = toId;
                spdlog::debug("[CAMTRANS] {} -> {} (6=PCTransition 7=Tween 9=ThirdPerson "
                             "10=Mount 12=Dragon)", fromId, toId);
            }
            if (pcT && toId == static_cast<int>(RE::CameraState::kMount)) {
                auto* horseT = skyrim_cast<RE::HorseCameraState*>(
                    pcT->cameraStates[RE::CameraState::kMount].get());
                auto* plyT   = RE::PlayerCharacter::GetSingleton();
                bool handleOk = horseT && horseT->horseRefHandle && horseT->horseRefHandle.get();
                RE::NiPointer<RE::Actor> mountT;
                const bool riding = plyT && plyT->IsOnMount() &&
                                    plyT->GetMount(mountT) && mountT;

                // Still genuinely riding but the handle is empty: REPAIR it.
                // Aborting here would be wrong twice over — the player really
                // is on a horse, and ForceThirdPerson while mounted resolves
                // to kMount anyway, so it would walk straight back into the
                // same entry with the same empty handle.
                if (!handleOk && riding && horseT) {
                    horseT->horseRefHandle = mountT->CreateRefHandle();
                    handleOk = horseT->horseRefHandle && horseT->horseRefHandle.get();
                    static int sSeededT = 0;
                    if (sSeededT < 8) {
                        ++sSeededT;
                        spdlog::warn("[CAMTRANS] mount-camera transition had an EMPTY "
                                     "horseRefHandle while still riding — seeded it from the "
                                     "live mount (ok={}, seed {}/8)", handleOk, sSeededT);
                    }
                }

                // Not riding at all: entering the mount camera is meaningless
                // and fatal. Third person is where the dismount is headed.
                if (!riding) {
                    static int sMountTransKills = 0;
                    if (sMountTransKills < 10) {
                        ++sMountTransKills;
                        spdlog::warn("[CAMTRANS] ABORTING transition into the mount camera — "
                                     "the player is NOT on a mount. Entering it is what CTDs "
                                     "inside TDM's horse-camera entry (kill {}/10)",
                                     sMountTransKills);
                    }
                    pcT->ForceThirdPerson();
                    if (a_nextState.get() != nullptr) a_nextState.reset();
                    return;
                }
            }
        }

        // ----- DISMOUNT: NO ENGINE LERP. DDC OWNS THE MOVE. ----------------
        //
        // Make the dismount an ordinary PROFILE CHANGE, which is what the user
        // has been asking for from the start: *"it should feel like the
        // transitions everywhere else in this mod"*, *"as smooth as switching
        // from sheathed to melee"*.
        //
        // Sheathed->melee is smooth because exactly ONE thing moves the camera:
        // DDC's springs ease the framing inside a single camera state. The
        // dismount had TWO owners — the engine lerping the state change while
        // DDC eased the framing — and two smoothers with different clocks and
        // different endpoints is the jerk. Covering the transition
        // ([[CAMCOVER]]) made DDC's half smooth but left the engine's half
        // running alongside it.
        //
        // So kill the engine's half. The state swap becomes instant and DDC's
        // normal transition system carries the framing from the horseback
        // profile to the on-foot profile — same omegas, same sliders, same
        // Weight as every other state change in the mod.
        //
        // ----- [MOUNTEXIT-CUT] IS GONE. IT CANCELLED [CAMCOVER]. -----------
        //
        // The abort that lived here killed the engine's dismount lerp so DDC's
        // transition system would own the move instead. It never worked, and
        // the reason is structural rather than a matter of tuning: THE ABORT
        // RETURNED FROM THIS FUNCTION ~130 LINES ABOVE THE
        // CameraController::Update CALL AT THE BOTTOM. That call is the only
        // place DDC can tick during kPCTransition — it *is* [CAMCOVER]. So the
        // abort deleted the engine's half of the dismount AND DDC's coverage of
        // it in a single stroke, and nothing was left to move the camera.
        //
        // Proof, and it is the flat kind: `grep -c CAMCOVER` over the logs from
        // both test sessions returns 0. That line logs once per session on the
        // first covered transition. It has never executed, not once.
        //
        // With both halves gone CameraController had to animate the whole
        // 300-unit pull-back itself — the [MOUNTEXIT-Y] zoom fold. That fold IS
        // the push-in the user has described in the same words for fifteen
        // rounds. Measured 2026-08-26 17:35:11, horizontal camera-to-player:
        //
        //   h = 73.1 -> 67.7   two frames on the back of the player's head
        //          -> 363.9    the fold lands and pops it back out
        //          -> ~123     ~1.5s of zoom spring dollying inward
        //
        // The user diagnosed this correctly in round 12 and it is still the
        // instruction: *"Its a snap of a transition that vanilla has. That means
        // that our mod isn't covering the transition."* COVER it — do not
        // replace it. PlayerCameraTransitionState interpolates
        // transitionFrom->GetTranslation() toward transitionTo->GetTranslation(),
        // so DDC writing its framing onto the DESTINATION state every frame
        // makes vanilla's own lerp deliver the camera to the DDC pose. We supply
        // the target, the engine supplies the motion, and exactly one thing is
        // moving the camera — which is the whole reason sheathed->melee feels
        // the way the user keeps asking this edge to feel.
        //
        // THIS IS THE SECOND TIME THIS ABORT HAS BEEN REMOVED FOR THIS EXACT
        // REASON (see [MOUNTEXIT-ABORT], round 12). DO NOT ADD A THIRD.
        //
        // ===== 2026-08-27: IT IS BACK, AND THE NOTE ABOVE WAS WRONG. ========
        //
        // User, after five days of me answering the wrong question: *"transitioning
        // off the horse is supposed to transition TO THE PLAYER with the same
        // mechanism as the rest of the transitions. You have repeatedly done the
        // opposite. You keep adding bullshit transitions on top of the transition
        // being already incorrect."*
        //
        // They are right, and "cover the engine's lerp" was never the same
        // mechanism as the rest of the transitions. Every other state change in
        // this mod is DDC's springs easing the framing INSIDE ONE camera state.
        // The dismount was a vanilla 267 ms lerp between two states, with DDC
        // writing an endpoint into it — and then mMountY easing for another
        // ~1.3 s behind it. TWO movers, neither of them the transition system.
        // Sheathed->melee has exactly one. That is the whole difference.
        //
        // WHY THE PROHIBITION ABOVE DOES NOT APPLY. Its stated reason is not
        // "aborting is wrong" — it is that the abort RETURNED ~130 lines above
        // the CameraController::Update call at the bottom of this function, so
        // DDC stopped ticking. That is a control-flow bug with a known fix, not
        // a property of the approach: with the transition aborted there is no
        // kPCTransition to tick through, the camera goes straight to
        // kThirdPerson, and CameraController::Update resumes on its normal path
        // via HookedThirdPersonUpdate. The one frame that genuinely slipped
        // through — the "back of the player's head" frame — is closed by the
        // PRE-Update seed in HookedThirdPersonUpdate, which is the same
        // mechanism that already makes the MOUNT edge continuous (frame +1 of
        // the mount measures dW=0.18 in the 2026-08-27 log).
        //
        // This is the idiom this function already uses in four other places —
        // furniture in, furniture out, werewolf->1p, POV toggle — every one of
        // them commented "so the engine never visibly lerps the camera."
        // The dismount is the fifth. 3p destination only; a dismount into first
        // person is left entirely alone.
        if (a_this && a_this->transitionFrom && a_this->transitionTo) {
            auto* playerCam  = RE::PlayerCamera::GetSingleton();
            auto* mountState = playerCam ? playerCam->cameraStates[RE::CameraState::kMount].get() : nullptr;
            auto* thirdState = playerCam ? playerCam->cameraStates[RE::CameraState::kThirdPerson].get() : nullptr;
            if (mountState && thirdState &&
                a_this->transitionFrom == mountState &&
                a_this->transitionTo   == thirdState) {
                static bool sLogged = false;
                if (!sLogged) {
                    sLogged = true;
                    spdlog::debug("[MOUNTEXIT-OWN] dismount: engine lerp aborted, DDC's transition "
                                 "system owns the move — one mover, on the user's sliders");
                }
                // Arm BOTH halves HERE. This is the only frame on which DDC
                // knows a dismount is happening while the mount state still
                // holds its framing; the resolver reports it one to three
                // frames later. The first build shipped 2026-08-27 proved what
                // that gap costs — [PITCHX] composed posAct=(30,0,-10), i.e.
                // y=0 against the mounted zoom, while the seed (gated on "the
                // previous Update was the mount state") fired 21 ms late. That
                // is the snap, and it is the same one-frame-late family as
                // every other edge bug on this camera.
                // SEED RIGHT HERE, NOT FROM THE NEXT Update. Two builds tried
                // to seed from HookedThirdPersonUpdate and BOTH landed exactly
                // one frame late (2026-08-27: abort 45.334, seed 45.358), for a
                // structural reason no gating condition can fix — that hook is
                // not called for the third-person state until the frame AFTER
                // ForceThirdPerson, so the engine always composes one frame at
                // the third-person state's own posOffset.y of 0 first. That
                // single frame IS the snap: [PITCHX] caught it twice as
                // posAct=(30.0, 0.0, -10.0) against the mounted zoom, i.e. the
                // camera ~298 units closer for one frame.
                //
                // transitionFrom still holds the live mounted framing at this
                // instant, and HorseCameraState derives from ThirdPersonState,
                // so copying it across costs nothing and makes the destination
                // state's first composed frame identical to the last mounted
                // one. The channels take it from there.
                auto* mountTps = static_cast<RE::ThirdPersonState*>(mountState);
                auto* destTps  = static_cast<RE::ThirdPersonState*>(thirdState);
                destTps->posOffsetExpected = mountTps->posOffsetExpected;
                destTps->posOffsetActual   = mountTps->posOffsetActual;
                destTps->currentZoomOffset = mountTps->currentZoomOffset;
                destTps->targetZoomOffset  = mountTps->targetZoomOffset;
                spdlog::debug("[MOUNTEXIT-SEED] destination seeded AT THE ABORT: "
                             "pos=({:.1f},{:.1f},{:.1f}) zoom={:.3f}",
                             destTps->posOffsetActual.x, destTps->posOffsetActual.y,
                             destTps->posOffsetActual.z, destTps->currentZoomOffset);
                CameraController::NotifyMountExit();
                playerCam->ForceThirdPerson();
                if (a_nextState.get() != nullptr) a_nextState.reset();
                return;
            }
        }

        // Abort transitions targeting (or leaving) kFurniture so the
        // engine never visibly lerps the camera into the furniture
        // marker pose. The upstream NoFurnitureCamera detour redirects
        // every furniture activation to kThirdPerson (chairs, thrones,
        // AND workbenches), but the engine has other code paths that
        // can still request the kFurniture transition; this is the
        // backstop that catches them.
        if (a_this && a_this->transitionTo) {
            auto* playerCam = RE::PlayerCamera::GetSingleton();
            auto* furnState = playerCam ? playerCam->cameraStates[RE::CameraState::kFurniture].get() : nullptr;
            if (furnState && a_this->transitionTo == furnState) {
                // Force back to the originating POV without driving
                // the engine's transition lerp.
                const bool wasFirst = WasLastFrameFirstPerson();
                spdlog::info("TransitionUpdate: aborting â†’ kFurniture transition (wasFirst={})",
                             wasFirst);
                if (wasFirst) playerCam->ForceFirstPerson();
                else          playerCam->ForceThirdPerson();
                if (a_nextState.get() != nullptr) a_nextState.reset();
                return;
            }
            // Also catch transitions FROM kFurniture so the stand-up
            // path doesn't lerp through the furniture marker pose.
            const bool fromFurn = (a_this->transitionFrom == furnState);
            if (fromFurn) {
                const bool wasFirst = WasLastFrameFirstPerson();
                spdlog::info("TransitionUpdate: aborting kFurniture â†’ ? exit transition (wasFirst={})",
                             wasFirst);
                if (wasFirst) playerCam->ForceFirstPerson();
                else          playerCam->ForceThirdPerson();
                if (a_nextState.get() != nullptr) a_nextState.reset();
                return;
            }
        }

        // (The kVATS transition refusal lived here 2026-08-16..17. Removed
        // with the Kill Camera feature â€” vanilla killcams are vanilla's again.)

        // Werewolf: refuse the transition INTO first person outright, so the
        // engine never plays the half-second lerp toward a view we are going
        // to take straight back (HookedUpdateCameraPost's per-frame guard).
        // Same abort shape as the kFurniture case above.
        if (a_this && a_this->transitionTo &&
            StateResolver::GetSingleton().IsWerewolf()) {
            auto* playerCam = RE::PlayerCamera::GetSingleton();
            auto* fpState   = playerCam ? playerCam->cameraStates[RE::CameraState::kFirstPerson].get()
                                        : nullptr;
            if (playerCam && fpState && a_this->transitionTo == fpState) {
                playerCam->ForceThirdPerson();
                if (a_nextState.get() != nullptr) a_nextState.reset();
                return;
            }
        }

        // If the engine started a 1pâ†’3p transition while R3 is being held
        // from a 1p press, abort immediately by forcing back to 1p. This
        // catches the auto-zoom-out path that bypasses FirstPersonState::
        // Update's a_nextState output.
        if (((s_r3Held && s_r3PressWasFirst) ||
             (s_fKeyHeld && s_fKeyPressWasFirst)) && a_this)
        {
            auto* playerCam = RE::PlayerCamera::GetSingleton();
            auto* fpState   = playerCam ? playerCam->cameraStates[RE::CameraState::kFirstPerson].get() : nullptr;
            if (playerCam && fpState && a_this->transitionFrom == fpState) {
                spdlog::info("TransitionUpdate: aborting 1p->3p during POV-toggle hold");
                playerCam->ForceFirstPerson();
                // Don't call original â€” we've replaced the transition.
                return;
            }
        }

        // COVER THE TRANSITION. ThirdPersonState::Update does not run while
        // the engine is in kPCTransition, so this is the only place
        // CameraController can tick during it. Without this call DDC applies
        // nothing for the whole transition (~267 ms on a dismount), the engine
        // composes vanilla framing, and our framing snaps in when the
        // transition lands — the dismount artifact.
        //
        // CameraController::Update detects the transition itself and writes to
        // the DESTINATION third-person state, which is what
        // PlayerCameraTransitionState interpolates toward. Called BEFORE the
        // original so the pose is in place when it composes this frame.
        if (auto* camT = RE::PlayerCamera::GetSingleton()) {
            CameraController::GetSingleton().Update(camT);
        }

        _originalTransitionUpdate(a_this, a_nextState);
    }

    RE::BSEventNotifyControl HookManager::R3ReleaseSink::ProcessEvent(
        RE::InputEvent* const* a_event,
        RE::BSTEventSource<RE::InputEvent*>*)
    {
        if (!a_event || !*a_event) return RE::BSEventNotifyControl::kContinue;

        constexpr float kMinHoldSecs = 0.2f;  // anything shorter is a tap (TDM target-lock)

        // POV state is captured at the first frame of the R3 press (IsDown)
        // and used when the release fires. This matters because while R3 is
        // held in 1p the engine begins an auto-transition to 3p â€” by release
        // time IsInFirstPerson() would return false and we'd fire
        // ForceFirstPerson, sending the user back to 1p. Capturing at press
        // time reflects the user's actual intent.
        static bool sPressCaptured = false;

        // ITEM-PREVIEW MENU: make the right-stick click mean INSPECT and
        // nothing else.
        //
        // True Directional Movement reads the target-lock key from its own raw
        // sink on this same event source, so no amount of blocking the
        // PlayerInputHandler chain reaches it, and its only guards are
        // "game paused" and "movement controls disabled" â€” both of which cost
        // something real to satisfy while a menu is open (DDC unpauses these
        // menus on purpose, and killing movement controls kills walking).
        //
        // The way through is that TDM and the MENU match on DIFFERENT FIELDS of
        // the same event: TDM compares its bound key against `idCode`, while the
        // inventory's zoom is dispatched on the user event, which the engine has
        // already resolved to "Item Zoom" by the time it reaches us (confirmed
        // by the [MENUBTN] capture: userEvent='Item Zoom' idCode=128). So blank
        // the idCode and leave the user event alone: the menu still inspects,
        // and TDM has nothing to match. Nothing else about the menu changes â€”
        // movement, look and the rest are untouched.
        //
        // This needs DDC's sink to run before TDM's, which is why the sink now
        // registers at kInputLoaded (see InstallR3ReleaseSink).
        if (UnpauseManager::IsItemPreviewMenuOpen()) {
            constexpr std::uint32_t kNeutralised = 0xFFFFFFFFu;
            for (auto* e = *a_event; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
                if (e->GetDevice()    != RE::INPUT_DEVICE::kGamepad)    continue;
                auto* be = e->AsButtonEvent();
                if (!be) continue;
                if (be->idCode != RE::BSWin32GamepadDevice::Keys::kRightThumb) continue;
                static int sNeutraliseLogs = 0;
                if (sNeutraliseLogs < 8) {
                    ++sNeutraliseLogs;
                    spdlog::debug("[MENULOCK] neutralising R3 idCode in item menu "
                                 "(userEvent='{}' kept, down={})",
                                 be->QUserEvent().c_str() ? be->QUserEvent().c_str() : "(null)",
                                 be->IsDown() ? 1 : 0);
                }
                be->idCode = kNeutralised;
            }
            sPressCaptured = false;
            s_r3Held       = false;
            return RE::BSEventNotifyControl::kContinue;
        }

        // An unpaused menu (Barter etc.) is open â€” swallow R3 entirely so POV
        // doesn't flip while the player is trading. Our R3-hold detector is a
        // separate sink from the handler chain and would otherwise still fire
        // ForceFirstPerson / ForceThirdPerson. Reset press state so a release
        // landing after the menu closes can't trigger a stale POV swap.
        //
        // NOTE THE ORDER: the item-preview block above MUST come first. This
        // return fires for ANY unpaused menu â€” and an unpaused inventory is
        // exactly the case the neutralise exists for â€” so with the two the
        // other way round the neutralise never ran at all. That is why the
        // inspect press kept reaching TDM while the code to stop it was
        // sitting right there: proven by [MENULOCK] never appearing in the log
        // even though [MENUBTN], from a different hook, did.
        if (UnpauseManager::GetUnpausedMenuCount() > 0) {
            sPressCaptured = false;
            s_r3Held       = false;
            return RE::BSEventNotifyControl::kContinue;
        }
        for (auto* e = *a_event; e; e = e->next) {
            if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            if (e->GetDevice()    != RE::INPUT_DEVICE::kGamepad)    continue;
            auto* be = e->AsButtonEvent();
            if (!be) continue;
            if (be->GetIDCode() != RE::BSWin32GamepadDevice::Keys::kRightThumb) continue;

            if (be->IsDown()) {
                if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                    s_r3PressWasFirst = pc->IsInFirstPerson();
                    sPressCaptured    = true;
                }
                s_r3Held = true;
                continue;
            }

            if (be->IsPressed()) {
                s_r3Held = true;
                continue;  // held frames between down and up
            }

            if (!be->IsUp()) continue;  // only the release edge matters from here on
            s_r3Held = false;

            // Taps (<0.2s) are TDM target-lock territory; only holds trigger POV swap.
            if (be->HeldDuration() < kMinHoldSecs) {
                sPressCaptured = false;
                continue;
            }

            if (!sPressCaptured) continue;  // missed the press â€” bail out safely
            sPressCaptured = false;

            if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                // VL forms have no usable 1p rig â€” drop the POV swap.
                // (Same gate as HookedTogglePOVProcessButton.)
                const auto vlState = StateResolver::GetSingleton().GetState();
                const bool isVL =
                    vlState == CameraState::VampireLordSheathed          ||
                    vlState == CameraState::VampireLordSheathedLevitating ||
                    vlState == CameraState::VampireLordMelee             ||
                    vlState == CameraState::VampireLordMagic             ||
                    vlState == CameraState::VampireLordConcentration     ||
                    vlState == CameraState::VampireLordFireAndForget;
                // Werewolf gets the same treatment as Vampire Lord: no usable
                // 1p rig, so the POV swap is simply refused. This sink is a
                // SEPARATE input path from TogglePOVHandler, so the rule in
                // HookedTogglePOVProcessButton doesn't reach it.
                if (isVL || StateResolver::GetSingleton().IsWerewolf()) continue;
                // Same for an open item-preview menu: R3 there means inspect.
                if (UnpauseManager::IsItemPreviewMenuOpen()) continue;

                spdlog::info("R3Release: held {:.2f}s, pressPOV={} â€” {}",
                             be->HeldDuration(), s_r3PressWasFirst ? "1p" : "3p",
                             s_r3PressWasFirst ? "ForceThirdPerson" : "ForceFirstPerson");
                if (s_r3PressWasFirst) pc->ForceThirdPerson();
                else                    pc->ForceFirstPerson();
                // Bucket index encodes (category, POV) and is locked at
                // dialogue open â€” re-resolve so the new POV's preset list
                // is consulted instead of carrying over the stale bucket.
                // allowReroll=false: a POV switch must NOT re-roll the random
                // preset â€” that was re-picking the look on every F/R3 toggle
                // mid-dialogue. RefreshForPovChange does the same (no reroll).
                if (s_dialogueMenuOpen) DialogueLookPicker::OnDialogueOpen(/*allowReroll=*/false);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    void HookManager::InstallR3ReleaseSink()
    {
        // Called from kInputLoaded (preferred, earliest) and again from
        // kDataLoaded as a fallback for the case where the input manager
        // wasn't up yet. Registering twice would put the sink in the list
        // twice and double every handler, so latch it.
        static bool sInstalled = false;
        if (sInstalled) return;
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            sInstalled = true;
            idm->AddEventSink<RE::InputEvent*>(&_r3ReleaseSink);
            spdlog::info("HookManager: R3 release sink installed on BSInputDeviceManager");
        } else {
            spdlog::warn("HookManager: BSInputDeviceManager singleton not available â€” R3 sink NOT installed");
        }
    }

    void HookManager::HookedOrbitProcessButton(RE::PlayerInputHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
    {
        // static_cast performs the secondary-base adjustment; treating the
        // incoming pointer as a ThirdPersonState* would read the wrong fields.
        auto* state = static_cast<RE::ThirdPersonState*>(a_this);
        const bool keepOrbit = a_event && a_event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
            a_event->GetIDCode() == RE::BSWin32GamepadDevice::Keys::kRightThumb &&
            a_event->QUserEvent() == "Toggle POV" && a_event->IsUp();
        const auto orbit = state->freeRotation;
        const auto& original = state->id == RE::CameraState::kMount
            ? _originalHorseOrbitProcessButton : _originalOrbitProcessButton;
        original(a_this, a_event, a_data);
        if (keepOrbit) {
            const auto reset = state->freeRotation;
            state->freeRotation = orbit;
            static unsigned logs = 0;
            if ((reset.x != orbit.x || reset.y != orbit.y) && logs < 12) {
                ++logs;
                spdlog::info("[R3-RECENTER] prevented native orbit reset state={} yaw={:.3f}->{:.3f} pitch={:.3f}->{:.3f}",
                    static_cast<int>(state->id), orbit.x, reset.x, orbit.y, reset.y);
            }
        }
    }

    void HookManager::HookedTogglePOVProcessButton(RE::TogglePOVHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
    {
        // Transformed forms have no real 1p rig â€” entering 1p shows a
        // floating-hands shot that breaks the fantasy and exposes seams in the
        // morph mesh. Vanilla already refuses first person in both beast forms;
        // the only thing that was overriding it is DDC's own held-view toggle
        // below, which calls ForceFirstPerson directly. So skip THAT and hand
        // the button straight down the chain.
        //
        // Handing it down matters: True Directional Movement implements target
        // lock by hooking this same handler, so anything that swallows the
        // button (or refuses it at CanProcess, which is where this rule briefly
        // lived) takes lock-on away with it. That is what "can't lock onto
        // targets in werewolf form" was.
        {
            const auto tfState = StateResolver::GetSingleton().GetState();
            const bool blocked =
                tfState == CameraState::VampireLordSheathed          ||
                tfState == CameraState::VampireLordSheathedLevitating ||
                tfState == CameraState::VampireLordMelee             ||
                tfState == CameraState::VampireLordMagic             ||
                tfState == CameraState::VampireLordConcentration     ||
                tfState == CameraState::VampireLordFireAndForget     ||
                StateResolver::GetSingleton().IsWerewolf();
            if (blocked) {
                _originalTogglePOVProcessButton(a_this, a_event, a_data);
                return;
            }
        }

        // An item-preview menu is up: the POV button belongs to the menu.
        //
        // SWALLOWED, not chained. Chaining it down was the remaining half of
        // the inspect problem: whatever is next in the chain (vanilla's POV
        // toggle, or True Directional Movement's target lock) recentres the
        // camera behind the character, so pressing R3 to look at a sword swung
        // the view round underneath the menu. The engine's 3D item zoom does
        // not need this handler â€” it rides the menu's own input path â€” so
        // nothing is lost by stopping here.
        if (UnpauseManager::IsItemPreviewMenuOpen()) return;

        // Only swap POV when the user has HELD the "Toggle POV" binding past
        // a short threshold. On controller with TDM this matters because
        // R3-tap is already claimed by TDM target-lock â€” we must not fire
        // on the initial press. Hold duration comes from the button event's
        // heldDownSecs, which the engine increments each frame the button
        // is down. Fire once per hold via sTriggered, reset on release.
        constexpr float kHoldThreshold = 0.3f;  // seconds â€” matches vanilla fPOVSwitchHoldDelay

        static bool sTriggered = false;

        if (a_event && a_event->QUserEvent() == "Toggle POV") {
            const bool isGamepad =
                a_event->device.get() == RE::INPUT_DEVICE::kGamepad;

            // Keyboard / mouse path: swallow every Toggle POV event so
            // vanilla never sees them. Fire the POV toggle exactly once
            // on the release edge. This sidesteps both vanilla's
            // hold-fire (which was firing in addition to ours and
            // producing double-toggles on hold) and vanilla's tap-Up
            // zoom-based transition (which stalls in 3p due to our
            // zoom lock and produced the original 3pâ†’1p failure).
            if (!isGamepad) {
                if (a_event->IsDown()) {
                    // Press edge: capture POV-at-press so the camera-
                    // state update hooks can revert any POV change
                    // that happens during the hold (vanilla appears
                    // to have a hold-fire path outside both vtable
                    // slots we hook).
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                        s_fKeyPressWasFirst = pc->IsInFirstPerson();
                    }
                    s_fKeyHeld = true;
                }
                if (a_event->IsUp()) {
                    s_fKeyHeld = false;
                    const auto vlState = StateResolver::GetSingleton().GetState();
                    const bool isVL =
                        vlState == CameraState::VampireLordSheathed          ||
                        vlState == CameraState::VampireLordSheathedLevitating ||
                        vlState == CameraState::VampireLordMelee             ||
                        vlState == CameraState::VampireLordMagic             ||
                        vlState == CameraState::VampireLordConcentration     ||
                        vlState == CameraState::VampireLordFireAndForget;
                    if (!isVL) {
                        if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                            // Toggle from the original POV (what user
                            // had when they pressed F), not the
                            // current one â€” the revert logic in the
                            // state update hooks may not have managed
                            // to fully reassert it by the time this
                            // release fires.
                            spdlog::info("TogglePOV: M&K release (held={:.2f}s) pressWasFirst={} â€” direct toggle",
                                         a_event->HeldDuration(), s_fKeyPressWasFirst);
                            if (s_fKeyPressWasFirst) pc->ForceThirdPerson();
                            else                     pc->ForceFirstPerson();
                            // No reroll on POV switch (see R3-release note above).
                            if (s_dialogueMenuOpen) DialogueLookPicker::OnDialogueOpen(/*allowReroll=*/false);
                        }
                    }
                }
                // Swallow Down / Held / Up â€” vanilla never gets it.
                return;
            }

            // Gamepad path unchanged: pass Down through so TDM's
            // TogglePOVHandler hook can fire its target-lock OFF latch.
            // R3 hold is owned by HookManager::R3ReleaseSink. The in-hook
            // hold-threshold below stays as a defensive secondary fire
            // for gamepad users without TDM (or when its sink doesn't
            // claim the event for some reason).
            if (a_event->IsDown()) {
                if (!sTriggered && a_event->HeldDuration() >= kHoldThreshold) {
                    sTriggered = true;
                    if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                        const bool wasFirst = pc->IsInFirstPerson();
                        spdlog::info("TogglePOV: gamepad hold-threshold (held={:.2f}s), wasFirst={}",
                                     a_event->HeldDuration(), wasFirst);
                        if (wasFirst) pc->ForceThirdPerson();
                        else          pc->ForceFirstPerson();
                        // No reroll on POV switch (see R3-release note above).
                        if (s_dialogueMenuOpen) DialogueLookPicker::OnDialogueOpen(/*allowReroll=*/false);
                    }
                }
                _originalTogglePOVProcessButton(a_this, a_event, a_data);
                return;
            }
            // Gamepad release â€” reset one-shot, fall through to vanilla.
            sTriggered = false;
        }

        _originalTogglePOVProcessButton(a_this, a_event, a_data);
    }

    void HookManager::HookedTogglePOVUpdateHeldState(RE::TogglePOVHandler* a_this, const RE::ButtonEvent* a_event)
    {
        // Swallow keyboard/mouse "Toggle POV" entirely. Vanilla's
        // hold-past-threshold POV toggle runs from this slot (verified
        // empirically: ProcessButton swallow alone left a mid-hold
        // toggle that the user reported). For M&K we want pure "POV
        // changes only on release" semantics, which HookedTogglePOV-
        // ProcessButton already implements â€” letting vanilla's
        // UpdateHeldStateActive run too would race against that and
        // produce a second toggle mid-hold.
        // Item-preview menu: same rule as ProcessButton â€” vanilla's
        // hold-past-threshold POV toggle runs from THIS slot, so leaving it
        // chained would let a held R3 flip the view from inside the menu.
        if (UnpauseManager::IsItemPreviewMenuOpen()) return;
        if (a_event && a_event->QUserEvent() == "Toggle POV") {
            const bool isGamepad =
                a_event->device.get() == RE::INPUT_DEVICE::kGamepad;
            if (!isGamepad) {
                // Don't call original â€” vanilla never sees the held
                // state for keyboard F.
                return;
            }
        }
        _originalTogglePOVUpdateHeldState(a_this, a_event);
    }

    // ThirdPersonState::ProcessWeaponDrawnChange â€” the engine's sheathe/draw
    // edge handler, and the one place it rewrites the over-shoulder framing.
    //
    // WHY THIS EXISTS. CameraController::Update runs AFTER
    // _originalThirdPersonUpdate has already composed the frame, so everything
    // it writes is what the NEXT frame renders. That is fine while nothing else
    // touches the fields â€” but this handler fires on the draw/sheathe event,
    // between our write and the next Update. The engine's values (vanilla
    // over-shoulder X/Z plus fOverShoulderCombatAddY on the drawn side) then get
    // exactly one rendered frame before our next write puts the profile back.
    // One frame at a completely different framing reads as a snap, and because
    // the pop is to VANILLA's numbers rather than to the other profile's, it
    // happens even when the sheathed and unsheathed entries are byte-identical â€”
    // which is how the user hit it.
    //
    // Re-asserting immediately after the original closes that window. It writes
    // only values CameraController already writes every frame, so on any frame
    // the engine did NOT move them this is a no-op.
    //
    // The [DRAWEDGE] line records what the handler actually changed. If the
    // deltas read 0 the engine writes the framing somewhere else and this guard
    // is inert â€” that is the evidence, not a guess, and it costs one log line
    // per sheathe.
    void HookManager::HookedProcessWeaponDrawnChange(RE::ThirdPersonState* a_this, bool a_drawn)
    {
        if (!a_this) return;

        const float beforeX = a_this->posOffsetExpected.x;
        const float beforeY = a_this->posOffsetExpected.y;
        const float beforeZ = a_this->posOffsetExpected.z;
        const float beforeAx = a_this->posOffsetActual.x;
        const float beforeAy = a_this->posOffsetActual.y;
        const float beforeAz = a_this->posOffsetActual.z;
        const float beforeZt = a_this->targetZoomOffset;
        const float beforeZc = a_this->currentZoomOffset;

        CallOriginalProcessWeaponDrawnChange(a_this, a_drawn);

        // MOUNTED: HOLD posOffset.y ACROSS THE EDGE.
        //
        // This is THE "horseback and melee and archery have different angles
        // with the same settings" bug, and it is fixed here rather than by the
        // polling equaliser in CameraController (now removed) because this is
        // the exact instant the engine rewrites the value. The poll could only
        // ever sample near the edge and kept latching the post-change number.
        //
        // Only y is held. x/z/zoom stay whatever the engine decided, matching
        // the standing decision that mounted framing belongs to the engine —
        // y is singled out because it is the one channel that DIFFERS between
        // sheathed and drawn, and that difference is the whole complaint.
        //
        // Symmetric by construction: held on both the draw and the sheathe
        // edge, so the value simply never moves while mounted.
        if (a_this->id == RE::CameraState::kMount) {
            if (std::abs(a_this->posOffsetExpected.y - beforeY) > 0.01f ||
                std::abs(a_this->posOffsetActual.y   - beforeAy) > 0.01f) {
                static int sMountYHeld = 0;
                if (sMountYHeld < 6) {
                    ++sMountYHeld;
                    spdlog::debug("[MOUNTY] drawn={} — engine moved mounted posOffset.y "
                                 "exp {:.1f}->{:.1f} act {:.1f}->{:.1f}; holding the "
                                 "pre-edge value so mount entries frame alike",
                                 a_drawn, beforeY, a_this->posOffsetExpected.y,
                                 beforeAy, a_this->posOffsetActual.y);
                }
                a_this->posOffsetExpected.y = beforeY;
                a_this->posOffsetActual.y   = beforeAy;
            }
        }

        // Arm the character-vs-camera height probe on the same edge.
        ArmDrawZProbe(a_drawn);

        auto& settings = SettingsManager::GetSingleton();
        float x = 0.0f, y = 0.0f, z = 0.0f, zt = 0.0f, zc = 0.0f;
        const bool haveFraming =
            !settings.diagnosticSuspendOverrides &&
            CameraController::GetLastAppliedFraming(x, y, z, zt, zc);

        // Trace the draw-edge guard when collecting verbose diagnostics.
        if (spdlog::should_log(spdlog::level::debug)) {
            spdlog::debug(
                      "[DRAWEDGE] drawn={} | exp ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) "
                      "act ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) "
                      "zoom t {:.3f}->{:.3f} c {:.3f}->{:.3f} | restore={} to ({:.1f},{:.1f},{:.1f}) z {:.3f}",
                      a_drawn,
                      beforeX, beforeY, beforeZ,
                      a_this->posOffsetExpected.x, a_this->posOffsetExpected.y, a_this->posOffsetExpected.z,
                      beforeAx, beforeAy, beforeAz,
                      a_this->posOffsetActual.x, a_this->posOffsetActual.y, a_this->posOffsetActual.z,
                      beforeZt, a_this->targetZoomOffset,
                      beforeZc, a_this->currentZoomOffset,
                      haveFraming, x, y, z, zt);
        }

        if (!haveFraming) return;

        // Mounted framing is deliberately left to the engine (the mount
        // profiles were tuned on top of it), matching the posOffset.y pin in
        // CameraController::Update. Drawing a weapon on horseback should keep
        // whatever the engine just decided.
        const auto st = StateResolver::GetSingleton().GetState();
        if (st == CameraState::Horseback || st == CameraState::DragonRiding) return;

        a_this->posOffsetExpected.x = x;
        a_this->posOffsetExpected.y = y;
        a_this->posOffsetExpected.z = z;
        a_this->posOffsetActual.x   = x;
        a_this->posOffsetActual.y   = y;
        a_this->posOffsetActual.z   = z;
        a_this->targetZoomOffset    = zt;
        a_this->currentZoomOffset   = zc;
    }

    void HookManager::HookedGetTranslation(RE::ThirdPersonState* a_this, RE::NiPoint3& a_translation)
    {
        CallOriginalGetTranslation(a_this, a_translation);

        static bool logged = false;
        if (!logged) {
            spdlog::info("HookManager: GetTranslation (post-update) hook fired");
            logged = true;
        }

        // Per-state diagnostic: log engine field values at render time so we
        // can verify whether the same user settings produce identical engine
        // state across Sheathed/Melee/Magic etc. If fields differ here, the
        // engine is re-overriding our writes.
        auto& resolver = StateResolver::GetSingleton();
        const auto curState = resolver.GetState();
        const auto curSub   = resolver.GetSubState();
        static CameraState    dumpState    = static_cast<CameraState>(-99);
        static CameraSubState dumpSubState = static_cast<CameraSubState>(-99);
        if (curState != dumpState || curSub != dumpSubState) {
            dumpState    = curState;
            dumpSubState = curSub;
            spdlog::info("GetTranslation: state={} sub={} posAct=({:.2f},{:.2f},{:.2f}) posExp=({:.2f},{:.2f},{:.2f}) tZoom={:.3f} cZoom={:.3f} pZoom={:.2f} trans=({:.1f},{:.1f},{:.1f})",
                         static_cast<int>(curState), static_cast<int>(curSub),
                         a_this->posOffsetActual.x, a_this->posOffsetActual.y, a_this->posOffsetActual.z,
                         a_this->posOffsetExpected.x, a_this->posOffsetExpected.y, a_this->posOffsetExpected.z,
                         a_this->targetZoomOffset, a_this->currentZoomOffset, a_this->pitchZoomOffset,
                         a_translation.x, a_translation.y, a_translation.z);
        }
    }

    // ----- BleedoutCameraState hooks -----

    void HookManager::HookedBleedoutBegin(RE::BleedoutCameraState* a_this)
    {
        ReleaseDeathFreeLookInput();  // every death / knockdown captures a fresh view
        if (DietDrCamera::SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            _originalBleedoutBegin(a_this);
            return;
        }

        // Ragdoll vs death. The engine enters BleedoutCameraState for BOTH a real
        // death and a recoverable knockdown (Unrelenting Force, paralysis, â€¦). For
        // a ragdoll, run NONE of the death cam: skip the slow-mo/FOV/free-look, and
        // push fPlayerDeathReloadTime out of reach so the load-save prompt can't
        // fire while the player gets up. The alive ThirdPersonUpdate sync restores
        // the slider value once they're standing again.
        const auto& sRag = SettingsManager::GetSingleton();
        s_deathCamRealDeath = BleedoutIsRealDeath();
        s_slowmoFadeReq     = false;
        if (!s_deathCamRealDeath) {
            // Recoverable ragdoll. Always suppress the load-save prompt and run the
            // ragdoll cinematic (slow-mo here, FOV/free-look in Update); the
            // individual sliders (slow-mo strength 0, free-look off, â€¦) gate the
            // sub-effects, so no separate enable toggle is needed.
            if (auto* gsc = RE::GameSettingCollection::GetSingleton())
                if (auto* setting = gsc->GetSetting("fPlayerDeathReloadTime"))
                    setting->data.f = 86400.0f;
            if (auto* ini = RE::INISettingCollection::GetSingleton())
                if (auto* setting = ini->GetSetting("fPlayerDeathReloadTime:GamePlay"))
                    setting->data.f = 86400.0f;
            // Duration 0 means "no slow motion" now that 0 is the default â€”
            // it is the second half of the same off switch as Strength 0.
            if (sRag.ragdollCamSlowmoStrength > 0.001f &&
                sRag.ragdollCamSlowmoDuration > 0.001f) {
                s_slowmoStrengthActive = sRag.ragdollCamSlowmoStrength;
                s_slowmoDurationActive = sRag.ragdollCamSlowmoDuration;
                s_deathSlowmoLast      = std::chrono::steady_clock::now();
                s_deathSlowmoElapsed   = 0.0f;
                s_deathSlowmoActive    = true;
            }
            spdlog::info("HookManager: BleedoutCameraState::Begin â€” RAGDOLL (player alive)");
            _originalBleedoutBegin(a_this);
            return;
        }

        // Critical: push fPlayerDeathReloadTime BEFORE the engine's Begin
        // runs. The reload-prompt countdown captures the setting value once
        // at Begin time, so any later write (Update, sink, etc.) only takes
        // effect on the *next* death. Writing here makes the duration
        // slider apply on the very first death of a session.
        const auto& sCfg2 = SettingsManager::GetSingleton();
        const float desired = sCfg2.deathCameraInfiniteDuration
            ? 86400.0f
            : sCfg2.deathCameraHoldDuration;
        if (auto* gsc = RE::GameSettingCollection::GetSingleton()) {
            if (auto* setting = gsc->GetSetting("fPlayerDeathReloadTime")) {
                setting->data.f = desired;
            }
        }
        if (auto* ini = RE::INISettingCollection::GetSingleton()) {
            if (auto* setting = ini->GetSetting("fPlayerDeathReloadTime:GamePlay")) {
                setting->data.f = desired;
            }
        }
        spdlog::info("HookManager: BleedoutCameraState::Begin â€” primed fPlayerDeathReloadTime to {:.1f}s (infinite={})",
                     desired, sCfg2.deathCameraInfiniteDuration);

        // Arm the death-cam slow-motion envelope. The driver in
        // HookedBleedoutUpdate eases time down then back over Duration seconds.
        if (sCfg2.deathCameraSlowmoStrength > 0.001f &&
            sCfg2.deathCameraSlowmoDuration > 0.001f) {
            s_slowmoStrengthActive = sCfg2.deathCameraSlowmoStrength;
            s_slowmoDurationActive = sCfg2.deathCameraSlowmoDuration;
            s_deathSlowmoLast    = std::chrono::steady_clock::now();
            s_deathSlowmoElapsed = 0.0f;
            s_deathSlowmoActive  = true;
            spdlog::info("HookManager: death-cam slow-mo armed (strength={:.2f} duration={:.1f}s)",
                         sCfg2.deathCameraSlowmoStrength, sCfg2.deathCameraSlowmoDuration);
        }

        _originalBleedoutBegin(a_this);
    }

    void HookManager::HookedBleedoutEnd(RE::BleedoutCameraState* a_this)
    {
        // End rarely (never?) fires on the death-to-main-menu transition, but
        // we release here too as a backup for any code path that does call it.
        ReleaseDeathFreeLookInput();
        // Safety: if the slow-mo envelope was still mid-flight when bleedout
        // ended, restore normal time so it can't leak into gameplay.
        RestoreDeathSlowmo();
        spdlog::debug("HookManager: BleedoutCameraState::End - reset free look");

        _originalBleedoutEnd(a_this);
    }

    void HookManager::ReleaseDeathFreeLookInput()
    {
        s_bleedoutFreeLook.Reset();
        s_forceCasterPatchForDeathFreeLook = false;
        UpdateCameraCasterPatch();
    }

    void HookManager::RequestRagdollSlowmoFade()
    {
        s_slowmoFadeReq = true;
    }

    bool HookManager::IsBleedoutRealDeath()
    {
        return s_deathCamRealDeath;
    }

    void HookManager::HookedTweenMenuUpdate(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        if (auto* pcTf = RE::PlayerCamera::GetSingleton()) s_tfStatePre = pcTf->worldFOV;
        _originalTweenMenuUpdate(a_this, a_nextState);
        if (auto* pcTf = RE::PlayerCamera::GetSingleton()) s_tfStatePost = pcTf->worldFOV;
        // The original just eased worldFOV one step toward the engine's
        // tween view angle — from ITS OWN internal state, which is why every
        // write from a later hook was recomputed away next tick and the ramp
        // still rendered ([TWEENCAM] 2026-08-31: 90.35 -> 100.00 at a steady
        // ~0.35°/frame with both freeze holds active). Undoing it HERE, in
        // the same pass its write happens, is the only spot that wins every
        // frame. Unconfigured menus behave like a pause (user ruling): the
        // FOV on screen when the tween opened is the FOV for the window.
        // SPIM-configured tweens keep their own writer, as everywhere else.
        if (!ShowPlayerInMenusController::GetSingleton().IsActive() &&
            !SettingsManager::GetSingleton().diagnosticSuspendOverrides &&
            s_menuFovFreeze > 1.0f) {
            if (auto* pcamTw = RE::PlayerCamera::GetSingleton();
                pcamTw && pcamTw->worldFOV != s_menuFovFreeze) {
                pcamTw->worldFOV = s_menuFovFreeze;
            }
        }

        // THE freeze-then-snap fix (user report 2026-08-31/09-01: "walking
        // forward, the camera freezes and stays behind and then snaps to
        // where I'd be"). VANILLA kTween gates its exit on a ±10° view-angle
        // ease (~0.4s at ~25°/s, measured by [TWEENFOV]/[TWEENCAM]; Tween
        // Menu Overhaul's DLL was string-dumped and contains no camera code
        // — the whole effect family is the engine's). The menu CLOSE
        // unpauses the world instantly, so for that ease the camera sits
        // parked at the tween pose while the player walks out from under
        // it, then snaps. When the menu is unconfigured, request the return
        // state NOW through the engine's own transition channel — the same
        // a_nextState the engine itself writes when the ease completes — so
        // the camera is back on the player the frame the menu closes.
        if (!ShowPlayerInMenusController::GetSingleton().IsActive() &&
            !SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            auto* uiTw = RE::UI::GetSingleton();
            const bool tweenOpen = uiTw && uiTw->IsMenuOpen(RE::TweenMenu::MENU_NAME);
            const bool favOpen   = uiTw && uiTw->IsMenuOpen(RE::FavoritesMenu::MENU_NAME);
            const bool stay1p    = WasLastFrameFirstPerson();
            // FIRST PERSON USED TO REFUSE TO PARK — it requested the exit on
            // every tween tick so the park never established. REMOVED
            // 2026-09-01, and this is what it cost with the world running:
            //
            //   [TWEENCAM] distRoot = 10.16, 0.03, 10.14, 0.00, 10.23, 0.00,
            //              10.45, 0.00 ...   (18:59:48.356 onward, paused=0)
            //
            // The camera alternated every pass between 10u behind the player
            // and exactly ON them. Unpaused, the engine re-asserts kTween on
            // every frame the tween menu is open; we handed it straight back
            // every frame; and each hand-back is a FirstPersonState::Begin,
            // which drops the follow anchor onto the player. Ping-pong at
            // frame rate — the "still skips when unpaused". Paused, the world
            // does not tick, the engine never re-asserts, and the same code
            // bounced exactly once, which is why that half looked fixed.
            //
            // So first person now parks exactly like third person — one entry,
            // one exit, no Begin until the menu is gone ("use the same
            // machinery that all other transitions use"). What made parking
            // unacceptable before was the camera leaving the head; that is
            // now held at the render, in HookedNiCameraUpdateWorldData, where
            // the parked pass is written back to the first-person pose.
            if (!tweenOpen && !favOpen) {
                if (auto* pcamTw = RE::PlayerCamera::GetSingleton()) {
                    auto& ret = pcamTw->cameraStates[stay1p
                        ? RE::CameraState::kFirstPerson
                        : RE::CameraState::kThirdPerson];
                    if (ret) {
                        a_nextState = ret;
                        // A first-person return resets the engine's follow
                        // spring, and the camera pays back the whole running
                        // trail in one frame — THE forward skip. Arm the
                        // carry so the next first-person update continues the
                        // spring it was on. Third person is untouched: its
                        // follow lag is DDC's own and does not live in the
                        // camera state.
                        if (stay1p) { s_fpSpringHold = true; s_fpSpringWind = 0.0f; }
                    }
                }
            }
        }
    }

    void HookManager::HookedBleedoutUpdate(RE::BleedoutCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        // While any DDC menu is open, skip the engine's bleedout Update.
        // SKSE Menu Framework's BlockUserInput=true sets Main::freezeTime
        // but doesn't increment numPausesGame, so the engine's bleedout
        // countdown keeps ticking through our overlay. Suppressing the
        // engine tick freezes the countdown; our own FOV/free-look code
        // below still runs so the user can preview tuning live.
        const bool ddcMenuOpen = SKSEMenuFramework::IsInstalled() &&
                                  SKSEMenuFramework::IsAnyBlockingWindowOpened();
        // [DEATHPOLE] probe (2026-09-05, "looking all the way above or
        // underneath causes it to snap and freeze until returning"): what
        // the ENGINE's own Update left in rotationMtx and where it put the
        // camera, captured before our Euler rewrite below, so the pole
        // frames show whether the engine fights the rotation, moves the
        // position, or neither.
        static RE::NiPoint3 sBlEngFwd{};
        static RE::NiPoint3 sBlEngPos{};
        static RE::NiPoint3 sBlPrevPos{};
        static bool         sBlPrevPosOk = false;
        if (!ddcMenuOpen) {
            _originalBleedoutUpdate(a_this, a_nextState);
        }
        {
            const auto& m = a_this->rotationMtx.entry;
            sBlEngFwd = RE::NiPoint3{ m[0][1], m[1][1], m[2][1] };
            if (auto* pcP = RE::PlayerCamera::GetSingleton(); pcP && pcP->cameraRoot)
                sBlEngPos = pcP->cameraRoot->world.translate;
        }

        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (SettingsManager::GetSingleton().diagnosticSuspendOverrides ||
            !playerCam || playerCam->currentState.get() != a_this) {
            ReleaseDeathFreeLookInput();
            return;
        }

        const auto& settings = SettingsManager::GetSingleton();

        // Death vs ragdoll (decided at Begin). Both run the cinematic path; the
        // active FOV / free-look / slow-mo come from whichever set applies (the
        // ragdoll sliders gate their own sub-effects, so there's no enable toggle).
        const bool ragdoll = !s_deathCamRealDeath;
        const float activeFov      = ragdoll ? settings.ragdollCamFov      : settings.deathCameraFov;
        const bool  activeFreeLook = ragdoll ? settings.ragdollCamFreeLook : settings.deathCameraFreeLook;

        // Slow-motion driver â€” shared by death and ragdoll. Eases the global time
        // multiplier down to (1 - strength), holds, then eases it back to 1.0,
        // spanning Duration seconds of RUNNING game time (the accumulator pauses
        // during menus). Trapezoid envelope with smootherstep edges. Parameters
        // (s_slowmoStrengthActive/Duration) were captured at arm time; the ragdoll
        // fade hotkey can jump it straight into the ramp-out via s_slowmoFadeReq.
        if (s_deathSlowmoActive) {
            if (auto* timer = RE::BSTimer::GetSingleton()) {
                const float dur      = s_slowmoDurationActive > 0.5f
                                           ? s_slowmoDurationActive : 0.5f;
                // Strength is a 0..90 PERCENT; convert to a slowdown fraction
                // (resulting speed = 1 - fraction).
                const float strength = std::clamp(s_slowmoStrengthActive / 100.0f, 0.0f, 0.9f);

                // Advance the running-time accumulator, paused during menus.
                const auto  now = std::chrono::steady_clock::now();
                float       dtw = std::chrono::duration<float>(now - s_deathSlowmoLast).count();
                s_deathSlowmoLast = now;
                dtw = std::clamp(dtw, 0.0f, 0.1f);   // guard against frames the driver skipped
                auto* uiSlow = RE::UI::GetSingleton();
                const bool paused = ddcMenuOpen || MenuUI::IsQuickTuneOpen() ||
                                    (uiSlow && uiSlow->GameIsPaused());
                if (!paused) s_deathSlowmoElapsed += dtw;

                // Fade hotkey (ragdoll): jump to the START of the ramp-out so the
                // slow-mo eases back to normal now instead of holding the duration.
                if (s_slowmoFadeReq) {
                    s_slowmoFadeReq = false;
                    const float rampOut = std::clamp(dur * 0.35f, 0.10f, 0.80f);
                    s_deathSlowmoElapsed = (std::max)(s_deathSlowmoElapsed, dur - rampOut);
                }
                const float t = s_deathSlowmoElapsed;

                if (t >= dur || strength <= 0.001f) {
                    timer->SetGlobalTimeMultiplier(1.0f, true);
                    s_deathSlowmoActive = false;
                } else {
                    // Fast onset, smooth release. The ramp-IN is SHORT and uses
                    // an ease-OUT curve (max slope at t=0) so the slowdown bites
                    // almost immediately (kills the "micro delay"); the ramp-OUT
                    // is longer with a smootherstep so it eases back gently.
                    const float rampIn  = std::clamp(dur * 0.12f, 0.05f, 0.20f);
                    const float rampOut = std::clamp(dur * 0.35f, 0.10f, 0.80f);
                    float amount;
                    if (t < rampIn) {
                        const float x = t / rampIn;
                        amount = 1.0f - (1.0f - x) * (1.0f - x);   // ease-out (immediate onset)
                    } else if (t > dur - rampOut) {
                        float x = (dur - t) / rampOut;
                        x = std::clamp(x, 0.0f, 1.0f);
                        amount = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);   // smootherstep
                    } else {
                        amount = 1.0f;
                    }
                    const float mult = std::clamp(1.0f - amount * strength, 0.05f, 1.0f);
                    timer->SetGlobalTimeMultiplier(mult, true);
                }
            } else {
                s_deathSlowmoActive = false;
            }
        }

        if (activeFov > 0.0f) {
            playerCam->worldFOV = activeFov;
        }

        // Keep vanilla's bleedout state/position/reload behavior. Only the
        // orientation is owned here, using independent world-yaw and pitch.
        if (activeFreeLook) {
            const bool starting = !s_bleedoutFreeLook.IsActive();
            auto* ui = RE::UI::GetSingleton();
            const bool menuOverlay = ddcMenuOpen || (ui && ui->GameIsPaused());

            // Drain mouse counts even while blocked, and discard the entry /
            // resume sample in the driver so queued movement cannot snap back.
            long mdx = 0, mdy = 0;
            DirectInputMouseCapture::GetSingleton().Poll(mdx, mdy);
            BleedoutFreeLook::Input input{};
            input.mouseX = static_cast<double>(mdx);
            input.mouseY = static_cast<double>(mdy);
            if (!menuOverlay) {
                XINPUT_STATE xstate{};
                if (XInputGetState(0, &xstate) == ERROR_SUCCESS) {
                    input.stickX = xstate.Gamepad.sThumbRX;
                    input.stickY = xstate.Gamepad.sThumbRY;
                }
            }
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            s_bleedoutFreeLook.Update(a_this->rotationMtx.entry, input, now, !menuOverlay);
            if (starting && s_bleedoutFreeLook.IsActive()) {
                sBlPrevPosOk = false;
                s_forceCasterPatchForDeathFreeLook = true;
                UpdateCameraCasterPatch();
                spdlog::info("Death/ragdoll free look engaged (yaw={:.3f} pitch={:.3f} roll={:.3f})",
                    s_bleedoutFreeLook.Yaw(), s_bleedoutFreeLook.Pitch(), s_bleedoutFreeLook.Roll());
            }

            // [DEATHPOLE] probe: every frame while the vertical axis is
            // within ~13 deg of its clamp. Columns: our yaw/pitch; the forward
            // the ENGINE's Update produced this frame (before our rewrite)
            // and its implied pitch; our forward; the camera root position
            // and its per-frame jump. A pole snap is one of: engFwd pitch
            // disagreeing with ours (the engine re-derives and clamps), a
            // dPos spike (the orbit position clamps), or neither (then it
            // is downstream of this hook).
            {
                static int sPoleLines = 0;
                if (std::abs(s_bleedoutFreeLook.Pitch()) > 1.25 && sPoleLines < 150) {
                    ++sPoleLines;
                    const auto& m = a_this->rotationMtx.entry;
                    const RE::NiPoint3 ourFwd{ m[0][1], m[1][1], m[2][1] };
                    float dPos = -1.0f;
                    if (sBlPrevPosOk) {
                        const float dx = sBlEngPos.x - sBlPrevPos.x, dy = sBlEngPos.y - sBlPrevPos.y,
                                    dz = sBlEngPos.z - sBlPrevPos.z;
                        dPos = std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                    spdlog::debug("[DEATHPOLE] pitch={:+.3f} yaw={:+.3f} | engFwd=({:+.3f},{:+.3f},{:+.3f}) engPitch={:+.1f} "
                                 "| ourFwd=({:+.3f},{:+.3f},{:+.3f}) ourPitch={:+.1f} | pos=({:.1f},{:.1f},{:.1f}) dPos={:.2f}",
                                 s_bleedoutFreeLook.Pitch(), s_bleedoutFreeLook.Yaw(),
                                 sBlEngFwd.x, sBlEngFwd.y, sBlEngFwd.z,
                                 std::asin(std::clamp(sBlEngFwd.z, -1.0f, 1.0f)) * 57.2957795f,
                                 ourFwd.x, ourFwd.y, ourFwd.z,
                                 std::asin(std::clamp(ourFwd.z, -1.0f, 1.0f)) * 57.2957795f,
                                 sBlEngPos.x, sBlEngPos.y, sBlEngPos.z, dPos);
                }
                sBlPrevPos   = sBlEngPos;
                sBlPrevPosOk = true;
            }
        } else {
            ReleaseDeathFreeLookInput();
            sBlPrevPosOk    = false;
        }

        static bool logged = false;
        if (!logged) {
            spdlog::info("HookManager: BleedoutCameraState::Update hook fired (fov={:.1f} hold={:.1f}s freeLook={})",
                         settings.deathCameraFov, settings.deathCameraHoldDuration,
                         settings.deathCameraFreeLook ? "on" : "off");
            logged = true;
        }
    }

    // ----- Furniture camera kill switch -----
    //
    // Hooks the engine's "request camera to enter kFurniture" call
    // inside TESFurniture::Activate. The original `call` instruction
    // is replaced via SKSE trampoline write_call<5> so our thunk
    // runs in its place. Port of Ersh's NoFurnitureCamera (MIT).

    namespace
    {
        // FurnitureForces1stPerson keyword (Skyrim.esm: 0x000A56D7).
        // Looked up lazily on first detour call so we don't fight
        // load-order timing in plugin init.
        RE::BGSKeyword* GetFurnitureForces1pKeyword()
        {
            static RE::BGSKeyword* sKywd = nullptr;
            static bool sChecked = false;
            if (!sChecked) {
                sChecked = true;
                auto* form = RE::TESForm::LookupByID(0x000A56D7);
                sKywd = form ? form->As<RE::BGSKeyword>() : nullptr;
            }
            return sKywd;
        }

    }

    void HookManager::EnterFurniture(RE::PlayerCamera* a_camera, RE::TESFurniture* a_furniture)
    {
        // No camera state at all â€” fall through (super-rare; defensive).
        if (!a_camera || !a_camera->currentState) {
            _originalEnterFurniture(a_camera, a_furniture);
            return;
        }
        const auto curId = a_camera->currentState->id;

        // Workbenches (crafting stations) keep vanilla behavior. Their
        // furniture-camera handling does additional setup we don't
        // understand fully, so pass through untouched.
        if (a_furniture &&
            a_furniture->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone)
        {
            _originalEnterFurniture(a_camera, a_furniture);
            return;
        }

        // Some furniture (specific chairs/idles) is keyworded to force
        // 1st person on use â€” honor that.
        if (auto* kywd = GetFurnitureForces1pKeyword();
            kywd && a_furniture && a_furniture->HasKeyword(kywd))
        {
            if (curId != RE::CameraState::kFirstPerson) {
                a_camera->ForceFirstPerson();
            }
            return;
        }

        // Default: keep the player in 3rd person. Use the engine's
        // raw SetCameraState so the transition goes through the
        // normal state machine, no furniture-marker pose anywhere.
        if (curId != RE::CameraState::kThirdPerson) {
            sPlayerCameraSetState(a_camera, RE::CameraState::kThirdPerson);
        }
    }

    void HookManager::InstallFurnitureCameraKill()
    {
        // Patch site: a `call` instruction inside TESFurniture::Activate
        // that asks PlayerCamera to enter kFurniture. SE offset 0x295,
        // AE offset 0x2A2 from the function start. 14 bytes of
        // trampoline cover the write_call<5> redirection.
        const auto callSite = RuntimeHooks::Get().enterFurniture;
        RuntimeHooks::RequireCall(callSite);
        auto& trampoline = SKSE::GetTrampoline();
        _originalEnterFurniture = trampoline.write_call<5>(
            callSite,
            reinterpret_cast<std::uintptr_t>(&EnterFurniture));
        spdlog::info("HookManager: TESFurniture::Activate EnterFurniture call patched");
    }

    // ----- FirstPersonState hook -----

    void HookManager::ArmFirstPersonSpringCarry()
    {
        s_fpSpringHold = true;
        s_fpSpringWind = 0.0f;
    }

    void HookManager::SetUiDriveActive(bool a_active)
    {
        s_uiDriveActive.store(a_active, std::memory_order_relaxed);
    }

    bool HookManager::IsUiDriveActive()
    {
        return s_uiDriveActive.load(std::memory_order_relaxed);
    }

    void HookManager::HookedFirstPersonUpdate(RE::FirstPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
    {
        s_lastFrameWasFirstPerson.store(true, std::memory_order_relaxed);

        // ----- DOUBLE-PUMP SUPPRESSION -------------------------------------
        //
        // With an unpaused menu open, the UI drive in
        // UnpauseManager::MainThreadHook pumps the camera TWICE per rendered
        // frame. The player advances on only one of those passes, so the
        // engine's follow spring converges twice for one step of movement and
        // the two passes settle on two different trails — measured
        // 19:14:07.815, first person throughout, Tween open, player running:
        //
        //   [TWEENCAM] distRoot = 17.18, 9.22, 17.18, 9.28, 17.05, 9.23 ...
        //
        // a stable ~8u oscillation at pass rate. (Underneath the SPIM anchor
        // drop it read as 10 -> 0; two stacked artefacts, one signature.)
        //
        // THE TEST IS CAUSAL, NOT KINEMATIC. The extra pass is one WE cause,
        // from the UI drive in MainThreadHook, so the drive flags itself and
        // any camera update running under that flag is the extra one. A
        // movement test was tried instead — "a pass where the player has not
        // moved has nothing to integrate" — and it is wrong in the one case
        // that matters most: a player standing still looks identical on BOTH
        // passes, so both were skipped, the spring never converged while they
        // stood, and the whole of it was spent the moment they moved again.
        // That is "if I move at all, it stutters hard when I stop" (user,
        // 2026-09-01, unpaused with movement enabled) — my own suppression,
        // not the engine's. The game's own pass must always integrate.
        //
        // THE GRACE (0.25s past the window) is the close, and there the
        // movement test IS right, because the frame in question is one the
        // ENGINE froze rather than one we care to skip wholesale. The engine's
        // convergence is a fixed ~30% of the trail PER UPDATE whenever the
        // player is standing still — 20:16:06.491, one frame after the menu
        // closed, anchor freshly restored to 20.9 behind and the velocity
        // zeroed: distRoot came out 14.70, and 20.9 * 0.7 = 14.6. The close
        // hands the engine exactly one frozen frame (dPly=0.00, the player
        // resumes 5.51 the next frame) and it takes 30% of the trail on it:
        // [SNAP] JUMP 6.26u. Zeroing the velocity cannot stop that, because
        // the convergence is not the velocity — it is the lerp. The frame
        // simply must not be integrated, and the same movement test says so.
        //
        // Bounded at 0.25s so a player who genuinely stopped still gets the
        // ordinary convergence a moment later; and only where the double pump
        // exists (unpaused), so the paused path — which the user has already
        // accepted — is untouched.
        bool skipEngineUpdate = false;
        {
            auto*      uiDp      = RE::UI::GetSingleton();
            const bool menuWinDp = uiDp &&
                (uiDp->IsMenuOpen(RE::TweenMenu::MENU_NAME) ||
                 uiDp->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
                 uiDp->IsMenuOpen(RE::MagicMenu::MENU_NAME));
            const bool windowDp = menuWinDp && uiDp && !uiDp->GameIsPaused() &&
                !ShowPlayerInMenusController::GetSingleton().IsActive();

            // "Was a window open recently", not "did a window just close".
            // An EDGE cannot be detected here: this hook does not run while
            // the camera is parked in kTween, so for a parked window the
            // rising edge never happened and the falling edge never fired —
            // the grace stayed disarmed and the close frame integrated
            // anyway ([SNAP] JUMP 6.28u dPly=0.00, 20:40:41.043). The stamp
            // comes from the render hook, which runs on every pass whatever
            // state is current, so the close frame sees it one pass old.
            if (windowDp) s_menuWindowSeenTp = std::chrono::steady_clock::now();
            // NOT gated on !windowDp. The menu's open flag and the frame the
            // engine freezes the player on do not coincide: at 20:45:04.669
            // the close frame still read the tween as OPEN at first-person
            // update time, so a grace written as "the window has closed" was
            // false exactly when it was needed and the frame integrated —
            // damp 20.77 -> distRoot 14.61, the 30% again. Eligibility is
            // "a window is open OR was within 0.25s", nothing finer.
            const bool graceDp =
                s_menuWindowSeenTp.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - s_menuWindowSeenTp).count() < 0.25f;

            const bool suspendDp =
                SettingsManager::GetSingleton().diagnosticSuspendOverrides;
            const bool spimDp = ShowPlayerInMenusController::GetSingleton().IsActive();

            // (1) THE EXTRA PASS — never integrates, moving or standing.
            // Gated on a menu window as well as the flag: during ordinary
            // play the drive runs every frame but produces no camera pass, so
            // this can never touch open-world movement.
            if (menuWinDp && IsUiDriveActive() && !spimDp && !suspendDp)
                skipEngineUpdate = true;

            // (2) A PASS WITH NOTHING TO INTEGRATE — the duplicate inside the
            // window, and the one frozen frame the engine is handed at the
            // close. Both are the same question and it is asked of the same
            // clock: HAS THE FOLLOW TARGET MOVED SINCE THE LAST RENDERED PASS?
            //
            // THE CLOCK IS THE TARGET, NOT THE PLAYER. The spring converges
            // toward `Camera1st [Cam1]` on the first-person skeleton, and with
            // an unpaused menu open the camera is pumped twice per rendered
            // frame while that skeleton is refreshed once — so the player's
            // data position and the target advance on OPPOSITE passes.
            // Measured 2026-09-02 19:19:27, running, Tween open unpaused:
            //
            //   ran=1 dPly=5.38 dCam1=0.00  trail 26.36->20.99  <- integrated
            //   ran=0 dPly=0.00 dCam1=5.38  trail 20.99->20.99  <- skipped
            //
            // Asking the player's clock picked the stale-target pass every
            // frame: each integration converged the trail to 21 against a
            // target that had not moved, and the next pass handed the whole
            // 5.4 straight back (20.99 + 5.38 = 26.38). That is the flat +5.4
            // the camera carried for the life of every window — taken at the
            // open, held all window, and repaid in ONE frame at the close
            // (18:58:04.045 [TWEENSTUT] camDp= 10.90 against 5.39 either
            // side), which is the residual stutter. Outside the window the two
            // clocks are in phase (dPly=5.38 dCam1=5.38, ran=1 every pass), so
            // this changes nothing in ordinary play.
            //
            // Reference is s_tweenCarryPrevCam1 — the target at the last
            // RENDERED PASS, latched in HookedNiCameraUpdateWorldData, the same
            // reference the spring carry's `moving` test uses. A private
            // "position at the last integration" was tried and is WRONG: this
            // hook does not run at all while the camera is parked in kTween,
            // so on the close frame that reference was hundreds of ms stale,
            // read as a large delta, and let the frame integrate. Measured
            // 20:21:43.156 — [FPSPRING] moving=0 (per-pass reference, correct)
            // on the very frame the engine took its 30%: damp 21.07 ->
            // distRoot 14.72, and 21.07 * 0.7 = 14.75. Two tests of the same
            // question disagreeing is what let it through.
            //
            // A LONE frozen frame INSIDE motion is the hiccup; a run of them
            // is the player standing still and must converge normally. That
            // distinction is the whole difference between fixing the close
            // and reintroducing "it stutters hard when I stop": skip at most
            // ONE frozen frame, and only when the pass before it was moving.
            // Stopping therefore costs exactly one frame of delayed
            // convergence and then behaves; the close, which is a single
            // frozen frame between two moving ones, is skipped outright.
            // Evaluated on the GAME's passes only — the extra pass is already
            // gone above and must not disturb this state machine.
            // The movement state is tracked UNCONDITIONALLY — only the skip
            // itself is gated. Resetting these flags whenever the window was
            // not up is what disarmed this the round it was written: ordinary
            // play cleared sPrevPassMoved every frame, then the camera parked
            // in kTween for the whole window (this hook does not run there at
            // all), so on the close frame the guard asked "was the previous
            // pass moving" about a pass that never happened, got false, and
            // let the frame integrate — 20:50:29.864, [FPSPRING] moving=0 with
            // damp 20.83 -> distRoot 14.65, the 30% one more time. Tracked
            // across the window instead, the flag still holds the truth from
            // when the player opened the menu at a run.
            static bool sPrevPassMoved = false;
            static bool sSkippedFrozen = false;
            if (!skipEngineUpdate && s_tweenCarryCam1Primed) {
                if (auto* c1Dp = FirstPersonCamNode(RE::PlayerCharacter::GetSingleton())) {
                    const RE::NiPoint3 pDp = c1Dp->world.translate;
                    const float dx = pDp.x - s_tweenCarryPrevCam1.x;
                    const float dy = pDp.y - s_tweenCarryPrevCam1.y;
                    const float dz = pDp.z - s_tweenCarryPrevCam1.z;
                    // 0.1u per pass. The player's data position is a step
                    // function and a 0.01u threshold suited it; the target
                    // carries idle breathing on top, which must not read as
                    // movement or the duplicate pass stops being detectable.
                    // A walk is ~2.7u per frame and a sneak ~1.5u, so the
                    // margin over the real signal is still two orders wide.
                    const bool  movedNow = (dx * dx + dy * dy + dz * dz) >= 0.01f;
                    if (movedNow) {
                        sSkippedFrozen = false;
                    } else if (graceDp && !spimDp && !suspendDp &&
                               sPrevPassMoved && !sSkippedFrozen) {
                        skipEngineUpdate = true;
                        sSkippedFrozen   = true;
                    }
                    sPrevPassMoved = movedNow;
                }
            }
        }

        // ----- Follow-spring carry across the tween bounce ------------------
        //
        // See the s_fpSpringHold note. The Tween menu takes the camera into
        // kTween and the tween hook hands it straight back to kFirstPerson;
        // that return zeroes the engine's follow spring and the camera makes
        // up the whole running trail in one frame ([SNAP] dCam=26.33 with
        // dPly=0.00). Write the pre-bounce spring back HERE — before the
        // original update reads it — so the frame after the return continues
        // the same curve instead of starting a new one. Runs ahead of every
        // early return below so no branch can skip it.
        //
        // The [FPSPRING] line adjudicates the mechanism in one repro: it
        // prints what the engine has after the return next to what we saved.
        // engine (0,0,0) against a saved offset the size of the trail means
        // this is the writer; two equal values mean it is not, and the hop
        // lives somewhere else.
        if (s_fpSpringHold && s_fpSpringSaved &&
            !SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            // IS THE PLAYER MOVING THIS FRAME? It decides what a faithful
            // carry even means, and getting it wrong is the whole close lurch.
            //
            // The saved velocity was latched while RUNNING (|v| ~ 371 u/s).
            // Handing that back to a frozen player spends a full frame of it
            // in one step — 371 * 14.5ms = 5.4u, which is the measured close:
            //   17:47:50.093  [SNAP] window CLOSE
            //   17:47:50.107  [SNAP] *** JUMP 4.79u ... dPly=0.00
            // one frame later, camera (r16.33,f-12.90) -> (r12.59,f-9.95).
            // A spring released from REST instead moves O(dt^2) on its first
            // frame, so the trail then decays exactly the way it does when you
            // simply stop running — no isolated lurch.
            //
            // So: MOVING carries everything and the trail continues as though
            // the tween never happened (the unpaused case, and the frame the
            // player resumes on). STILL carries only the offset — which is
            // what actually holds the camera in place — and releases the
            // spring from rest. lastPosition is left to the engine while
            // still: it is the engine's own bookkeeping of where it last
            // integrated from, and pinning a stale one is a second phantom
            // step waiting to be spent at release.
            bool         moving  = false;
            bool         havePly = false;
            RE::NiPoint3 plyNow{};
            if (auto* plyS = RE::PlayerCharacter::GetSingleton()) {
                plyNow  = plyS->GetPosition();
                havePly = true;
                // SAME CLOCK AS THE SKIP TEST — the follow target at the last
                // rendered pass. Two tests of one question must never read two
                // references; that disagreement is what let the close frame
                // integrate for three rounds. Now that the clock is the target
                // rather than the player's data position, a pass this test
                // would call STILL is exactly a pass the rule above already
                // skipped, and the skip branch below holds the spring verbatim
                // instead of starting the wind-down — so the wind-down can no
                // longer fire on a player who is actually running.
                if (s_tweenCarryCam1Primed) {
                    if (auto* c1S = FirstPersonCamNode(plyS)) {
                        const RE::NiPoint3 cNow = c1S->world.translate;
                        const float dx = cNow.x - s_tweenCarryPrevCam1.x;
                        const float dy = cNow.y - s_tweenCarryPrevCam1.y;
                        const float dz = cNow.z - s_tweenCarryPrevCam1.z;
                        moving = (dx * dx + dy * dy + dz * dz) > 0.25f;  // >0.5u/pass
                    }
                }
            }
            if (s_fpSpringLogs < 20) {
                ++s_fpSpringLogs;
                spdlog::debug("[FPSPRING] tween return: moving={} engine damp=({:.2f},{:.2f},{:.2f}) "
                             "vel=({:.2f},{:.2f},{:.2f}) lastPos=({:.1f},{:.1f},{:.1f}) "
                             "ply=({:.1f},{:.1f},{:.1f}) -> saved damp=({:.2f},{:.2f},{:.2f}) "
                             "vel=({:.2f},{:.2f},{:.2f})",
                             moving ? 1 : 0,
                             a_this->dampeningOffset.x, a_this->dampeningOffset.y,
                             a_this->dampeningOffset.z,
                             a_this->lastFrameSpringVelocity.x,
                             a_this->lastFrameSpringVelocity.y,
                             a_this->lastFrameSpringVelocity.z,
                             a_this->lastPosition.x, a_this->lastPosition.y,
                             a_this->lastPosition.z, plyNow.x, plyNow.y, plyNow.z,
                             s_fpSpringDamp.x, s_fpSpringDamp.y, s_fpSpringDamp.z,
                             s_fpSpringVel.x, s_fpSpringVel.y, s_fpSpringVel.z);
            }
            // MEASURED SEMANTICS ([FPSPRING], 18:53:21.789 and after) — the
            // one fact the whole carry turns on:
            //
            //   engine damp=(15.82,-13.59)
            //   lastPos=(-1630.6,-114.6)  ply=(-1646.4,-101.0)
            //   lastPos - ply = (15.8,-13.6) == damp, every held frame.
            //
            // lastPosition is NOT the player's position — it is the camera's
            // LAGGED ANCHOR, the smoothed follow point that trails the player
            // while running, and dampeningOffset is just (anchor - player)
            // recomputed from it. lastFrameSpringVelocity is the anchor's
            // velocity. (It reads equal to the player only on the return frame
            // itself, because that is exactly what Begin does to it: sets the
            // anchor onto the player, which collapses the trail — THE skip.)
            //
            // So the ANCHOR is the control. Everything else is downstream:
            //   * Leaving lastPosition alone while zeroing the velocity (two
            //     builds ago) left Begin's collapsed anchor in place — the
            //     whole trail gone on frame 2 of the window.
            //   * Writing lastPosition = playerPos to "avoid a phantom step"
            //     (last build) IS the collapse, done by hand, with the held
            //     velocity spent on top: [SNAP] JUMP 23.13u dPly=0.00 at
            //     18:53:22.104, camera (r15.45,f-14.20) -> (r-1.61,f+1.41),
            //     two units PAST the player.
            // The anchor is never dropped. It is either held, or walked.
            auto* uiFp = RE::UI::GetSingleton();
            const bool windowUp = uiFp &&
                                  uiFp->IsMenuOpen(RE::TweenMenu::MENU_NAME) &&
                                  uiFp->GameIsPaused();
            static std::chrono::steady_clock::time_point sWindLast{};
            const auto nowWind = std::chrono::steady_clock::now();
            float dtWind = sWindLast.time_since_epoch().count()
                ? std::chrono::duration<float>(nowWind - sWindLast).count() : (1.0f / 60.0f);
            sWindLast = nowWind;
            dtWind = std::clamp(dtWind, 0.0001f, 0.1f);

            if (skipEngineUpdate) {
                // The engine is not integrating this pass at all (see the
                // double-pump suppression above), so there is nothing to
                // carry ACROSS — hold the spring verbatim and do not start
                // the wind-down. Two mechanisms aimed at the same frame would
                // otherwise both spend it: the walk would move the anchor on
                // a frame the engine was already frozen on.
                a_this->lastPosition            = s_fpSpringLastPos;
                a_this->lastFrameSpringVelocity = s_fpSpringVel;
                a_this->dampeningOffset         = s_fpSpringDamp;
            } else if (moving) {
                // The player is live again: the saved spring describes exactly
                // what they are doing, so hand it back whole and let go. This
                // is the unpaused case, and the frame movement resumes on
                // after a paused window — the trail continues as though the
                // tween never happened. If the wind-down below had already
                // started eating the trail, leave the engine where it is
                // rather than snapping the trail back out.
                if (s_fpSpringWind <= 0.001f) {
                    a_this->lastPosition            = s_fpSpringLastPos;
                    a_this->lastFrameSpringVelocity = s_fpSpringVel;
                    a_this->dampeningOffset         = s_fpSpringDamp;
                }
                s_fpSpringHold = false;
            } else if (windowUp) {
                // Frozen world, menu up: hold the spring verbatim. The player
                // cannot move, so the saved values stay exactly valid and the
                // window has literally zero camera movement.
                a_this->lastPosition            = s_fpSpringLastPos;
                a_this->lastFrameSpringVelocity = s_fpSpringVel;
                a_this->dampeningOffset         = s_fpSpringDamp;
                s_fpSpringWind = 0.0f;
            } else {
                // Menu gone and the player STILL frozen. Handing the anchor
                // back here costs the whole trail either way: released, the
                // engine spends a frame of the held RUNNING velocity at once
                // (371 u/s * 14.5ms = 5.4u — [SNAP] JUMP 4.79u dPly=0.00 at
                // 17:47:50.107), which is its own first frame of catch-up
                // when you stop running (4.27, 3.53, 2.90, 2.22 ...). Normally
                // your deceleration masks that; a menu pause makes the stop
                // instant, so it lands bare.
                //
                // So WALK the anchor to the player instead of dropping it —
                // smoothstep over ~0.35s, slow at both ends, no frame bigger
                // than about 1.3u — and fade the anchor's velocity out with
                // it so nothing is left to spend at the end. The first frames
                // are nearly free, so a player who resumes within a few frames
                // (the ordinary tab-tab-still-holding-W) loses almost none of
                // the trail before the branch above hands it all back.
                s_fpSpringWind += dtWind;
                const float t = std::clamp(s_fpSpringWind / 0.35f, 0.0f, 1.0f);
                const float s = t * t * (3.0f - 2.0f * t);   // smoothstep
                if (havePly) {
                    a_this->lastPosition = RE::NiPoint3{
                        s_fpSpringLastPos.x + (plyNow.x - s_fpSpringLastPos.x) * s,
                        s_fpSpringLastPos.y + (plyNow.y - s_fpSpringLastPos.y) * s,
                        s_fpSpringLastPos.z + (plyNow.z - s_fpSpringLastPos.z) * s };
                } else {
                    a_this->lastPosition = s_fpSpringLastPos;
                }
                // ZERO, not a fading fraction of the saved velocity. WE are
                // driving the anchor on this branch, so any velocity left in
                // the engine is spent ON TOP of the walk: at s=0.005 the lerp
                // has moved the anchor by nothing at all, and the engine still
                // carried the full running 371 u/s into the frame —
                //   20:10:48.286 [SNAP] JUMP 7.99u dPly=0.00, trail 21 -> 13
                //   20:10:47.688 [SNAP] JUMP 4.90u dPly=0.00, trail 20.8 -> 15.9
                // — which is the entire close lurch this branch exists to
                // prevent, reintroduced by the fade.
                //
                // (Zeroing the velocity is only safe BECAUSE this branch
                // writes lastPosition every frame. The build that zeroed it
                // while leaving the anchor alone collapsed the whole trail —
                // there the engine's own anchor was the one that moved. The
                // rule is: whoever owns the anchor owns the velocity too.)
                a_this->lastFrameSpringVelocity = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
                if (t >= 1.0f) s_fpSpringHold = false;
            }
        }

        // Ragdoll/death recovery mirror. The third-person update owns the
        // normal recovery for the bleedout free-look feature â€” it clears the
        // collision override and calls RestoreDeathSlowmo() every frame (see
        // the ~962/967 block). But if the player toggles to FIRST PERSON
        // while a ragdoll (recoverable knockdown) is in effect, neither the
        // third-person update NOR the bleedout update runs anymore, so the
        // global time multiplier stays clamped low â€” the whole game runs in
        // slow motion and the player crawls, reading as "can't move." Mirror
        // the recovery here so any first-person frame restores normal time
        // (and reverts the collision patch; UpdateCameraCasterPatch is
        // change-gated so the per-frame call is a no-op once settled). Runs
        // before the early-return branches below so it can't be skipped.
        s_bleedoutFreeLook.Reset();
        s_forceCasterPatchForDeathFreeLook = false;
        RestoreDeathSlowmo();
        UpdateCameraCasterPatch();

        // Inverse revert via direct state-pointer swap (same rationale
        // as the ThirdPersonUpdate site â€” avoid Force*Person side
        // effects that animate the head bone).
        if (s_fKeyHeld && !s_fKeyPressWasFirst) {
            if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                auto& tpSlot = pc->cameraStates[RE::CameraState::kThirdPerson];
                if (tpSlot.get() && pc->currentState != tpSlot) {
                    pc->currentState = tpSlot;
                }
                return;
            }
        }

        // Tick StateResolver in 1p too. The 3p update path drives the
        // resolver via CameraController::Update, but 1p doesn't go
        // through there â€” so without this call, GetSchool() / GetCastType()
        // (and BlockKind, etc.) go stale while the player is in 1p,
        // breaking school-specific FP profile routing.
        StateResolver::GetSingleton().Update(RE::PlayerCharacter::GetSingleton());

        if (DietDrCamera::SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
            _originalFirstPersonUpdate(a_this, a_nextState);
            return;
        }

        TickDialogueAimInit();

        // [FPKICK] THE SECOND FOV WRITER (2026-08-30, "sprinting increases
        // the fov even when it has the same settings"). Vanilla's 1p sprint
        // FOV widen is not an ini value and not a write to worldFOV (the
        // FOREIGN-WRITE detector never fired): the sprint ANIMATION drives
        // the skeleton's FOV-control node, and FirstPersonState::Update —
        // the original we are about to call — composes the render FOV from
        // that node DOWNSTREAM of everything DDC writes. That is why every
        // curve change felt identical and why identical entries still
        // widened: the kick rides a channel our applier never touched.
        //
        // Fix: pin the node's local transform to its rest pose HERE, after
        // the anim graph wrote it this frame and before the engine reads
        // it. The bobbing is untouched — it lives on the camera bone, a
        // different node. This also retires block/draw anim zooms on the
        // same channel: FOV becomes exactly what the resolved entry says,
        // which is the whole contract of the First Person section.
        //
        // Rest pose is latched from live idle (not assumed) the first time
        // the engine is quiet — no sprint, no melee state — and re-latched
        // if the node pointer changes (skeleton swap). Until latched we
        // leave the node alone rather than pin a guessed identity.
        if (auto* fovCtl = a_this->firstPersonFOVControl) {
            static RE::NiTransform sFovCtlRest{};
            static RE::NiNode*     sFovCtlLast     = nullptr;
            static bool            sFovCtlRestValid = false;
            static float           sKickLogScale   = -999.0f;
            static float           sKickLogY       = -999.0f;
            static int             sKickLog        = 0;
            if (fovCtl != sFovCtlLast) {
                sFovCtlLast      = fovCtl;
                sFovCtlRestValid = false;
            }
            // Probe BEFORE the pin: this is what the animation wrote this
            // frame, i.e. the kick itself, whichever channel carries it.
            const float animScale = fovCtl->local.scale;
            const auto& animPos   = fovCtl->local.translate;
            if (sKickLog < 60 &&
                (std::abs(animScale - sKickLogScale) > 0.002f ||
                 std::abs(animPos.y - sKickLogY) > 0.05f)) {
                ++sKickLog;
                sKickLogScale = animScale;
                sKickLogY     = animPos.y;
                spdlog::debug("[FPKICK] fovControl '{}' scale={:.4f} pos=({:.3f},{:.3f},{:.3f}) rest={}",
                             fovCtl->name.c_str(), animScale,
                             animPos.x, animPos.y, animPos.z,
                             sFovCtlRestValid ? "pinned" : "unlatched");
            }
            if (!sFovCtlRestValid) {
                if (auto* plK = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* asK = plK->AsActorState()) {
                        if (!asK->IsSprinting() &&
                            asK->actorState1.meleeAttackState ==
                                RE::ATTACK_STATE_ENUM::kNone) {
                            sFovCtlRest      = fovCtl->local;
                            sFovCtlRestValid = true;
                            spdlog::debug("[FPKICK] rest pose latched: scale={:.4f} pos=({:.3f},{:.3f},{:.3f})",
                                         sFovCtlRest.scale,
                                         sFovCtlRest.translate.x,
                                         sFovCtlRest.translate.y,
                                         sFovCtlRest.translate.z);
                        }
                    }
                }
            } else {
                fovCtl->local = sFovCtlRest;
            }
        }

        if (!skipEngineUpdate) _originalFirstPersonUpdate(a_this, a_nextState);

        // Latch the follow spring's end-of-frame state for the carry above.
        // Skipped while a hold is live, so the pre-bounce values survive the
        // whole tween window. Taken before the face-lock pin below, which
        // zeroes lastFrameSpringVelocity for its own reasons.
        if (!s_fpSpringHold) {
            s_fpSpringLastPos = a_this->lastPosition;
            s_fpSpringVel     = a_this->lastFrameSpringVelocity;
            s_fpSpringDamp    = a_this->dampeningOffset;
            s_fpSpringSaved   = true;
        }

        // Pin the engine's internal pitch-offset spring during face-lock.
        // FirstPersonState::Update (the original we just called) integrates
        // currentPitchOffset/targetPitchOffset/lastFrameSpringVelocity into
        // a damped pitch the engine writes into the camera matrix downstream.
        // That damping has a ~1s time constant: even when we pin
        // player->data.angle.x to face pitch each frame, the engine's
        // rendered pitch ramps toward our value over ~1s â€” so face-lock
        // *visibly* lags during dialogue entry. Zeroing the offset state
        // here means the engine's offset stays at 0 with no momentum,
        // and the rendered pitch tracks data.angle.x (which we pin) with
        // no smoothing on top. Run this BEFORE SyncDialogueLookLock so
        // any state the engine derived during the original call is
        // overwritten before downstream reads.
        //
        // Use s_dialogueFaceLockWasActive as the gate â€” true while face-
        // lock is engaged AND through the release grace, false post-grace.
        // 1-frame lag on entry (HookedFirstPersonUpdate runs before
        // HookedUpdateCameraPost in the same frame, so engagement frame
        // sees the previous frame's flag) is acceptable; on next frame
        // pinning is fully active.
        if (s_dialogueFaceLockWasActive) {
            a_this->currentPitchOffset       = 0.0f;
            a_this->targetPitchOffset        = 0.0f;
            a_this->lastFrameSpringVelocity.x = 0.0f;
            a_this->lastFrameSpringVelocity.y = 0.0f;
            a_this->lastFrameSpringVelocity.z = 0.0f;

            // Set IsNPC=true on the player's anim graph so the engine's
            // anim-graph state machine treats the player as an NPC. This
            // suppresses the player-specific "greeting â†’ dialogue talking"
            // body-pose transition that fires ~1.1s after dialogue start
            // and snaps WEAPON/rHand/lHand/rUpA/etc bones simultaneously
            // (confirmed via per-frame node-rotation diagnostic log).
            // SetGraphVariableBool triggers anim graph re-evaluation, so
            // we only write it ONCE per face-lock activation (gated by
            // s_isNpcOverriddenByUs). The existing exit path in
            // SyncDialogueLookLock resets it to false when face-lock ends.
            // Per-frame anim graph suppression. The snap at greeting-voice-
            // end is the engine's "DialogueIdle â†’ DialogueResponseIdle"
            // state transition, gated by iSyncDialogueResponse changing
            // value. The IdleDialogueUnlock event (which we WERE sending,
            // wrongly) is what fires at voice-end and permits the snap.
            //
            // Pin iSyncDialogueResponse to 0 every frame so the response
            // sync doesn't change. Send IdleDialogueLock continuously to
            // keep the actor in DialogueIdle and prevent the unlock state
            // from being entered.
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                // Snapshot BEFORE the first write of this activation â€” this
                // runs ahead of the head-track clear below, so what we capture
                // is the pre-dialogue state, not our own override.
                if (!s_isNpcOverriddenByUs) {
                    if (auto* aState = player->AsActorState())
                        s_savedHeadTracking = aState->actorState2.headTracking;
                    std::int32_t sync = 0;
                    if (player->GetGraphVariableInt("iSyncDialogueResponse", sync))
                        s_savedSyncDialogueResponse = sync;
                }
                player->SetGraphVariableBool("IsNPC", true);
                player->SetGraphVariableBool("bIdleBeforeConversation", false);
                player->SetGraphVariableBool("bAnimationDrivenDialogue", false);
                player->SetGraphVariableBool("bMotionDrivenDialogue",    false);
                player->SetGraphVariableBool("bIsDialogueExpressive",    false);
                player->SetGraphVariableBool("bHHDialogue",              false);
                player->SetGraphVariableBool("bHDDialogue",              false);
                player->SetGraphVariableInt("iSyncDialogueResponse",     0);
                player->NotifyAnimationGraph("IdleDialogueLock");
                if (!s_isNpcOverriddenByUs) {
                    s_isNpcOverriddenByUs = true;
                    spdlog::debug("[DLG] anim-graph per-frame suppression begin (Lock event, iSyncDialogueResponse=0)");
                }
            }

            // Suppress the player's head-tracking entirely during face-lock.
            // Previous attempt cleared just kDialogue â€” log showed the
            // engine's pitch still ramped, so the lag isn't (only) from
            // the kDialogue head-track. Try the broader hammer:
            //   - Clear ALL six head-track types (kDefault/kAction/kScript/
            //     kCombat/kDialogue/kProcedure)
            //   - Zero the actorState2.headTracking bit-flag
            // If the engine's pitch ramp is from head-tracking at all,
            // this catches it. If the next log STILL shows the ramp,
            // the source is something else (not head-track) and we need
            // a different investigation.
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                auto* proc = player->GetActorRuntimeData().currentProcess;
                if (proc && proc->high) {
                    using HT = RE::HighProcessData::HEAD_TRACK_TYPE;
                    proc->high->ClearHeadtrackTarget(HT::kDefault,   false);
                    proc->high->ClearHeadtrackTarget(HT::kAction,    false);
                    proc->high->ClearHeadtrackTarget(HT::kScript,    false);
                    proc->high->ClearHeadtrackTarget(HT::kCombat,    false);
                    proc->high->ClearHeadtrackTarget(HT::kDialogue,  false);
                    proc->high->ClearHeadtrackTarget(HT::kProcedure, false);
                }
                player->AsActorState()->actorState2.headTracking = 0;
            }
        }

        SyncDialogueLookLock();

        auto* playerCam = RE::PlayerCamera::GetSingleton();

        // Diagnostic: during R3 hold, log which state is active each ~15
        // frames so we can pinpoint where the engine redirects to produce
        // the "close-up 3p" visual.
        if (s_r3Held) {
            static int sTick = 0;
            if ((++sTick % 15) == 0 && playerCam) {
                auto* cur = playerCam->currentState.get();
                int stateId = cur ? static_cast<int>(cur->id) : -1;
                spdlog::info("FPStateDiag: r3Held currentStateId={} IsInFirst={} zoomInput={:.3f}",
                             stateId, playerCam->IsInFirstPerson(), playerCam->zoomInput);
            }
        }

        if (!playerCam || playerCam->currentState.get() != a_this) return;

        // Suppress the engine's R3-hold-zoom-out-to-3p auto-transition
        // while R3 is actually held. Two channels:
        //   1. Zero the zoom input accumulator so the auto-transition
        //      threshold can't arm.
        //   2. Nuke any state-transition request the original Update
        //      wrote into a_nextState â€” that's how the engine asks the
        //      camera to leave 1p. Clearing it keeps us in 1p until the
        //      release sink fires the explicit POV toggle.
        if ((s_r3Held && s_r3PressWasFirst) ||
            (s_fKeyHeld && s_fKeyPressWasFirst))
        {
            playerCam->zoomInput = 0.0f;
            if (a_nextState.get() != nullptr) {
                a_nextState.reset();
            }
        }

        const auto& settings = SettingsManager::GetSingleton();

        // World FOV (offset 0x13C) drives scene rendering while in 1p.
        // firstPersonFOV (offset 0x140) is the viewmodel (hands/weapon) FOV â€”
        // these must be set separately so the hands can be tuned independently
        // of the world.
        //
        // Dialogue-1p override: when the Dialogue Menu is open AND the 1p
        // dialogue toggle is on, the world FOV target switches to the
        // dialogueFirstPersonProfile's FOV. The actual write goes through a
        // critical-damped spring (same idiom as CameraController::springStep)
        // so transitions in/out of dialogue are smooth instead of snapping.
        // Ï‰ = 4.0 Ã— dialogueMulFOV (see omegaBaseDialogue below) â€” the
        // dialogue path keeps its own linear mul, tuned against this curve.
        // Resolve the active 1p profile (global + per-state map). FOV
        // resolution is gated on the FOV sub-toggle in the Global tab.
        // When off, fpProfile stays null and the spring decays to vanilla
        // 80 below. Dialogue keeps its own override path so it works
        // independently of the FOV gate.
        auto* fpProfile = settings.firstPersonFovEnabled
            ? SettingsManager::GetSingleton().ResolveFirstPersonFovProfile()
            : nullptr;
        float targetWorldFov = fpProfile ? fpProfile->worldFov : 80.0f;
        // [FPFOV] the adjudication instrument (2026-08-31): the user sees
        // TWO value changes per sprint edge — a third value is in the chain
        // somewhere, and with six possible sources only a log can name it.
        // One line per RESOLVED-TARGET change: the state key, the winning
        // storage, the pointer, and the fov values.
        {
            static const void* sPrevFpPtr = nullptr;
            static float       sPrevFpFov = -1.0f;
            if (fpProfile != sPrevFpPtr || (fpProfile && fpProfile->worldFov != sPrevFpFov)) {
                sPrevFpPtr = fpProfile;
                sPrevFpFov = fpProfile ? fpProfile->worldFov : -1.0f;
                static int sFpFovLog = 0;
                if (sFpFovLog < 300) {
                    ++sFpFovLog;
                    auto& setNC = SettingsManager::GetSingleton();
                    spdlog::debug("[FPFOV] key='{}' source={} ptr=0x{:x} world={:.1f} hands={:.1f} speed={:.2f}",
                                 setNC.ResolveFirstPersonStateKey(),
                                 setNC.DescribeFpProfileSource(fpProfile),
                                 reinterpret_cast<std::uintptr_t>(fpProfile),
                                 fpProfile ? fpProfile->worldFov : 0.0f,
                                 fpProfile ? fpProfile->handsFov : 0.0f,
                                 fpProfile ? fpProfile->transitionSpeed : 0.0f);
                }
            }
        }
        if (settings.dialogueFirstPersonEnabled) {
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen("Dialogue Menu")) {
                // Phase 2: prefer the active bucket preset's FOV; fall back
                // to the legacy single dialogueFirstPersonProfile.fov when
                // no bucket is active (empty buckets / pre-Phase-1 config).
                targetWorldFov = settings.ResolveDialogueProfile(true)->fov;
            }
        }

        static float sDialogueFovCurrent = 0.0f;
        static float sDialogueFovVel     = 0.0f;
        static bool  sDialogueFovInit    = false;
        static auto  sDialogueFovLastFrame = std::chrono::steady_clock::now();
        if (!sDialogueFovInit) {
            sDialogueFovCurrent   = targetWorldFov;
            sDialogueFovVel       = 0.0f;
            sDialogueFovLastFrame = std::chrono::steady_clock::now();
            sDialogueFovInit      = true;
        }
        const auto nowFovTp = std::chrono::steady_clock::now();
        float dtFov = std::chrono::duration<float>(nowFovTp - sDialogueFovLastFrame).count();
        sDialogueFovLastFrame = nowFovTp;
        dtFov = std::clamp(dtFov, 0.001f, 0.1f);
        // Per-state 1p transitions and dialogue transitions share the
        // same spring family. Dialogue uses dialogueMulFOV and keeps the
        // critical-damped 2nd-order spring it was tuned against; the
        // rest of the 1p FOV path uses a 1st-order exp-lerp so state
        // changes BEGIN moving on frame 1 instead of velocity-ramping in
        // from zero. The velocity-ramp delay of the 2nd-order spring is
        // what made non-dialogue transitions feel sluggish at start.
        // Fire-and-Forget states get a deliberately slower spring so the
        // FOV swell during a charge reads as cinematic rather than snappy.
        const bool inDialogue =
            settings.dialogueFirstPersonEnabled &&
            RE::UI::GetSingleton() &&
            RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
        // Dialogue-EXIT FOV ease. On dialogue exit the 1p world FOV must return
        // from the dialogue FOV to the gameplay 1p FOV. That return is governed
        // by the DIALOGUE FOV transition speed (dialogueMulFOV) â€” it mirrors the
        // dialogue ENTRY spring (same base Ã— same slider) so the swing OUT feels
        // like the reverse of the swing IN, and it matches the 3p side, where the
        // dialogue transition sliders already drive the exit blend. (Previously
        // the exit used the first-person STATE transition speed over a fixed 0.8s
        // window, so the dialogue speed slider had no effect on how the 1p FOV
        // left dialogue, and the exp-lerp path below also started at max velocity
        // â€” a +2Â°/frame jump on the first exit frame read as a jitter.)
        //
        // A settle-LATCH (not a fixed time window) holds the ease active until
        // the spring effectively settles: a slow dialogue speed can take several
        // seconds to complete, and a fixed window would cut it off and snap the
        // remainder via the gameplay exp-lerp. Cleared on settle or a generous
        // speed-scaled cap so a mid-exit target change can never strand it.
        static bool  sWasInDialogueFov      = false;
        static bool  sDialogueFovExiting     = false;
        static float sDialogueFovExitElapsed = 0.0f;
        if (inDialogue) {
            sDialogueFovExiting = false;         // dialogue owns the FOV
        } else if (sWasInDialogueFov) {
            sDialogueFovExiting     = true;      // falling edge â†’ begin exit ease
            sDialogueFovExitElapsed = 0.0f;
        }
        sWasInDialogueFov = inDialogue;
        // 17.0: the CRITICAL-SPRING base. [FPFOV-TRACE] 2026-08-31 showed
        // the 1st-order exp-lerp's textbook shape — fastest motion on frame
        // ONE (a ~5-unit snap) then a decelerating creep, which reads as two
        // distinct changes. The critically-damped spring starts from zero
        // velocity and settles without overshoot: one motion. 17 x speed
        // keeps time-to-90% about equal to the old exp-lerp's.
        // 8.0 (was 17, was 10): [FPFOV-TRACE] proved the curve FAMILY was
        // never the lever — at these stiffnesses half a 40-unit FOV change
        // lands inside the first ~85ms regardless of lerp vs spring, and
        // that half-then-creep IS the "two distinct changes" report.
        // DURATION is the lever: 8 x speed makes the default a single
        // legible ~1s glide; the Transition Speed slider still scales it
        // all the way to a snap at 10.
        //
        // 10.0, exp-lerp again (2026-08-30): both curve sagas above were
        // chasing a SECOND WRITER — the vanilla sprint anim driving the
        // skeleton's FOV-control node — which [FPKICK] now pins at the
        // source. With it gone, the spring's own zero-velocity start became
        // the report ("slight delay before it begins to transition back"
        // at sprint exit)... and the exp-lerp's own shape became the next
        // one ("stutter upon entering sprinting"): the 21:31:24 trace shows
        // 63% of a 30-unit change inside 81ms — a ~5-unit first-frame pop —
        // then a 400ms creep for the last five units. Snap-then-creep.
        //
        // 17.0, critical spring, CLOSED FORM (2026-08-30, the clean-world
        // retrial): every pre-[FPKICK] curve verdict was contaminated by
        // the anim kick, so ω=17 is judged on its own for the first time —
        // visible motion by ~30ms (no exit delay), zero-velocity start (no
        // entry pop), settled ~230ms (no creep). Closed form because
        // explicit Euler is only stable to ω·dt≈0.83 (the 2026-06-28
        // exact-spring session) and the Transition Speed slider reaches
        // ω=170.
        constexpr float omegaBaseFov      = 17.0f;
        constexpr float omegaBaseDialogue = 4.0f;   // dialogue spring base (entry + exit share it)
        float perStateSpeed = settings.firstPersonTransitionSpeed;
        if (fpProfile && fpProfile->transitionSpeed > 0.0f) {
            perStateSpeed = fpProfile->transitionSpeed;
        }
        // Detect FoF cells in the resolved key. We don't have the key
        // string here, but we DO know the active StateResolver cast type;
        // FoF maps to a noticeably slower spring (Ã—0.35) for the
        // cinematic build-and-release feel during a charge cast.
        float fofMul = 1.0f;
        if (fpProfile &&
            StateResolver::GetSingleton().GetCastType() == CastType::FireAndForget)
        {
            fofMul = 0.35f;
        }
        // First-person dialogue FOV rides its own spring rather than the
        // shared blend mapping.
        if (inDialogue) {
            // Keep the existing critical-damped path for dialogue â€” the
            // dialogueMulFOV slider was tuned against this curve and
            // changing it would alter the dialogue feel.
            const float speedMul = (std::max)(0.05f, settings.dialogueMulFOV);
            const float omegaFov = omegaBaseDialogue * speedMul;
            const float kFov = omegaFov * omegaFov;
            const float cFov = 2.0f * omegaFov;
            const float accelFov = kFov * (targetWorldFov - sDialogueFovCurrent) - cFov * sDialogueFovVel;
            sDialogueFovVel     += accelFov * dtFov;
            sDialogueFovCurrent += sDialogueFovVel * dtFov;
        } else if (sDialogueFovExiting) {
            // Dialogue EXIT spring â€” same base Ã— same slider as the dialogue
            // ENTRY spring above (omegaBaseDialogue Ã— dialogueMulFOV), so the
            // FOV leaves dialogue at the dialogue transition speed the user set.
            // Velocity carries over from the just-settled entry spring (~0), so
            // the return starts gently (no frame-1 lurch).
            const float speedMul = (std::max)(0.05f, settings.dialogueMulFOV);
            const float omegaFov = omegaBaseDialogue * speedMul;
            const float kFov = omegaFov * omegaFov;
            const float cFov = 2.0f * omegaFov;
            const float accelFov = kFov * (targetWorldFov - sDialogueFovCurrent) - cFov * sDialogueFovVel;
            sDialogueFovVel     += accelFov * dtFov;
            sDialogueFovCurrent += sDialogueFovVel * dtFov;
            // End the ease once the spring has effectively settled, or after a
            // generous cap that scales with the chosen speed (so a slow setting
            // gets proportionally longer before the safety cutoff, and a mid-
            // exit target change can never strand the latch).
            sDialogueFovExitElapsed += dtFov;
            const float capSecs = 4.0f *
                SettingsManager::DialogueBlendDuration(settings.dialogueMulFOV) + 1.0f;
            const bool settled = std::abs(targetWorldFov - sDialogueFovCurrent) < 0.1f &&
                                 std::abs(sDialogueFovVel) < 1.0f;
            if (settled || sDialogueFovExitElapsed > capSecs) {
                sDialogueFovExiting = false;
            }
        } else {
            // Closed-form critically damped step (see the omegaBaseFov
            // note) — exact for any omega*dt, and it shares sDialogueFovVel
            // so entering/leaving dialogue stays continuous.
            const float speedMul = (std::max)(0.05f, perStateSpeed * fofMul);
            const float omegaFov = omegaBaseFov * speedMul;
            const float eFov   = std::exp(-omegaFov * dtFov);
            const float dxFov  = sDialogueFovCurrent - targetWorldFov;
            const float tmpFov = sDialogueFovVel + omegaFov * dxFov;
            sDialogueFovCurrent = targetWorldFov + (dxFov + tmpFov * dtFov) * eFov;
            sDialogueFovVel     = (sDialogueFovVel - omegaFov * tmpFov * dtFov) * eFov;
        }

        // Fire-and-Forget cycle: charge-phase FOV pull (zoom in during
        // windup) + release-phase widen burst. Per-hand caster-state
        // detection matches the noise path so both effects land on the
        // same frame. Fire events: any transition INTO kCasting OR a
        // kReady â†’ kNone fall (handles engine paths that skip kCasting).
        using CState = RE::MagicCaster::State;
        static CState sFovLastLeftState  = CState::kNone;
        static CState sFovLastRightState = CState::kNone;
        static float  sFoFFovBurstElapsed = 999.0f;
        float fofChargeProgress = 0.0f;
        bool  fofAnyCharging    = false;
        {
            auto* fofPlayer = RE::PlayerCharacter::GetSingleton();
            using Src = RE::MagicSystem::CastingSource;
            CState curLeft  = CState::kNone;
            CState curRight = CState::kNone;
            bool leftIsFoF  = false;
            bool rightIsFoF = false;
            if (fofPlayer) {
                auto isFoFSpell = [](RE::MagicCaster* caster) -> bool {
                    if (!caster || !caster->currentSpell) return false;
                    auto* spell = caster->currentSpell->As<RE::SpellItem>();
                    if (!spell) return false;
                    return spell->GetCastingType() ==
                           RE::MagicSystem::CastingType::kFireAndForget;
                };
                auto handProgress = [](RE::MagicCaster* caster, CState cs) -> float {
                    if (cs == CState::kNone)    return 0.0f;
                    if (cs == CState::kCasting) return 1.0f;
                    if (cs == CState::kReady)   return 1.0f;
                    if (cs == CState::kUnk07 || cs == CState::kUnk08 || cs == CState::kUnk09)
                        return 0.0f;
                    // castingTimer counts DOWN from chargeTime â†’ 0
                    // during the windup; invert for progress.
                    float raw = 0.0f;
                    if (caster && caster->currentSpell) {
                        if (auto* spell = caster->currentSpell->As<RE::SpellItem>()) {
                            const float ct = (std::max)(0.1f, spell->GetChargeTime());
                            raw = 1.0f - std::clamp(caster->castingTimer / ct, 0.0f, 1.0f);
                        }
                    }
                    return (std::max)(raw, 0.4f);
                };
                auto isCharging = [](CState s) {
                    return s != CState::kNone &&
                           s != CState::kCasting &&
                           s != CState::kReady &&
                           s != CState::kUnk07 && s != CState::kUnk08 && s != CState::kUnk09;
                };
                for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                    auto* caster = fofPlayer->GetMagicCaster(src);
                    if (!caster) continue;
                    const auto cs = caster->state.get();
                    const bool fof = isFoFSpell(caster);
                    if (src == Src::kLeftHand) { curLeft = cs; leftIsFoF = fof; }
                    else                       { curRight = cs; rightIsFoF = fof; }
                    if (fof) {
                        fofChargeProgress = (std::max)(fofChargeProgress, handProgress(caster, cs));
                        if (isCharging(cs) || cs == CState::kReady) fofAnyCharging = true;
                    }
                }
            }
            auto isCasting = [](CState s) { return s == CState::kCasting; };
            // Gate each edge on the hand currently holding an FoF spell.
            const bool leftFire  = leftIsFoF  && !isCasting(sFovLastLeftState)  && isCasting(curLeft);
            const bool rightFire = rightIsFoF && !isCasting(sFovLastRightState) && isCasting(curRight);
            const bool leftRel   = leftIsFoF  && sFovLastLeftState  == CState::kReady && curLeft  == CState::kNone;
            const bool rightRel  = rightIsFoF && sFovLastRightState == CState::kReady && curRight == CState::kNone;
            if (leftFire || rightFire || leftRel || rightRel) {
                sFoFFovBurstElapsed = 0.0f;
            }
            sFovLastLeftState  = curLeft;
            sFovLastRightState = curRight;
        }
        // Cannon-impulse FOV widen on the release frame. Bigger peak
        // (+14Â°, was +8Â°) and a sharper, shorter curve â€” quartic
        // ease-out instead of quadratic so the very first frame hits
        // the full peak and decays away in ~0.35s instead of 0.45s.
        // That's the "release of pressure" beat that sells the cast as
        // a cannon shot rather than a fireball squirt.
        // Recoil-style release: subtle FOV widen at the firing frame,
        // quick decay. ~3.5Â° peak reads as "the cast had a little kick"
        // without dragging the eye through a noticeable zoom swing.
        // Quartic ease-out so the very first frame is the peak and the
        // tail dies away inside ~0.25s â€” a flicker, not a movement.
        constexpr float kFovBurstDuration = 0.25f;
        constexpr float kFovBurstPeakDeg  = 3.5f;
        float fovBurst = 0.0f;
        if (sFoFFovBurstElapsed < kFovBurstDuration) {
            sFoFFovBurstElapsed += dtFov;
            const float t = sFoFFovBurstElapsed / kFovBurstDuration;
            const float inv = 1.0f - t;
            const float envelope = (inv * inv) * (inv * inv);
            fovBurst = kFovBurstPeakDeg * envelope;
        }
        // Suppress reference to fofAnyCharging / fofChargeProgress so
        // the unused-variable warning stays quiet now that the charge
        // pull is gone.
        (void)fofAnyCharging;
        (void)fofChargeProgress;

        const float effectiveWorldFov = sDialogueFovCurrent + fovBurst;
        // Defer to Improved Camera SE when present. ICSE's UpdateCamera
        // trampoline at PlayerCamera::Update+0x1A6 runs AFTER this hook
        // each frame and writes its own per-state FOV, so our value would
        // be silently overwritten. Skip the write so we're not fighting.
        // The UI in MenuUI::RenderFirstPerson hides the sliders in this
        // case too.
        static const bool icseLoaded =
            GetModuleHandleA("ImprovedCameraSE.dll") != nullptr ||
            GetModuleHandleA("ImprovedCameraSE-NG.dll") != nullptr;
        // Hands FOV â€” same 1st-order exp-lerp as the world FOV path so
        // viewmodel transitions begin on frame 1. Omega matches the
        // active (non-dialogue) world-FOV omega.
        static float sHandsFovCurrent = 0.0f;
        static float sHandsFovVel     = 0.0f;
        static bool  sHandsFovInit    = false;
        const float targetHandsFov = fpProfile ? fpProfile->handsFov : 80.0f;
        if (!sHandsFovInit) {
            sHandsFovCurrent = targetHandsFov;
            sHandsFovInit    = true;
        }
        // Same closed-form critical spring as the world FOV so the two
        // channels move as one.
        const float handsSpeedMul = (std::max)(0.05f, perStateSpeed * fofMul);
        const float handsOmega    = omegaBaseFov * handsSpeedMul;
        const float handsE   = std::exp(-handsOmega * dtFov);
        const float handsDx  = sHandsFovCurrent - targetHandsFov;
        const float handsTmp = sHandsFovVel + handsOmega * handsDx;
        sHandsFovCurrent = targetHandsFov + (handsDx + handsTmp * dtFov) * handsE;
        sHandsFovVel     = (sHandsFovVel - handsOmega * handsTmp * dtFov) * handsE;

        // (The round-7b "menu window" write gate that lived here was REMOVED
        // 2026-08-31 same day: with an UNPAUSED tween the camera never parks
        // in kTween — this state keeps ticking as the LIVE camera — and
        // gating its writes silenced the only 1p FOV applier while 3p's
        // ApplyFOV kept running, which was itself the "fov snap in first
        // person but not third" report. When this hook ticks, it owns the 1p
        // FOV, menu open or not; the parked-kTween case never reaches it.)
        if (!icseLoaded && !s_mapMenuOpen) {
            // Same invariant as CameraController::ApplyFOV: a worldFOV <= 0
            // inverts the projection (inside-out) and never self-heals. The 1p
            // exp-lerp can't normally undershoot a positive target, but the
            // dialogue 2nd-order spring branch could overshoot â€” floor it so
            // this path can't flip the world either.
            // (s_mapMenuOpen: while the World Map is up the map-FOV guard
            // owns worldFOV â€” writing the dialogue/profile FOV here on the
            // map's opening frame re-latched the zoomed dialogue FOV into
            // the map render. Springs keep ticking; writes resume on close.)
            // [FPFOV-TRACE] two detectors, both throttled + capped:
            // (1) a mid-transition sampler — the SHAPE of the move (a
            //     two-phase curve means the speed or target changed in
            //     flight); (2) a foreign-writer check — if the value we
            //     wrote LAST frame isn't what the engine holds now,
            //     something else is also writing worldFOV.
            {
                static float sPrevWritten  = -1.0f;
                static auto  sLastTraceTp  = std::chrono::steady_clock::now();
                static int   sTraceLines   = 0;
                const float renderedNow = playerCam->worldFOV;
                const bool foreign = sPrevWritten > 0.0f &&
                                     std::fabs(renderedNow - sPrevWritten) > 0.3f;
                const bool moving  = std::fabs(sDialogueFovCurrent - targetWorldFov) > 0.3f;
                const auto nowTp   = std::chrono::steady_clock::now();
                const bool due     = std::chrono::duration<float>(nowTp - sLastTraceTp).count() > 0.08f;
                if (sTraceLines < 400 && ((moving && due) || foreign)) {
                    ++sTraceLines;
                    sLastTraceTp = nowTp;
                    // The FRUSTUM is what the renderer actually displays —
                    // playerCam->worldFOV is only what we ask for. If these
                    // ever disagree beyond the write lag, something
                    // composes FOV downstream of us and THAT is the second
                    // visible change.
                    float frustumDeg = 0.0f;
                    if (playerCam->cameraRoot) {
                        if (auto* rootN = playerCam->cameraRoot->AsNode()) {
                            for (auto& chN : rootN->GetChildren()) {
                                if (auto* ncam = skyrim_cast<RE::NiCamera*>(chN.get())) {
                                    const auto& fr = ncam->viewFrustum;
                                    if (fr.fNear > 0.0001f)
                                        frustumDeg = (std::atan(-fr.fLeft / fr.fNear) +
                                                      std::atan(fr.fRight / fr.fNear)) * 57.29578f;
                                    break;
                                }
                            }
                        }
                    }
                    spdlog::debug("[FPFOV-TRACE] target={:.1f} current={:.1f} rendered={:.1f} frustum={:.1f}{} "
                                 "hands {:.1f}->{:.1f} omega={:.1f}",
                                 targetWorldFov, sDialogueFovCurrent, renderedNow, frustumDeg,
                                 foreign ? " FOREIGN-WRITE" : "",
                                 sHandsFovCurrent, targetHandsFov,
                                 omegaBaseFov * (std::max)(0.05f, perStateSpeed * fofMul));
                }
                sPrevWritten = (std::max)(1.0f, effectiveWorldFov);
            }
            playerCam->worldFOV       = (std::max)(1.0f, effectiveWorldFov);
            playerCam->firstPersonFOV = sHandsFovCurrent;
            s_fpLastWrittenFov        = playerCam->worldFOV;

            // One-shot cleanup of legacy persistence: previous builds
            // wrote the hands FOV into the `fDefault1stPersonHandFOV`
            // GameSetting, which saves into the player's .ess. Reset
            // it to vanilla 80 once per session so future saves drop
            // any custom value the user inherited from older builds.
            // The per-frame `firstPersonFOV` write above keeps the
            // actual rendered FOV at the user's choice regardless.
            static bool sHandsFovSettingResetDone = false;
            if (!sHandsFovSettingResetDone) {
                if (auto* gsc = RE::GameSettingCollection::GetSingleton()) {
                    if (auto* setting = gsc->GetSetting("fDefault1stPersonHandFOV")) {
                        setting->data.f = 80.0f;
                    }
                }
                sHandsFovSettingResetDone = true;
                spdlog::info("HookManager: reset fDefault1stPersonHandFOV GameSetting to vanilla 80");
            }
        }

        // 1p dialogue face-lock lives in HookedUpdateCameraPost
        // (PlayerCamera::Update+0x1A6) â€” the critical-damped spring there
        // writes the final camera matrix AND data.angle directly. The
        // earlier 1p face-lock block at this site (IsNPC=true,
        // IdleForceDefaultState, smoothed-target tracking) was removed in
        // favor of the matrix-override approach: the anim-graph writes
        // caused a one-frame visual glitch on entry, and the matrix
        // override dominates rendering regardless of the head-track anim,
        // so IsNPC suppression is unnecessary.

        // Hands FOV: per-frame write to `playerCam->firstPersonFOV`
        // above is sufficient. Previously we ALSO mirrored the value
        // into the `fDefault1stPersonHandFOV` GameSetting so the
        // engine wouldn't clobber it on weapon swaps â€” but
        // GameSettingCollection writes persist in save files, so
        // disabling the mod left the user's save with our custom
        // value baked in. We accept a one-frame flash on weapon-
        // swap re-entry in exchange for clean save state.
        //
        // The INI mirror (process-local, doesn't persist to disk) is
        // also dropped for simplicity; the per-frame field write is
        // the authoritative source.

        static bool logged = false;
        if (!logged) {
            spdlog::info("HookManager: FirstPersonState::Update hook fired (global worldFov={:.1f} handsFov={:.1f})",
                         settings.firstPersonGlobal.worldFov,
                         settings.firstPersonGlobal.handsFov);
            logged = true;
        }
    }

    // Critical-damped spring state for the 1p face-lock yaw/pitch â€”
    // matches the mod's transition architecture (CameraController::springStep).
    //
    // Entry: spring initializes from the engine's current rendered camera
    // orientation and targets the face each frame â€” eases the transition.
    // Active: target tracks NPC head bone position each frame; the boost
    // on stiffness keeps lag imperceptible.
    // Exit: after the speaker gate releases, the spring keeps running for
    // kReleaseSec targeting player->data.angle (which we've been writing
    // to the face position, so the spring stays at face unless the player
    // moves mouse â€” in which case it smoothly retargets toward input).
    // This avoids a hard switch-off jump even if the engine's rendering
    // differs from our last overwrite.
    static inline float s_dialogueFaceLockYaw     = 0.0f;
    static inline float s_dialogueFaceLockPitch   = 0.0f;
    static inline float s_dialogueFaceLockVelYaw   = 0.0f;
    static inline float s_dialogueFaceLockVelPitch = 0.0f;
    // s_dialogueFaceLockWasActive declared earlier (forward-declared near
    // s_dialogueMenuOpen so HookedFirstPersonUpdate can read it).
    static inline bool  s_dialogueFaceLockReleasing = false;
    static inline float s_dialogueFaceLockExitYaw   = 0.0f;
    static inline float s_dialogueFaceLockExitPitch = 0.0f;
    static inline std::chrono::steady_clock::time_point s_dialogueFaceLockLastFrame{};
    static inline std::chrono::steady_clock::time_point s_dialogueFaceLockReleaseStart{};
    static constexpr float kDialogueFaceLockReleaseSec = 0.3f;

    // Smooth transition state. Spring keeps tight stiffness (7Ã— boost) for
    // active face-tracking; transitions are produced by lerping the spring's
    // *target* over the entry/exit windows so the spring chases a smoothly-
    // moving target. This matches the rest of the mod's transition feel.
    //
    // Pre-dialogue snapshot: cameraNI col0 captured on the very first
    // speaker-edge of a conversation. This is "where you were looking
    // before activating" and serves as the exit lerp endpoint, replacing
    // the frozen-hold release. Stable because cameraNI col0 is the
    // rendered orientation (not lagged like data.angle).
    //
    // Entry transition: cameraNI col0 captured on every speaker-edge
    // (fresh entry OR re-entry mid-grace). Serves as the entry lerp start.
    static inline bool  s_preDialogueValid    = false;
    static inline float s_preDialogueYaw      = 0.0f;
    static inline float s_preDialoguePitch    = 0.0f;
    static inline bool  s_entryTransActive    = false;
    static inline float s_entryStartYaw       = 0.0f;
    static inline float s_entryStartPitch     = 0.0f;
    static inline std::chrono::steady_clock::time_point s_entryTransStart{};
    static constexpr float kDialogueEntryTransSec = 0.3f;

    // Trampoline body at PlayerCamera::Update +0x1A6 (RelocationID 49852/50784).
    // This runs AFTER the per-state camera pipeline (pitch spring, rotation
    // resolvers, etc.) has written cameraNI->world.rotate for the frame.
    // Writing here is the last chance to override before the renderer
    // samples the camera orientation.
    void HookManager::HookedUpdateCameraPost(RE::TESCamera* a_camera)
    {
        TweenCameraTrace::StageSample postCameraTrace("post-camera-exit");
        // Hoisted singleton â€” was previously fetched multiple times in
        // this hook (suspend check, settings ref). Same value every call
        // so we hold one reference for the duration.
        auto& settings = SettingsManager::GetSingleton();

        // [SNAP] preX â€” read the lateral offset BEFORE the engine's own camera
        // pass runs, so the dump can say whether the engine rewrites the field
        // mid-frame. SnapTick below samples the post-pass value.
        if (a_camera && a_camera->currentState) {
            if (auto* pcam = RE::PlayerCamera::GetSingleton();
                pcam && a_camera->currentState.get() ==
                            pcam->cameraStates[RE::CameraState::kThirdPerson].get()) {
                s_snapPreUpdSideX =
                    static_cast<RE::ThirdPersonState*>(a_camera->currentState.get())
                        ->posOffsetActual.x;
            }
        }

        // [TWEENX] bracket B - top of the post hook, above SnapTick.
        TweenXSampleLat(s_tweenXUcpRoot, s_tweenXUcpTrans);

        if (auto* pcTf = RE::PlayerCamera::GetSingleton()) s_tfUcpIn = pcTf->worldFOV;
        VanityCamera::Tick();
        _originalUpdateCameraPost(a_camera);

        // Unsupported states can run without ThirdPersonState::Update. Clear
        // the mounted tail here, but do not reset the state-update sample clock
        // or steady-tracking latch at this later camera stage.
        if (!a_camera || !a_camera->currentState ||
            (a_camera->currentState->id != RE::CameraState::kMount &&
             a_camera->currentState->id != RE::CameraState::kThirdPerson)) {
            s_mountedLockPitch.Reset();
        }

        // Sample every frame, report only each 30-frame window. Peaks preserve
        // short jolts that a periodic snapshot would miss. Translation is the
        // completed native solver result, relative to the mount's world root.
        {
            static std::uint64_t lastFrame = 0;
            static unsigned samples = 0, rows = 0;
            static bool hadPrevious = false;
            static RE::NiPoint3 previousRelative{};
            static float peakTravel = 0.0f, peakDt = 0.0f;
            const bool lockedMount = a_camera && a_camera->currentState &&
                a_camera->currentState->id == RE::CameraState::kMount &&
                TDMIntegration::GetSingleton().IsTargetLocked();
            if (!lockedMount) {
                hadPrevious = false;
                samples = 0;
                peakTravel = peakDt = 0.0f;
            } else if (rows < 240 && s_mountedLockFrame != lastFrame && s_mountedLockDt > 0.0f) {
                lastFrame = s_mountedLockFrame;
                auto* state = static_cast<RE::HorseCameraState*>(a_camera->currentState.get());
                if (auto mount = state->horseRefHandle.get()) {
                    const auto relative = state->translation - mount->GetPosition();
                    if (hadPrevious && s_mountedLockDt <= 0.25f)
                        peakTravel = (std::max)(peakTravel, (relative - previousRelative).Length());
                    previousRelative = relative;
                    hadPrevious = true;
                    peakDt = (std::max)(peakDt, s_mountedLockDt);
                    if (++samples >= 30) {
                        ++rows;
                        spdlog::debug("[MOUNTTRACK2-POS] frame={} relative=({:.1f},{:.1f},{:.1f}) "
                                     "peakFrameTravel={:.2f} peakDt={:.4f}",
                            lastFrame, relative.x, relative.y, relative.z, peakTravel, peakDt);
                        samples = 0;
                        peakTravel = peakDt = 0.0f;
                    }
                } else hadPrevious = false;
            }
        }

        if (auto* camera = RE::PlayerCamera::GetSingleton();
            !camera || !camera->currentState ||
            (camera->currentState->id != RE::CameraState::kThirdPerson &&
             camera->currentState->id != RE::CameraState::kTween)) {
            ResetMenuCameraAnimation();
        }

        // [TWEENX] bracket C - what the engine's own continuation left behind.
        TweenXSampleLat(s_tweenXEngRoot, s_tweenXEngTrans);

        // ----- Tween-menu FOV hold -----------------------------------------
        // The engine parks the camera in kTween to render the Tween / Favorites
        // wheel. CameraController::Update does not tick in that state, so the
        // profile FOV stops being asserted and whatever worldFOV the menu camera
        // wants renders instead â€” a hard cut on open and a second one on close.
        // With the Show Player In Menus entry left unconfigured nothing else is
        // holding the camera either, so both cuts are bare. That is the "bad FOV
        // transition when not configured at all".
        //
        // Position in kTween belongs to the engine's menu camera and is not
        // ours to take without the SetState the unconfigured path deliberately
        // avoids (re-entering Begin resets freeRotation â€” the documented Tween
        // open-flicker). FOV is different: worldFOV is a plain field on
        // PlayerCamera, independent of which state is driving position, so
        // holding it costs nothing and cannot snap anything.
        //
        // Skipped while Show Player In Menus is active â€” that path saves and
        // restores worldFOV itself, and two writers would fight.
        // Holds s_menuFovFreeze — the FOV rendering the frame before the menu
        // opened — NOT GetLastAppliedWorldFov: that is the 3p pipeline's last
        // value, stale whenever the Tween opens from first person (or any
        // state 3p wasn't ticking in), and asserting it was itself the
        // "flicker snap when menus aren't configured" (user, 2026-08-31). A
        // pause freezes what was on screen; it does not consult a resolver.
        if (a_camera && a_camera->currentState &&
            a_camera->currentState->id == RE::CameraState::kTween &&
            !ShowPlayerInMenusController::GetSingleton().IsActive() &&
            !settings.diagnosticSuspendOverrides) {
            // worldFOV lives on PlayerCamera, not the TESCamera base this hook
            // is handed â€” and the Tween camera is always the player's.
            if (auto* pcamTween = RE::PlayerCamera::GetSingleton();
                pcamTween && s_menuFovFreeze > 1.0f &&
                pcamTween->worldFOV != s_menuFovFreeze) {
                pcamTween->worldFOV = s_menuFovFreeze;
            }
        }
        if (auto* pcTf = RE::PlayerCamera::GetSingleton()) s_tfUcpOut = pcTf->worldFOV;

        // Tween-EXIT one-frame restore ("there's a 1 frame flicker",
        // 2026-09-01). The forced exit lands the return state's Begin inside
        // this same engine pass, and Begin writes vanilla framing/fov that
        // would render for exactly one frame before the normal pipelines
        // reassert — the draw-edge ordering fact, so the draw-edge guard's
        // remedy: restore the last-applied values here, same pass.
        {
            static bool sWasTweenState = false;
            const bool isTweenState = a_camera && a_camera->currentState &&
                a_camera->currentState->id == RE::CameraState::kTween;
            if (sWasTweenState && !isTweenState && a_camera && a_camera->currentState &&
                !ShowPlayerInMenusController::GetSingleton().IsActive() &&
                !settings.diagnosticSuspendOverrides) {
                // FOV: the freeze IS the pre-open value both pipelines
                // resume to next frame — hold it across the gap.
                if (auto* pcamX = RE::PlayerCamera::GetSingleton();
                    pcamX && s_menuFovFreeze > 1.0f) {
                    pcamX->worldFOV = s_menuFovFreeze;
                }
                // 3p framing: identical restore (and identical mounted
                // exclusion) to the draw-edge guard.
                if (a_camera->currentState->id == RE::CameraState::kThirdPerson) {
                    const auto stX = StateResolver::GetSingleton().GetState();
                    float x = 0, y = 0, z = 0, zt = 0, zc = 0;
                    if (stX != CameraState::Horseback && stX != CameraState::DragonRiding &&
                        CameraController::GetLastAppliedFraming(x, y, z, zt, zc)) {
                        if (auto* tpsX = skyrim_cast<RE::ThirdPersonState*>(
                                a_camera->currentState.get())) {
                            tpsX->posOffsetExpected.x = x;
                            tpsX->posOffsetExpected.y = y;
                            tpsX->posOffsetExpected.z = z;
                            tpsX->posOffsetActual.x   = x;
                            tpsX->posOffsetActual.y   = y;
                            tpsX->posOffsetActual.z   = z;
                            tpsX->targetZoomOffset    = zt;
                            tpsX->currentZoomOffset   = zc;
                        }
                    }
                }
            }
            sWasTweenState = isTweenState;
        }

        // ----- Bow-zoom (Eagle Eye) FOV hold -------------------------------
        // WHY HERE and not only in HookedNiCameraUpdateWorldData, where this
        // was first tried: ApplyFOV runs from HookedThirdPersonUpdate, i.e.
        // INSIDE ThirdPersonState::Update, and the engine applies its own
        // Eagle Eye FOV afterwards in PlayerCamera::Update â€” so our value is
        // overwritten every single frame before anything renders. The NiCamera
        // pass is later than the engine's write but later than the frustum
        // build too, so re-asserting there only ever landed for the NEXT
        // frame, which the engine then overwrote again: the user saw vanilla
        // zoom every time, exactly as reported ("when drawing and zoomed have
        // the same settings, it still falls back to the vanilla amount").
        //
        // This hook is the first point AFTER the engine's camera pass, which
        // is the same reason the Tween hold above lives here.
        //
        // FIRST PERSON IS EXCLUDED: CameraController::Update does not tick
        // there, so GetLastAppliedWorldFov would be a stale third-person
        // value, and the 1p path applies its own FOV in the late NiCamera
        // hook. Holding here would clobber it with the wrong number.
        if (a_camera && !settings.diagnosticSuspendOverrides &&
            !ShowPlayerInMenusController::GetSingleton().IsActive())
        {
            auto* pcamZoom = RE::PlayerCamera::GetSingleton();
            // Both flags, same reasoning as the NiCamera hold: the engine's
            // covers the frames it is actively writing its zoom, the
            // resolver's additionally covers the attack-lag window where
            // Projectile Lag is still holding the shot framing.
            const bool zoomHeld = pcamZoom &&
                (pcamZoom->bowZoomedIn || StateResolver::GetSingleton().IsBowZoomed());
            if (zoomHeld && !pcamZoom->IsInFirstPerson()) {
                bool zfValid = false;
                const float zoomFov = CameraController::GetLastAppliedWorldFov(zfValid);
                if (zfValid && zoomFov > 1.0f && pcamZoom->worldFOV != zoomFov) {
                    pcamZoom->worldFOV = zoomFov;
                }
            }
        }

        // TDM yaw-control safety release on a universal per-frame hook (this
        // fires in every camera state, unlike CameraController::Update which
        // only ticks in third person). Without this, locking off while in first
        // person / mid-transform leaves TDM owning the yaw -> the player can
        // cast but cannot move. No-op unless the lock took yaw and it's gone.
        CameraController::ReleaseLeakedLockYaw();

        // Keep the environment fresh in EVERY camera state. The other write of
        // settings.indoorMode lives in HookedThirdPersonUpdate, which only ticks
        // in third person â€” so when that hook isn't running (1p, transform, a
        // paused menu overlay), indoorMode goes stale and edits/resolution land
        // in the wrong env map (e.g. Quick Tune noise editing the indoor profile
        // while you're actually outdoors). This universal hook can't go stale.
        if (auto* cellPlayer = RE::PlayerCharacter::GetSingleton()) {
            if (auto* parentCell = cellPlayer->GetParentCell()) {
                settings.indoorMode = parentCell->IsInteriorCell();
            }
        }
        // Same reasoning for the active location â€” it must be as fresh as
        // indoorMode, since a place's appliesIndoors combines the two.
        LocationDetector::GetSingleton().Update();

        // [SNAP] â€” one camera sample per frame, in every camera state.
        {
            static auto sSnapLast = std::chrono::steady_clock::now();
            const auto  snapNow   = std::chrono::steady_clock::now();
            float snapDt = std::chrono::duration<float>(snapNow - sSnapLast).count();
            sSnapLast = snapNow;
            SnapTick(std::clamp(snapDt, 0.0f, 0.25f));
            TweenRawPrint("POST");
            // [PITCHX] â€” height-vs-aim coupling. Silent unless Height moves.
            PitchXTick();
            // Both silent unless armed by their own edge.
            DrawZTick();
            TweenCamTick();
        }


        // Werewolf is third person only. The beast form has no first-person
        // rig â€” vanilla gives you a floating view with no arms, most animation
        // packs never author 1p beast anims at all, and every camera setting
        // DDC exposes for the form is a third-person framing. So take the POV
        // back the moment anything puts the player in first person while
        // transformed, whether that was the POV key, a scripted forced view, or
        // an auto-switch out of a killmove.
        //
        // Checked on this universal hook rather than on the POV toggle: a
        // transformation that HAPPENS while already in first person has no
        // toggle to intercept, and neither does a script-driven switch.
        // ForceThirdPerson is idempotent, so the steady-state cost is one bool
        // read per frame.
        if (StateResolver::GetSingleton().IsWerewolf()) {
            if (auto* wwCam = RE::PlayerCamera::GetSingleton(); wwCam && wwCam->IsInFirstPerson()) {
                static bool sLoggedWwPov = false;
                if (!sLoggedWwPov) {
                    sLoggedWwPov = true;
                    spdlog::debug("[WWPOV] first person entered in werewolf form; forcing third person");
                }
                wwCam->ForceThirdPerson();
            }
        }

        // Target lock must not fire from inside an item-preview menu.
        //
        // Blocking TogglePOVHandler was never going to do it: True Directional
        // Movement reads the target-lock key from its OWN raw input sink on
        // BSInputDeviceManager, so it never touches the PlayerInputHandler
        // chain DDC patches. Its source guards only on `ui->GameIsPaused()` and
        // `!controlMap->IsMovementControlsEnabled()`, and DDC's own Unpause
        // feature makes the first false â€” so an unpaused inventory looks like
        // ordinary gameplay to TDM and R3-to-inspect acquires a lock.
        //
        // Clearing the movement flag to satisfy TDM's second guard was TRIED
        // AND REVERTED: that flag is what lets the player walk with a menu
        // open, which is a feature of the Unpause system, not a side effect.
        //
        // The right lever is TDM's own API. Per its source, UpdateTargetLock()
        // calls ToggleTargetLock(false) whenever GetForceDisableTargetLock() is
        // set â€” so holding that claim doesn't hide a lock, it makes one
        // impossible to keep. Claim it on the menu's OPEN EDGE, and only when
        // the player wasn't already locked onto something: a lock they
        // deliberately established before opening the menu is theirs, and
        // survives untouched.
        // TDM V5 exposes a force-disable claim that makes a lock impossible to
        // keep; take it when the build supports it. This machine's TDM
        // negotiates V4 ([TDM: IVTDM4 acquired] in the log), where the call is a
        // no-op â€” which is why it is only half the fix. The other half is the
        // press-instant guard in R3ReleaseSink; see there.
        {
            static bool sItemMenuWasUp   = false;
            static bool sHeldMenuLockOff = false;
            static bool sLockAtMenuOpen  = false;
            const bool  itemMenuUp = UnpauseManager::IsItemPreviewMenuOpen();
            auto&       tdm        = TDMIntegration::GetSingleton();
            if (itemMenuUp && !sItemMenuWasUp) {
                sLockAtMenuOpen = tdm.IsAvailable() && tdm.GetRawTargetLockState();
                if (tdm.IsAvailable() && !sLockAtMenuOpen) {
                    sHeldMenuLockOff = tdm.RequestDisableTargetLock();
                }
            } else if (!itemMenuUp && sItemMenuWasUp) {
                if (sHeldMenuLockOff) {
                    tdm.ReleaseDisableTargetLock();
                    sHeldMenuLockOff = false;
                }
                // Verdict line: did a lock appear during the menu that wasn't
                // there when it opened? One log answers whether the guard held,
                // instead of another round of asking.
                if (tdm.IsAvailable() && !sLockAtMenuOpen && tdm.GetRawTargetLockState()) {
                    spdlog::warn("[MENULOCK] a target lock was acquired while an item menu was "
                                 "open â€” the press-instant guard did not hold on this setup");
                }
            }
            sItemMenuWasUp = itemMenuUp;
        }

        // Hold ControlMap's movement flag OFF for as long as an item-preview
        // menu is up. That is the guard TDM honours, and holding it for the
        // whole window removes the timing question entirely.
        //
        // Clearing it only for the instant of the press was tried and FAILED:
        // it assumed DDC's input sink is notified before TDM's, and it isn't â€”
        // SKSE's kInputLoaded message fires before kDataLoaded, so a plugin
        // that registers its input sink at kInputLoaded is ahead of DDC no
        // matter what the DLLs are called. The user saw the lock fire on the
        // press that ENDED an inspection, which is exactly that race being lost.
        //
        // Walking keeps working because MovementHandler's CanProcess hook
        // answers for the flag while this window is open â€” see the carve-out
        // in UnpauseManager's InputHandlerHook. The flag is only ever restored
        // if WE cleared it, so another mod's claim survives.
        // [SLOWMO-LEAK] Global time multiplier watchdog.
        //
        // The death / ragdoll camera slows the whole world down by writing
        // BSTimer's global time multiplier, and the driver that walks it back
        // to 1.0 lives in HookedBleedoutUpdate â€” which only ticks while the
        // camera is actually in BleedoutCameraState. There is a safety restore
        // too, but it sits in HookedThirdPersonUpdate, and THAT only ticks in
        // THIRD PERSON. So any path that leaves bleedout without the state's
        // End() firing, while the player is in first person â€” getting up from
        // a ragdoll in 1p, or reloading a save from a 1p death â€” left the
        // multiplier wherever the ramp had got to. The world then runs at a
        // fraction of speed indefinitely, which is the long-standing "frozen in
        // place": not stuck, crawling.
        //
        // This hook is universal â€” every camera state, both views â€” so it is
        // the right place for the guard. Scoped to OUR flag only, so a Slow
        // Time shout or another mod's slow-mo is never touched.
        if (s_deathSlowmoActive) {
            const bool inBleedout = a_camera && a_camera->currentState &&
                                    a_camera->currentState->id == RE::CameraState::kBleedout;
            if (!inBleedout) {
                float leaked = 1.0f;
                if (auto* t = RE::BSTimer::GetSingleton()) leaked = t->QGlobalTimeMultiplier();
                RestoreDeathSlowmo();
                spdlog::warn("[SLOWMO-LEAK] time multiplier was left at {:.3f} outside bleedout "
                             "(camera state {}) â€” restored to 1.0", leaked,
                             a_camera && a_camera->currentState
                                 ? static_cast<int>(a_camera->currentState->id) : -1);
            }
        }

        // [FREEZE] "Frozen in place" watchdog.
        //
        // A long-standing, intermittent report: the player stops being able to
        // move. It has been guessed at more than once (a leaked time
        // multiplier, a blocked movement flag) and guessing has not closed it,
        // so this measures instead. Every mechanism DDC owns that can stop the
        // player walking is checked and dumped in ONE line the moment the
        // symptom is detectable â€” a leaked TDM yaw claim (TDM's ProcessInput
        // early-returns while a plugin owns yaw: "can cast but can't walk"), a
        // leaked directional-movement disable, the menu-framing controller
        // stuck active (it pins body yaw every frame AND legitimises the yaw
        // claim, so the existing orphan watchdog won't reap it), a cleared
        // movement flag, or a slowed clock.
        //
        // Detection: real movement input, no meaningful position change for a
        // while, and nothing that legitimately pins the player. Walking into a
        // wall trips it too â€” harmless, the line is rate-limited and the
        // repairs below only ever release claims that are provably orphaned.
        {
            static RE::NiPoint3 sFrzPrevPos{};
            static bool  sFrzInit    = false;
            static float sFrzStuckT  = 0.0f;
            static int   sFrzLogs    = 0;
            static auto  sFrzLast    = std::chrono::steady_clock::now();
            // ONSET/RESOLUTION tracking â€” the gap that made prior [FREEZE]
            // lines useless for telling a 1.6s wall-bump apart from a real
            // multi-second freeze: the old code re-armed sFrzStuckT to 0
            // after every log, so each line looked like an independent
            // fresh 1.5s event with no way to see it was the SAME episode
            // continuing, and RESOLUTION (movement resuming) was never
            // logged at all. Now: log once at onset, log again on every
            // re-fire with elapsed-since-onset (not reset), and log once
            // when movement resumes with the total duration â€” so severity
            // is legible from the log alone instead of guessed from line
            // count.
            static bool  sFrzEpisodeActive = false;
            static auto  sFrzEpisodeStart  = std::chrono::steady_clock::now();
            static int   sFrzEpisodeFires  = 0;
            // Per-frame input tally across the episode (2026-09-05). The
            // fires sample the stick every 1.5s, so seven fires reading
            // raw=0 could not tell a centred stick from one tapped between
            // samples - and every episode in the 09:42 log was exactly that
            // question. Count every frame instead, and remember how long ago
            // the DDC menu last closed ("it seems to be tied to opening our
            // menus").
            static int   sFrzEpFrames = 0, sFrzEpRawFrames = 0, sFrzEpEngFrames = 0, sFrzEpBodyFrames = 0;
            static float sFrzEpRawMax = 0.0f, sFrzEpEngMax = 0.0f;
            static float sFrzSinceDdcClose = 999.0f;
            static bool  sFrzPrevSmfBlock  = false;
            // PRE-ONSET HISTORY (2026-08-19). Every [FREEZE] line so far
            // describes the world 1.5 SECONDS AFTER the freeze began â€” by
            // which time whatever changed at the onset frame has long since
            // settled, and all the line can honestly report is "everything
            // looks normal and the player still isn't moving". That is why
            // four investigations in a row ended at "cause outside all
            // tracked claims": nobody has ever seen the onset frame.
            //
            // So keep a ring of the last ~2s of cheap per-frame samples and
            // replay it ONCE when an episode opens. Nothing is formatted
            // unless an episode actually starts.
            struct FrzSample
            {
                float rawMag = 0.0f, engMag = 0.0f, tdmMag = 0.0f;
                float ccVel = 0.0f, ccSpeedPct = 0.0f;
                float posDelta = 0.0f, timeMult = 1.0f;
                int   ccState = -1, asMoveBits = 0, camState = -1;
                std::uint32_t ccFlags = 0;
                bool  engBlocked = false, plyMoving = false, locked = false;
                bool  moveCtl = true, gateOpen = false;
            };
            constexpr int kFrzRing = 128;            // ~2s at 60fps, ~1s at 120
            static FrzSample sFrzRing[kFrzRing]{};
            static int       sFrzRingN  = 0;
            static int       sFrzDumps  = 0;         // whole-session budget
            constexpr int    kFrzMaxDumps = 8;
            // The history is COPIED at onset but PRINTED only once the episode
            // proves it isn't a wall-bump. Every episode in the 2026-08-19 log
            // was under 3.6s and every one of them had a mundane explanation;
            // printing 128 lines for each would burn the budget before the real
            // freeze arrived â€” the same self-muting that cost the 21:59 log its
            // one real capture. Snapshot cheap, print late.
            static FrzSample sFrzSnap[kFrzRing]{};
            static int       sFrzSnapN       = 0;
            static bool      sFrzSnapPending = false;
            constexpr float  kFrzHistorySec  = 3.0f;
            // The VERDICT block is four lines, not 128, so it gets its own
            // much larger budget. Sharing the history's budget meant the 9th
            // freeze of a session â€” which could easily be the interesting one
            // â€” would be adjudicated by nothing at all.
            static bool      sFrzVerdictPending = false;
            static int       sFrzVerdicts       = 0;
            constexpr int    kFrzMaxVerdicts    = 40;

            const auto  frzNow = std::chrono::steady_clock::now();
            float frzDt = std::chrono::duration<float>(frzNow - sFrzLast).count();
            sFrzLast = frzNow;
            frzDt = std::clamp(frzDt, 0.0f, 0.25f);

            auto& tdmF   = TDMIntegration::GetSingleton();
            auto* uiF    = RE::UI::GetSingleton();
            auto* plyF   = RE::PlayerCharacter::GetSingleton();
            const bool pausedF = uiF && uiF->GameIsPaused();
            const bool menuF   = UnpauseManager::GetUnpausedMenuCount() > 0 ||
                                 (uiF && uiF->IsMenuOpen(RE::DialogueMenu::MENU_NAME));

            // ---- INPUT: three INDEPENDENT reads (2026-08-19) -------------
            // The watchdog used to ask TDM alone. Two ways that blinds it:
            // no TDM at all, and â€” the one that matters â€” a freeze whose
            // mechanism is "the movement vector is being zeroed", in which
            // case TDM reports 0, `wantsMove` goes false, and the watchdog
            // concludes the player gave up. FOUR of the seven episodes in the
            // 2026-08-19 log closed with endedBy="player released movement
            // input (gave up)", which is precisely what that failure looks
            // like from in here. A watchdog that cannot tell "let go of the
            // stick" from "the stick stopped being heard" cannot see this bug.
            //
            // So read all three and REPORT all three. The disagreement is the
            // diagnosis:
            //   raw>0, engine=0  -> input is being eaten before the game sees it
            //   raw>0, engine>0  -> input arrives; the body is not moving on it
            //   raw=0            -> the player really did let go
            float rawMag = 0.0f;
            {
                // All four XInput slots, not just the first: a second pad,
                // a wireless receiver, or Steam Input can park the live one
                // at slot 1-3, and a raw read that only asks slot 0 would
                // then call a held stick "no input" (2026-09-05).
                for (DWORD slot = 0; slot < 4; ++slot) {
                    XINPUT_STATE xsF{};
                    if (XInputGetState(slot, &xsF) != ERROR_SUCCESS) continue;
                    const float lx = static_cast<float>(xsF.Gamepad.sThumbLX) / 32767.0f;
                    const float ly = static_cast<float>(xsF.Gamepad.sThumbLY) / 32767.0f;
                    const float m  = std::sqrt(lx * lx + ly * ly);
                    if (m > 0.25f) rawMag = (std::max)(rawMag, m);   // past the deadzone
                }
                if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState('A') & 0x8000) ||
                    (GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState('D') & 0x8000))
                    rawMag = (std::max)(rawMag, 1.0f);
            }
            float engMag     = 0.0f;
            bool  engBlocked = false;
            if (auto* pcF = RE::PlayerControls::GetSingleton()) {
                engMag     = std::abs(pcF->data.moveInputVec.x) +
                             std::abs(pcF->data.moveInputVec.y);
                engBlocked = pcF->blockPlayerInput;
            }
            float tdmMag = 0.0f;
            if (tdmF.IsAvailable()) {
                const auto mv = tdmF.GetMovementInput();
                tdmMag = std::abs(mv.x) + std::abs(mv.y);
            }
            const bool wantsMove = (engMag > 0.15f) || (rawMag > 0.25f);

            // ---- CONTROLLER: what the physics body is actually doing -----
            // "Instrument the character controller next" was the verdict the
            // 2026-08-16 experiment closed with, and it was never built. With
            // the input reads above, these five numbers finish the picture:
            // held input + a live controller state + zero outVelocity says the
            // hold is in the movement solve, not in any claim DDC or TDM makes.
            int           ccState    = -1;
            float         ccVel      = 0.0f;
            float         ccSpeedPct = 0.0f;
            std::uint32_t ccFlags    = 0;
            int           asMoveBits = 0;
            bool          plyMoving  = false;
            if (plyF) {
                if (auto* ccF = plyF->GetCharController()) {
                    ccState = static_cast<int>(ccF->context.currentState);
                    const float vx = ccF->outVelocity.quad.m128_f32[0];
                    const float vy = ccF->outVelocity.quad.m128_f32[1];
                    ccVel      = std::sqrt(vx * vx + vy * vy);
                    ccSpeedPct = ccF->speedPct;
                    ccFlags    = static_cast<std::uint32_t>(ccF->flags.underlying());
                }
                if (auto* asMv = plyF->AsActorState()) {
                    asMoveBits = (asMv->actorState1.movingForward ? 1 : 0) |
                                 (asMv->actorState1.movingBack    ? 2 : 0) |
                                 (asMv->actorState1.movingLeft    ? 4 : 0) |
                                 (asMv->actorState1.movingRight   ? 8 : 0);
                }
                plyMoving = plyF->IsMoving();
            }

            // ---- TARGET LOCK: how long since the last edge ---------------
            // The user calls this "the target lock Lock freeze bug", so the
            // watchdog has to be able to say whether a lock edge sits at the
            // onset. Tracked here rather than read from StateResolver so the
            // watchdog owns its own timeline and cannot be desynced by a
            // reorder elsewhere.
            static bool  sFrzPrevLocked   = false;
            static float sFrzSinceAcquire = 999.0f;
            static float sFrzSinceRelease = 999.0f;
            {
                const bool lockedNow = tdmF.GetRawTargetLockState();
                sFrzSinceAcquire += frzDt;
                sFrzSinceRelease += frzDt;
                if (lockedNow != sFrzPrevLocked) {
                    if (lockedNow) sFrzSinceAcquire = 0.0f;
                    else           sFrzSinceRelease = 0.0f;
                    sFrzPrevLocked = lockedNow;
                }
            }

            bool sitting = false;
            if (plyF) {
                if (auto* as = plyF->AsActorState()) {
                    sitting = as->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
                              as->IsSwimming();
                }
                if (plyF->IsDead()) sitting = true;
            }
            // A DDC menu window capturing input (Quick Tune, the panel) is
            // EXPECTED stillness: the game legitimately receives nothing while
            // the stick drives the menu. These frames used to count â€” and the
            // 21:59 log shows the cost: 19 of the 20 rate-limited lines were
            // spent inside two Quick Tune sessions, so the one REAL freeze of
            // the session had no budget left and logged NOTHING. A watchdog
            // whose cap can be exhausted by a known-benign state has muted
            // itself exactly when it is needed.
            const bool smfBlockF = MenuUI::IsGameInputBlocked();
            sFrzSinceDdcClose += frzDt;
            if (sFrzPrevSmfBlock != smfBlockF) {
                LogFreezeMovementState(smfBlockF ? "SMF input blocked" : "SMF input released");
            }
            if (sFrzPrevSmfBlock && !smfBlockF) sFrzSinceDdcClose = 0.0f;
            sFrzPrevSmfBlock = smfBlockF;
            if (sFrzEpisodeActive) {
                ++sFrzEpFrames;
                if (rawMag > 0.0f) { ++sFrzEpRawFrames; sFrzEpRawMax = (std::max)(sFrzEpRawMax, rawMag); }
                if (engMag > 0.0f) { ++sFrzEpEngFrames; sFrzEpEngMax = (std::max)(sFrzEpEngMax, engMag); }
                if (ccVel  > 0.5f) ++sFrzEpBodyFrames;
            }

            // Camera states where standing still is CORRECT and the player has
            // no say. kVATS is the vanilla killcam: the 2026-08-16 log spent a
            // whole 7-second episode on one (camState=2, timeMult walking
            // 1.000 -> 0.233 -> 0.100 -> 1.000) because IsInKillMove() reports
            // being the VICTIM, not the performer, so nothing else excused it.
            // kBleedout is the same kind of legitimate hold.
            bool noMoveCamF = false;
            if (a_camera && a_camera->currentState) {
                const int csF = static_cast<int>(a_camera->currentState->id);
                noMoveCamF = csF == static_cast<int>(RE::CameraState::kVATS) ||
                             csF == static_cast<int>(RE::CameraState::kBleedout);
            }

            // 2026-09-09 capture: full raw/engine/TDM movement, zero body
            // motion, all DDC claims released, but fovSlideMode remained on.
            // TDM's ProcessInput explicitly stands down in this vanilla zoom
            // mode.
            //
            // 2026-09-10 19:21:54 (the next session, same build): the latch
            // switched ON 0.36s after Quick Tune closed, with held=true
            // registered=true and NO pad button down at the close edge, stayed
            // on for 4.6s, and cleared only when the next R3 press+release
            // reached the engine. Vanilla clears fovSlideMode on the button's
            // Up, so a latch that outlives the physical release IS a swallowed
            // Up (the framework's input block or a downstream hook ate it) -
            // that is the whole test. The repair therefore no longer waits for
            // the player to push the stick into a body that will not move:
            // "physically released for 0.2s while the latch holds", in
            // ordinary gameplay, is enough. The movement stall is still
            // measured and printed so the line says whether the player was
            // being held at the moment of repair. A real hold is never
            // cancelled: the button must read Released. Unknown still blocks,
            // and the edge line now prints WHY it was unknown - both edges of
            // the 19:21 latch read Unknown, which is what kept the previous
            // version of this repair silent.
            {
                static POVSlideRecovery recovery;
                static bool wasSlide = false;
                auto* controls = RE::PlayerControls::GetSingleton();
                auto* main = RE::Main::GetSingleton();
                auto* map = RE::ControlMap::GetSingleton();
                const bool slide = controls && controls->data.fovSlideMode;
                std::string physicalWhy;
                const auto physical = slide ? ReadPhysicalPOVButton(&physicalWhy) : POVButtonState::Unknown;
                auto* handler = controls ? controls->togglePOVHandler : nullptr;
                if (slide != wasSlide) {
                    spdlog::debug("[POVSLIDE] active={} physical={} ({}) held={} registered={} release={} "
                                 "raw={:.2f} engine={:.2f} ccVel={:.2f} sinceMenu={:.2f}s smfBlock={} qtOpen={}",
                                 slide, static_cast<int>(physical), slide ? physicalWhy : "-",
                                 handler && handler->heldStateActive,
                                 handler && handler->pressRegistered,
                                 handler && handler->triggerReleaseEvent,
                                 rawMag, engMag, ccVel, sFrzSinceDdcClose,
                                 smfBlockF, MenuUI::IsQuickTuneWindowOpen());
                }
                wasSlide = slide;
                const bool ordinaryPOV = a_camera && a_camera->currentState &&
                    (a_camera->currentState->id == RE::CameraState::kFirstPerson ||
                     a_camera->currentState->id == RE::CameraState::kThirdPerson);
                // Ordinary gameplay: nothing on screen that could legitimately
                // own the button, no script/beast POV mode, controls live.
                const bool repairableGameplay = controls && handler && plyF && main && map &&
                    main->gameActive && !main->freezeTime && !pausedF && !menuF && !smfBlockF &&
                    !controls->blockPlayerInput && !controls->data.remapMode &&
                    !controls->data.povScriptMode && !controls->data.povBeastMode &&
                    map->IsMovementControlsEnabled() && ordinaryPOV && !sitting &&
                    !plyF->IsInKillMove();
                // Reported, not required: was the player pushing into a body
                // that would not move at the moment of repair?
                const bool movementStalled = rawMag > 0.25f && engMag > 0.15f &&
                    ccState >= 0 && ccVel < 0.5f && asMoveBits == 0;
                if (recovery.Update(frzDt, slide, repairableGameplay, physical)) {
                    spdlog::warn("[POVSLIDE] repairing stale POV zoom latch: button released ({}) "
                                 "while fovSlideMode held | movementStalled={} raw={:.2f} eng={:.2f} "
                                 "ccVel={:.2f} sinceMenu={:.2f}s (held={} registered={} release={})",
                                 physicalWhy, movementStalled, rawMag, engMag, ccVel, sFrzSinceDdcClose,
                                 handler->heldStateActive, handler->pressRegistered,
                                 handler->triggerReleaseEvent);
                    controls->data.fovSlideMode = false;
                    handler->heldStateActive = false;
                    handler->pressRegistered = false;
                    handler->triggerReleaseEvent = false;
                    s_fKeyHeld = false;
                    s_r3Held = false;
                }
            }

            // ONE gate, named, so the stuck test and the experiment watcher
            // below cannot drift apart.
            // 2026-09-05: `!menuF && !smfBlockF` are GONE from this gate. Both
            // are DDC-owned booleans that DDC itself uses to block movement -
            // UnpauseManager refuses MovementHandler while an unpaused menu
            // with allowMovement=false is on the stack, and the menu framework
            // eats game input while it believes a blocking window is open. A
            // freeze caused by either of those leaking past its menu's close
            // was, by construction, invisible here: the user's werewolf-form
            // freeze in the 09:53-09:58 window ("tied to opening our menus")
            // left NO [FREEZE] line at all. Now they are printed, not gated;
            // an episode while a menu is genuinely open is self-labelled by
            // smfBlock= / unpaused= on its own line.
            const bool frzGateOpen = plyF && wantsMove && !pausedF &&
                                     !sitting && !noMoveCamF;

            bool  stillStuckThisFrame = false;
            float frzPosDelta         = 0.0f;
            if (frzGateOpen) {
                const RE::NiPoint3 p = plyF->GetPosition();
                if (sFrzInit) {
                    const float dx = p.x - sFrzPrevPos.x;
                    const float dy = p.y - sFrzPrevPos.y;
                    frzPosDelta = std::sqrt(dx * dx + dy * dy);
                    // "NOT MOVING", not "moving slowly".
                    //
                    // The floor was 1.0 units per frame â€” 60 u/s at 60fps, and
                    // Skyrim's full-stick walk is only about 145 u/s. So a
                    // GAMEPAD walk at partial deflection sat under it: the
                    // 2026-08-23 log is full of episodes whose own ring buffer
                    // shows dPos climbing 0.00 -> 0.47 the whole time, i.e. the
                    // player creeping forward exactly as asked. Nine of the
                    // eleven episodes that session were that, and one of them
                    // spent the forced-TDM-release experiment and came back
                    // "CONFIRMED" â€” when all that had happened was the player
                    // pushing the stick harder and crossing the 1.0 line.
                    //
                    // 0.15 u/frame is ~9 u/s: slower than any locomotion state
                    // in the game, so anything under it really is standing
                    // still. A watchdog that fires on ordinary walking spends
                    // its rate limit before the real freeze ever happens.
                    constexpr float kFrzStillPerFrame = 0.15f;
                    if (frzPosDelta < kFrzStillPerFrame) {
                        sFrzStuckT += frzDt;
                        stillStuckThisFrame = true;
                    } else {
                        sFrzStuckT = 0.0f;
                    }
                }
                sFrzPrevPos = p;
                sFrzInit    = true;
            } else {
                sFrzStuckT = 0.0f;
                if (plyF) {
                    const RE::NiPoint3 p = plyF->GetPosition();
                    if (sFrzInit) {
                        const float dx = p.x - sFrzPrevPos.x;
                        const float dy = p.y - sFrzPrevPos.y;
                        frzPosDelta = std::sqrt(dx * dx + dy * dy);
                    }
                    sFrzPrevPos = p;
                    sFrzInit    = true;
                }
            }

            // Record EVERY frame, gate open or not â€” the frames just before
            // the gate opens are the ones that say what changed.
            {
                FrzSample fs;
                fs.rawMag     = rawMag;
                fs.engMag     = engMag;
                fs.tdmMag     = tdmMag;
                fs.ccVel      = ccVel;
                fs.ccSpeedPct = ccSpeedPct;
                fs.posDelta   = frzPosDelta;
                if (auto* tF = RE::BSTimer::GetSingleton()) fs.timeMult = tF->QGlobalTimeMultiplier();
                fs.ccState    = ccState;
                fs.asMoveBits = asMoveBits;
                fs.camState   = a_camera && a_camera->currentState
                                    ? static_cast<int>(a_camera->currentState->id) : -1;
                fs.ccFlags    = ccFlags;
                fs.engBlocked = engBlocked;
                fs.plyMoving  = plyMoving;
                fs.locked     = sFrzPrevLocked;
                if (auto* cmR = RE::ControlMap::GetSingleton())
                    fs.moveCtl = cmR->IsMovementControlsEnabled();
                fs.gateOpen   = frzGateOpen;
                sFrzRing[sFrzRingN % kFrzRing] = fs;
                ++sFrzRingN;
            }

            // RESOLUTION: the episode was live and this frame broke it (either
            // real movement happened, or a legitimate excuse â€” pause, menu,
            // sitting, an SMF window â€” took over). Report which, and for how
            // long. A resolution that fires within a second or two of onset
            // and lines up with a wall/corner is the mundane case the old
            // logs couldn't be told apart from; one that runs into double
            // digits of seconds with no excuse is the real bug.
            if (sFrzEpisodeActive && !stillStuckThisFrame) {
                const float totalStuck = std::chrono::duration<float>(
                    frzNow - sFrzEpisodeStart).count();
                if (sFrzLogs < 200) {
                    ++sFrzLogs;
                    // `endedBy` is the fix for the old log's blind spot: every
                    // previous episode closed with resumedMoving=false, which
                    // could mean the freeze broke OR that the gate simply
                    // stopped passing. Name it instead.
                    const bool reallyMoved = frzGateOpen;
                    const char* endedBy =
                        reallyMoved   ? "PLAYER MOVED"
                        : !wantsMove  ? "player released movement input (gave up)"
                        : smfBlockF   ? "DDC menu opened"
                        : menuF       ? "a menu opened"
                        : pausedF     ? "game paused"
                        : sitting     ? "sit/swim/dead"
                                      : "no-move camera state (killcam/bleedout)";
                    // "gave up" is only trustworthy now that wantsMove reads
                    // the HARDWARE too: before, a freeze that worked by
                    // zeroing the movement vector closed with this exact
                    // string and looked like the player letting go. The
                    // triple is printed so that stays checkable.
                    spdlog::warn(
                        "[FREEZE] resolved after {:.2f}s ({} onset log(s)) | endedBy=\"{}\" | "
                        "in(raw={:.2f} eng={:.2f} tdm={:.2f}) cc(state={} vel={:.1f}) | "
                        "resumedMoving={} pausedNow={} menuNow={} sittingNow={} smfBlockNow={} | "
                        "ep(frames={} raw>0={} rawMax={:.2f} eng>0={} engMax={:.2f} body>0={}) "
                        "sinceDdcMenuClose={:.1f}s | unpaused={} blockMove={} qtOpen={}",
                        totalStuck, sFrzEpisodeFires, endedBy,
                        rawMag, engMag, tdmMag, ccState, ccVel,
                        reallyMoved, pausedF, menuF, sitting, smfBlockF,
                        sFrzEpFrames, sFrzEpRawFrames, sFrzEpRawMax,
                        sFrzEpEngFrames, sFrzEpEngMax, sFrzEpBodyFrames,
                        sFrzSinceDdcClose,
                        UnpauseManager::GetUnpausedMenuCount(),
                        UnpauseManager::ShouldBlockPlayerMovement(),
                        MenuUI::IsQuickTuneWindowOpen());
                }
                sFrzEpisodeActive = false;
                sFrzEpisodeFires  = 0;
                // Resolved under the history threshold: it was a wall-bump.
                // Drop the snapshot unprinted and keep the budget for a real one.
                sFrzSnapPending    = false;
                sFrzVerdictPending = false;
            }

            if (sFrzStuckT > 1.5f) {
                if (!sFrzEpisodeActive) {
                    sFrzEpisodeActive = true;
                    sFrzEpisodeStart  = frzNow - std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(std::chrono::duration<float>(sFrzStuckT));
                    sFrzEpisodeFires  = 0;
                    sFrzEpFrames = sFrzEpRawFrames = sFrzEpEngFrames = sFrzEpBodyFrames = 0;
                    sFrzEpRawMax = sFrzEpEngMax = 0.0f;
                    // Freeze the run-up NOW (the ring keeps overwriting) and
                    // decide later whether it earned the ink.
                    if (sFrzDumps < kFrzMaxDumps) {
                        sFrzSnapN = (std::min)(sFrzRingN, kFrzRing);
                        for (int i = 0; i < sFrzSnapN; ++i)
                            sFrzSnap[i] = sFrzRing[(sFrzRingN - sFrzSnapN + i) % kFrzRing];
                        sFrzSnapPending = true;
                    }
                    sFrzVerdictPending = sFrzVerdicts < kFrzMaxVerdicts;
                }
                sFrzStuckT = 0.0f;   // re-arm the 1.5s trigger, not the episode
                ++sFrzEpisodeFires;
                const float episodeElapsed = std::chrono::duration<float>(
                    frzNow - sFrzEpisodeStart).count();
                float gtm = 1.0f;
                if (auto* t = RE::BSTimer::GetSingleton()) gtm = t->QGlobalTimeMultiplier();
                auto* cmF = RE::ControlMap::GetSingleton();
                auto& spimF = ShowPlayerInMenusController::GetSingleton();
                const bool spimStuck = spimF.IsActive() && !menuF && !pausedF;
                // Vanilla causes that look identical to a DDC-owned freeze
                // but aren't: a stagger/knockdown or a kill-move genuinely
                // stops the player and has nothing to do with any claim
                // below. Cheap to check, and rules out an entire class of
                // false alarm at a glance.
                RE::KNOCK_STATE_ENUM knockF = RE::KNOCK_STATE_ENUM::kNormal;
                bool killMoveF = false;
                if (plyF) {
                    if (auto* asF = plyF->AsActorState()) knockF = asF->GetKnockState();
                    killMoveF = plyF->IsInKillMove();
                }
                // The episode has now outlasted anything a wall or a stagger
                // explains, so the run-up is worth its lines. `gate` flips
                // 0 -> 1 on the frame the player started trying to move
                // without moving; read BACKWARDS from there.
                if (sFrzSnapPending && episodeElapsed >= kFrzHistorySec) {
                    sFrzSnapPending = false;
                    ++sFrzDumps;
                    spdlog::warn("[FREEZE] --- pre-onset history, {} frames ending at onset "
                                 "(oldest first; dump {}/{}) ---",
                                 sFrzSnapN, sFrzDumps, kFrzMaxDumps);
                    for (int i = 0; i < sFrzSnapN; ++i) {
                        const FrzSample& fs = sFrzSnap[i];
                        spdlog::warn("[FREEZE]   -{:>3} in(raw{:.2f} eng{:.2f} tdm{:.2f}) "
                                     "blocked={} moveCtl={} | cc(state={} vel{:.1f} spd{:.2f} "
                                     "flags=0x{:X}) asMove=0x{:X} moving={} | dPos={:.2f} "
                                     "tMult={:.3f} cam={} locked={} gate={}",
                                     sFrzSnapN - 1 - i, fs.rawMag, fs.engMag, fs.tdmMag,
                                     fs.engBlocked ? 1 : 0, fs.moveCtl ? 1 : 0,
                                     fs.ccState, fs.ccVel, fs.ccSpeedPct, fs.ccFlags,
                                     fs.asMoveBits, fs.plyMoving ? 1 : 0,
                                     fs.posDelta, fs.timeMult, fs.camState,
                                     fs.locked ? 1 : 0, fs.gateOpen ? 1 : 0);
                    }
                    spdlog::warn("[FREEZE] --- end pre-onset history ---");
                }

                if (sFrzVerdictPending && episodeElapsed >= kFrzHistorySec) {
                    sFrzVerdictPending = false;
                    ++sFrzVerdicts;

                    // THE VERDICT LINE. The 2026-08-23 log contained two REAL
                    // episodes with OPPOSITE signatures and it took a manual
                    // read of every field to tell them apart â€” so the log now
                    // adjudicates itself and names which of the two it is.
                    //
                    //   INPUT LOST   raw high, engine sees nothing. Something
                    //                between the device and the movement
                    //                handler is eating it. DDC's own
                    //                MovementHandler gate is the first
                    //                suspect, hence the claim block below.
                    //   ACTOR STUCK  engine HAS the input and the actor still
                    //                will not move. The anim graph or the
                    //                character controller is refusing; the
                    //                graph ring below is the evidence.
                    //   NO INPUT     the watchdog fired without the player
                    //                asking to move â€” a false alarm, and the
                    //                line says so instead of being mistaken
                    //                for a freeze.
                    const char* verdict =
                        (rawMag < 0.15f)                 ? "NO RAW INPUT OBSERVED (cached engine input possible)"
                      : (engMag < 0.15f)                 ? "RAW INPUT PRESENT, engine movement vector empty"
                      : (ccVel  < 0.15f)                 ? "MOVEMENT VECTOR PRESENT, controller stationary (cause unproven)"
                                                         : "CONTROLLER VELOCITY PRESENT (cause unproven)";
                    spdlog::warn("[FREEZE] VERDICT: {} | raw={:.2f} eng={:.2f} ccVel={:.2f} "
                                     "asMove=0x{:X}", verdict, rawMag, engMag, ccVel, asMoveBits);
                    LogFreezeMovementState("sustained movement stall", true);

                    // DDC's OWN movement blockers, spelled out. UnpauseManager
                    // gates the engine's MovementHandler directly â€” if that
                    // ever reads true with no menu on screen, the input never
                    // reaches the engine and this IS the bug, not a symptom.
                    spdlog::warn("[FREEZE] ddc-blocks: unpauseBlocksMove={} unpausedMenus={} "
                                 "anyAllowsMove={} smfBlock={} spim={} qtDM={}",
                                 UnpauseManager::ShouldBlockPlayerMovement(),
                                 UnpauseManager::GetUnpausedMenuCount(),
                                 UnpauseManager::IsAnyUnpausedMenuAllowingMovement(),
                                 smfBlockF, spimF.IsActive(), MenuUI::HasQuickTuneDMClaim());

                    // Actor-side detail for the ACTOR STUCK case. A stuck
                    // melee attack state or a sit/sleep state that never
                    // cleared both look exactly like a freeze from outside,
                    // and neither is visible in the fields above.
                    if (plyF) {
                        if (auto* asDet = plyF->AsActorState()) {
                            spdlog::warn("[FREEZE] actor: melee=0x{:X} sitSleep=0x{:X} "
                                         "fly=0x{:X} knock={} killMove={} swim={} sprint={}",
                                         static_cast<int>(asDet->actorState1.meleeAttackState),
                                         static_cast<int>(asDet->GetSitSleepState()),
                                         static_cast<int>(asDet->actorState1.flyState),
                                         static_cast<int>(knockF), killMoveF,
                                         asDet->IsSwimming(), asDet->IsSprinting());
                        }
                    }

                    // The player's last graph events, oldest first. For an
                    // ACTOR STUCK verdict this is the whole ballgame: a graph
                    // that entered an attack / stagger / paired animation and
                    // never emitted its matching end tag shows up here as a
                    // start with no finish.
                    {
                        std::array<std::string, StateResolver::kGraphRing> evs{};
                        std::size_t dropped = 0;
                        const std::size_t n =
                            StateResolver::GetSingleton().GetRecentGraphEvents(evs, dropped);
                        std::string joined;
                        for (std::size_t i = 0; i < n; ++i) {
                            if (!joined.empty()) joined += " -> ";
                            joined += evs[i];
                        }
                        spdlog::warn("[FREEZE] last {} graph events ({} skipped on contention): {}", n, dropped,
                                     joined.empty() ? "(none)" : joined.c_str());
                    }
                }
                if (sFrzLogs < 200) {
                    ++sFrzLogs;
                    // INPUT and CONTROLLER lead the line now â€” they are what
                    // the last four investigations lacked, and the claim
                    // mirrors that follow have read all-false every time.
                    spdlog::warn(
                        "[FREEZE] stuck {:.2f}s total ({}x, this fire {:.2f}s) | "
                        "in(raw={:.2f} eng={:.2f} tdm={:.2f}) blockPlayerInput={} | "
                        "cc(state={} vel={:.1f} spd={:.2f} flags=0x{:X}) asMove=0x{:X} "
                        "IsMoving={} | sinceLockOn={:.1f}s sinceLockOff={:.1f}s | "
                        "lockYaw={} combatFaceYaw={} dlgBodyYaw={} qtDM={} spimActive={} "
                        "tdmYawAny={} tdmDMDisabled={} tdmLocked={} | "
                        "movementCtl={} timeMult={:.3f} slowmoFlag={} camState={} "
                        "smfBlock={} knock={} killMove={} | "
                        "ep(frames={} raw>0={} rawMax={:.2f} eng>0={} engMax={:.2f} body>0={}) "
                        "sinceDdcMenuClose={:.1f}s | unpaused={} blockMove={} qtOpen={} menuF={}",
                        episodeElapsed, sFrzEpisodeFires, 1.5f,
                        rawMag, engMag, tdmMag, engBlocked,
                        ccState, ccVel, ccSpeedPct, ccFlags, asMoveBits, plyMoving,
                        sFrzSinceAcquire, sFrzSinceRelease,
                        CameraController::IsLockYawTaken(), CameraController::IsCombatFaceYawTaken(),
                        HookManager::IsDialogueFacingBodyYaw(), MenuUI::HasQuickTuneDMClaim(),
                        spimF.IsActive(),
                        tdmF.HasYawControl(), tdmF.HasDirectionalMovementDisabled(),
                        tdmF.GetRawTargetLockState(),
                        cmF ? (cmF->IsMovementControlsEnabled() ? 1 : 0) : -1,
                        gtm, s_deathSlowmoActive,
                        a_camera && a_camera->currentState
                            ? static_cast<int>(a_camera->currentState->id) : -1,
                        MenuUI::IsGameInputBlocked(),
                        static_cast<int>(knockF), killMoveF,
                        sFrzEpFrames, sFrzEpRawFrames, sFrzEpRawMax,
                        sFrzEpEngFrames, sFrzEpEngMax, sFrzEpBodyFrames,
                        sFrzSinceDdcClose,
                        UnpauseManager::GetUnpausedMenuCount(),
                        UnpauseManager::ShouldBlockPlayerMovement(),
                        MenuUI::IsQuickTuneWindowOpen(), menuF);
                }
                // Repair what is provably ours and provably orphaned. The menu
                // framing being active with no menu on screen is the one the
                // existing orphan watchdog cannot catch, because an active
                // controller is exactly what makes a yaw claim look legitimate.
                if (spimStuck) {
                    spdlog::warn("[FREEZE] menu framing was still active with no menu open â€” "
                                 "restoring");
                    spimF.ForceRestoreToVanilla();
                }
                // Quick Tune's claim is a live holder too â€” releasing it here
                // would recreate the exact double-release the orphan watchdog
                // fix removed.
                if (tdmF.HasDirectionalMovementDisabled() && !spimF.IsActive() &&
                    !MenuUI::HasQuickTuneDMClaim()) {
                    spdlog::warn("[FREEZE] releasing orphaned TDM directional-movement disable");
                    tdmF.ReleaseDisableDirectionalMovement();
                }

            }
        }

        // Map-menu FOV guard, per-frame arm. The open-edge write in
        // OnMapMenuOpenChange covers the paused-map case; this covers any
        // frame the camera update still ticks while the map is up (unpaused
        // map mods, the opening transition frame where a state hook may
        // write FOV after the event fired). Keeps the map at the neutral
        // default until it closes.
        if (s_mapMenuOpen) {
            if (auto* mapCam = RE::PlayerCamera::GetSingleton()) {
                mapCam->worldFOV = DefaultWorldFov();
            }
        }

        // Re-pick the dialogue look if the POV changed mid-conversation (manual F /
        // R3 toggle after a forced-POV entry). Runs every frame in both POVs and
        // self-gates on the Dialogue Menu being open.
        DialogueLookPicker::RefreshForPovChange();

        // Time-based dialogue behaviour: the auto-switch interval. Needs real
        // elapsed time rather than an edge, and has to keep running while the
        // player walks around mid-conversation, so it ticks here alongside the
        // POV refresh. Self-gates on the Dialogue Menu being open.
        {
            static auto sLastDlgTick = std::chrono::steady_clock::now();
            const auto  nowDlgTick   = std::chrono::steady_clock::now();
            const float dlgDt = std::chrono::duration<float>(nowDlgTick - sLastDlgTick).count();
            sLastDlgTick = nowDlgTick;
            DialogueLookPicker::Tick(dlgDt);
        }

        // Read-only reconnaissance for far-zoom audio drop-out. Self-limiting:
        // takes a handful of samples at genuinely different camera positions,
        // writes its conclusion to the log, and costs nothing thereafter.
        AudioListenerProbe::Update();

        // First-person dialogue auto-face-target (the 1p face-lock) is KEPT â€”
        // it's a wanted feature. The "Mara statue slowdown" was NOT vanilla's
        // INI soft-stop (those fDialogue*StopAngle* settings aren't even
        // registered in INISettingCollection â€” GetSetting returned null every
        // time, confirmed in the log; that whole theory was a dead end). The
        // real cause: the Alternate Start Mara statue is an actor with a static
        // mesh and NO head bone, so the face-lock's target fallback aimed at a
        // fixed height above the ref ORIGIN (empty air partway up a tall statue)
        // and held the view there, fighting you. The headless-target fallback is
        // fixed below (aim at the model's world-bound center), and the 1p noise
        // is now composited onto the face-lock matrix so the camera keeps its
        // life while locked (the face-lock's world.rotate assignment used to wipe
        // it). See the target-resolve + noise-compose sites further down.

        // Dialogue stutter diagnostic: capture engine-composed cameraRoot/
        // niCamera transforms into the latest [DLG-PERFRAME] row. No-op
        // outside the dialogue capture window. Called BEFORE the rest of
        // this hook's writes (face-lock matrix, ShowPlayerInMenus override)
        // so the row reflects the engine's natural post-pipeline state;
        // the second late-capture site (HookedNiCameraUpdateWorldData)
        // overwrites with the truly-final pre-render state after our own
        // writes have landed.
        if (auto* pcCap = RE::PlayerCamera::GetSingleton()) {
            CameraController::GetSingleton().CaptureDlgPerFrameLate(
                pcCap, CameraController::DlgLateCaptureSite::kUpdateCameraPost);
        }

        // Crosshair correction tick. Runs every frame regardless of which
        // early-return path the rest of this hook takes â€” CrosshairManager
        // has its own internal gates (3p-only, paused, SmoothCam-owned,
        // etc.). Scope-guard ensures it fires after our own cameraNI
        // writes below are done, even on the diagnostic-suspend path.
        struct CrosshairTickGuard {
            ~CrosshairTickGuard() {
                CrosshairManager::GetSingleton().Tick();
            }
        } _crosshairTickGuard;

        // Random-on-option-select poll. Sits ABOVE all early-return paths
        // (suspend overrides, non-1p, etc.) so it runs every frame
        // regardless of POV â€” face-lock and most of this hook are 1p-only,
        // but option detection has to fire in 3p dialogue too.
        {
            auto* mtmPoll = RE::MenuTopicManager::GetSingleton();
            static RE::MenuTopicManager::Dialogue* sLastSelDlg = nullptr;
            static RE::TESTopicInfo*               sLastTopic  = nullptr;
            if (mtmPoll && s_dialogueMenuOpen) {
                auto* curDlg   = mtmPoll->lastSelectedDialogue;
                auto* curTopic = mtmPoll->currentTopicInfo;
                if (s_dialoguePollNeedsPrime) {
                    sLastSelDlg = curDlg;
                    sLastTopic  = curTopic;
                    s_dialoguePollNeedsPrime = false;
                    spdlog::debug("[DLG-RANDOM] prime (suppress open-frame edge)");
                } else {
                    const bool dlgEdge   = (curDlg   != sLastSelDlg) && curDlg   != nullptr;
                    const bool topicEdge = (curTopic != sLastTopic ) && curTopic != nullptr;
                    // A topic edge is the NPC starting a new line. When
                    // "Switch Presets On Every NPC Line" owns that edge it
                    // takes it; otherwise it keeps falling through to the
                    // option-select path, which is where it has always gone â€”
                    // so turning the new toggle off leaves existing setups
                    // behaving exactly as before. One edge only ever produces
                    // one switch, never both.
                    const bool npcLineOwnsTopic =
                        SettingsManager::GetSingleton().dialogueSwitchOnNpcLine;
                    // currentTopicInfo is the TOPIC, not the LINE. It is set
                    // once when the NPC starts responding and holds for the
                    // whole response, so a multi-line answer produced exactly
                    // one edge â€” the 20:57 poll shows the same pointer for 4+
                    // seconds while the NPC kept talking, and only 3 edges in a
                    // 90-second conversation. "Switch on every NPC line" fired
                    // per topic, which reads as "not working".
                    //
                    // The per-LINE signal is the speaker's own voice state: it
                    // goes kStart at the top of each spoken line. Degrades
                    // safely â€” a rig that never drives voiceState simply never
                    // adds edges, leaving the topic edge behaving as before.
                    // A new SUBTITLE for the speaker is the per-line signal that
                    // actually fires. voiceState alone did not: the 21:24 log
                    // caught the one edge of that conversation as
                    // "topic=true voice=false", i.e. the state never
                    // transitioned across lines. The subtitle string is
                    // replaced for every spoken line, so a change of text
                    // (or of speaker) is exactly one edge per line.
                    //
                    // Both signals are kept and OR'd: voiceState covers a setup
                    // running with subtitles off, subtitles cover one whose
                    // voiceState never cycles. Either missing just contributes
                    // no edges, so this degrades to the old topic-only
                    // behaviour rather than breaking.
                    bool voiceEdge = false;
                    std::string npcLineSub;   // the new line's subtitle text (may be empty)
                    if (npcLineOwnsTopic) {
                        static bool        sPrevSpeaking = false;
                        static std::string sPrevSubtitle;
                        bool        speakingNow = false;
                        std::string subNow;
                        auto* spk = DialogueLookPicker::GetActiveSpeakerRef();
                        if (spk) {
                            if (auto* spkActor = spk->As<RE::Actor>()) {
                                if (auto* hp = spkActor->GetHighProcess()) {
                                    const auto vs = hp->voiceState.get();
                                    speakingNow = (vs == RE::VOICE_STATE::kStart ||
                                                   vs == RE::VOICE_STATE::kContinue);
                                }
                            }
                        }
                        if (auto* subs = RE::SubtitleManager::GetSingleton()) {
                            RE::BSSpinLockGuard guard(subs->lock);
                            for (const auto& si : subs->subtitles) {
                                auto sp = si.speaker.get();
                                if (!sp || (spk && sp.get() != spk)) continue;
                                const char* txt = si.subtitle.c_str();
                                if (txt && *txt) { subNow = txt; break; }
                            }
                        }
                        const bool subEdge = !subNow.empty() && subNow != sPrevSubtitle;
                        voiceEdge     = (speakingNow && !sPrevSpeaking) || subEdge;
                        sPrevSpeaking = speakingNow;
                        if (!subNow.empty()) sPrevSubtitle = subNow;
                        npcLineSub = std::move(subNow);
                    }
                    if ((topicEdge || voiceEdge) && npcLineOwnsTopic) {
                        // OPTIONAL short-line skip (toggle beside the NPC-line
                        // switch, user redesign 2026-08-15): re-framing for a
                        // one-liner reads as flicker. Spoken length is
                        // estimated from the subtitle text (~15 chars/sec);
                        // under 5 seconds keeps the current preset. No
                        // subtitle text (subtitles off, or a topic-only edge)
                        // estimates nothing and switches as before.
                        constexpr float kMinNpcLineSec = 5.0f;
                        constexpr float kSubCharsPerSec = 15.0f;
                        const bool skipShort =
                            SettingsManager::GetSingleton().dialogueSkipShortNpcLines;
                        const float estSec = (!skipShort || npcLineSub.empty())
                            ? kMinNpcLineSec
                            : static_cast<float>(npcLineSub.size()) / kSubCharsPerSec;
                        if (estSec >= kMinNpcLineSec) {
                            spdlog::debug("[DLG-RANDOM] npc-line edge (dlgChange={} topic={} voice={} est={:.1f}s)",
                                         dlgEdge, topicEdge, voiceEdge, estSec);
                            DialogueLookPicker::OnNpcLineChanged();
                        } else {
                            spdlog::debug("[DLG-RANDOM] npc-line edge SKIPPED â€” short line (est {:.1f}s < {:.1f}s)",
                                         estSec, kMinNpcLineSec);
                        }
                    } else if (dlgEdge || topicEdge) {
                        spdlog::debug("[DLG-RANDOM] option-select edge: dlgChange={} topicChange={}",
                                     dlgEdge, topicEdge);
                        DialogueLookPicker::OnDialogueOptionSelected();
                    }
                    sLastSelDlg = curDlg;
                    sLastTopic  = curTopic;
                }

                // Throttled poll log so we can see what these fields do
                // during a typical conversation. Drop once behavior is
                // confirmed.
                static auto sLastDlgLog = std::chrono::steady_clock::now();
                const auto  nowDlg = std::chrono::steady_clock::now();
                if (std::chrono::duration<float>(nowDlg - sLastDlgLog).count() > 1.0f) {
                    sLastDlgLog = nowDlg;
                    spdlog::debug("[DLG-RANDOM] poll: lastSelDlg={} currentTopic={} lastTopic={}",
                                 static_cast<void*>(curDlg),
                                 static_cast<void*>(curTopic),
                                 static_cast<void*>(mtmPoll->lastTopicInfo));
                }
            } else if (!s_dialogueMenuOpen) {
                sLastSelDlg = nullptr;
                sLastTopic  = nullptr;
            }
        }

        if (settings.diagnosticSuspendOverrides) {
            s_dialogueFaceLockWasActive = false;
            s_preDialogueValid          = false;
            s_entryTransActive          = false;
            s_faceLockShouldRender      = false;
            return;
        }

        // `settings` reference was hoisted to the top of this function.
        auto*       playerCam  = RE::PlayerCamera::GetSingleton();
        auto*       cameraRoot = playerCam ? playerCam->cameraRoot.get() : nullptr;

        // Restore the engine-default firstPersonFOV (offset 0x140) whenever
        // we're not in kFirstPerson. The field is shared with kAnimated's
        // forge / alchemy / enchanting zoom â€” the engine reads 0x140 to
        // size the workstation viewmodel, and our 1p hook's per-frame
        // write of settings.firstPersonHandsFov leaks into it because
        // nothing resets the field on state-out. Restore from the
        // fDefault1stPersonHandFOV GameSetting (vanilla 80) so the forge
        // anim renders at vanilla zoom.
        if (playerCam) {
            const auto* curState = playerCam->currentState.get();
            const auto* fpState  = playerCam->cameraStates[RE::CameraState::kFirstPerson].get();
            if (curState != fpState) {
                float vanillaHandsFov = 80.0f;
                if (auto* gsc = RE::GameSettingCollection::GetSingleton()) {
                    if (auto* gs = gsc->GetSetting("fDefault1stPersonHandFOV")) {
                        vanillaHandsFov = gs->data.f;
                    }
                }
                playerCam->firstPersonFOV = vanillaHandsFov;
            }
        }

        RE::NiAVObject* cameraNI = nullptr;
        if (cameraRoot) {
            if (auto* asNode = cameraRoot->AsNode();
                asNode && !asNode->GetChildren().empty()) {
                cameraNI = asNode->GetChildren()[0].get();
            }
        }


        // ShowPlayerInMenus matrix override on the MAIN world NiCamera.
        // PlayerCamera::Update fires every frame regardless of menu
        // state. For Barter the engine freezes the main NiCamera's
        // UpdateWorldData but DataChange / Update on PlayerCamera
        // still ticks, so this write reaches the main render even
        // when HookedNiCameraUpdateWorldData can't.
        if (auto& spim = ShowPlayerInMenusController::GetSingleton();
            spim.IsActive() && !spim.IsActiveCameraControlEnabled())
        {
            if (cameraNI) {
                if (auto* niCam = skyrim_cast<RE::NiCamera*>(cameraNI)) {
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        if (spim.ApplyRenderMatrix(niCam, player)) {
                            using fn_t = void(*)(RE::NiCamera*);
                            static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                            updateW2S(niCam);
                        }
                    }
                }
            }
        }

        // Camera looseness â€” SmoothCam-style world-position follow lag.
        // Each frame, our smoothed view position lerps toward the engine-
        // computed cameraNI world position with a distance-adaptive rate.
        // The rendered camera is at the smoothed position, so it trails
        // the player during movement and catches up when they stop.
        // Only applies in 3p / mount / dragon states. Cell changes /
        // teleports re-init via a distance threshold.
        // Per-entry looseness override: the resolved profile can replace the
        // global Looseness slider (same opt-in toggle as its other transition
        // sliders) so a single state can trail more or less than the rest.
        // During a Vanity exit the outgoing profile retains this setting too.
        float effLooseness = settings.cameraLooseness;
        if (auto* lp = CameraController::GetSingleton().GetTransitionProfile();
            lp && lp->transitionSetLooseness) {
            effLooseness = lp->transitionLooseness;
        }
        // The CONFIGURED looseness, before the target-lock ramp below. The
        // follow-lag block is gated on THIS, not the scaled value: while locked
        // the scaled looseness is ~0 (rate ~1) so the lag-tracker tracks the
        // player exactly and the offset stays ~0 (fresh). Gating on the scaled
        // value instead would SKIP the block while locked, freezing the tracker
        // at a stale offset that then snaps to 0 on the kill â€” confirmed via
        // [LOOSEDIAG]: "off 337.8->0.0 locked=0" on release = the unlock flicker.
        const float configuredLooseness = effLooseness;
        // Target-lock camera tightening. We DON'T scale the looseness rate any
        // more: doing so converged the position offset non-uniformly â€” the rate
        // (0.01^looseness) barely moved the camera for ~0.7s then rushed as it
        // crossed over, which read badly at high looseness with the character
        // off-screen ("locking on doesn't mesh"). Instead the lag tracker keeps
        // integrating at the CONFIGURED rate, and a smooth critically-damped
        // factor fades the APPLIED follow-lag offset to 0 while locked (and back
        // in on unlock). Uniform, snap-free, and decoupled from the rate curve.
        // sLockTighten: 0 = full looseness trail, 1 = tight (locked). Declared at
        // function scope so the follow-lag offset block below can fade by it.
        static float sLockTighten    = 0.0f;
        static float sLockTightenVel = 0.0f;
        // Dialogue tighten: the same critically-damped fade as the target-lock
        // tighten, but driven by the Dialogue Menu. Fades the APPLIED follow-lag
        // offset to 0 while in dialogue (looseness -> rigid) and smoothly back
        // on exit. Mirrors the lock approach exactly: the lag tracker keeps
        // integrating at the configured rate and only the applied offset is
        // faded, so entry/exit is a snap-free dolly rather than the visible
        // slide the old per-frame dialogue rate-floor produced while moving.
        static float sDlgTighten     = 0.0f;
        static float sDlgTightenVel  = 0.0f;
        {
            static std::chrono::steady_clock::time_point sLockTightLast{};
            static bool                                  sLockTightInit = false;
            const auto now = std::chrono::steady_clock::now();
            float dt = sLockTightInit
                ? std::chrono::duration<float>(now - sLockTightLast).count() : (1.0f / 60.0f);
            sLockTightLast = now;
            sLockTightInit = true;
            dt = std::clamp(dt, 0.0001f, 0.1f);
            const float omega  = 4.0f / 0.5f;   // ~0.5s critically-damped settle
            const float target = TDMIntegration::GetSingleton().IsTargetLocked() ? 1.0f : 0.0f;
            CriticalDampedSpringExact(sLockTighten, sLockTightenVel, target, omega, dt);
            if (sLockTighten < 0.0f) sLockTighten = 0.0f;
            if (sLockTighten > 1.0f) sLockTighten = 1.0f;

            const bool dlgOpen =
                RE::UI::GetSingleton() &&
                RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
            CriticalDampedSpringExact(sDlgTighten, sDlgTightenVel, dlgOpen ? 1.0f : 0.0f, omega, dt);
            if (sDlgTighten < 0.0f) sDlgTighten = 0.0f;
            if (sDlgTighten > 1.0f) sDlgTighten = 1.0f;
        }
        // Ease the effective looseness across state changes. Different states can
        // carry different looseness; switching to a low/zero-looseness state while
        // the camera is still trailing from a loose one would otherwise drop the
        // follow-lag offset in a single frame -- the gate below stops running, and
        // the rate hits 1.0 (instant catch-up) -- which reads as a SNAP. Easing the
        // looseness value lets the trailing offset converge to its new amount
        // smoothly. Both the gate and the follow rate below read this eased value.
        // (Reported 2026-06-06.)
        static float                                 sSmoothLoose     = -1.0f;
        static std::chrono::steady_clock::time_point sSmoothLooseLast{};
        static bool                                  sSmoothLooseInit = false;
        {
            const auto lnow = std::chrono::steady_clock::now();
            float ldt = sSmoothLooseInit
                ? std::chrono::duration<float>(lnow - sSmoothLooseLast).count() : (1.0f / 60.0f);
            sSmoothLooseLast = lnow;
            sSmoothLooseInit = true;
            ldt = std::clamp(ldt, 0.0001f, 0.1f);
            if (sSmoothLoose < 0.0f) sSmoothLoose = configuredLooseness;   // first run: no ease
            constexpr float kLooseEaseTau = 0.4f;                          // ~0.4s blend between states
            sSmoothLoose += (configuredLooseness - sSmoothLoose) * (1.0f - std::exp(-ldt / kLooseEaseTau));
        }
        const float smoothedLooseness = sSmoothLoose;
        static FleeFraming::FollowState sFlee;
        static FleeFraming::MovementImpulseTracker sFleeMovementImpulse;
        static RE::NiPoint3 sFleePrevPos{};
        static std::chrono::steady_clock::time_point sFleePrevT{};
        static RE::TESCameraState* sFleePrevState = nullptr;
        static bool sFleeInit = false;
        bool fleeUpdated = false;
        s_looseValid = false;   // re-published below only on frames the block runs
        if (smoothedLooseness > 0.001f && playerCam && cameraNI) {
            auto*      curState = playerCam->currentState.get();
            auto*      tpsState = playerCam->cameraStates[RE::CameraState::kThirdPerson].get();
            auto*      mntState = playerCam->cameraStates[RE::CameraState::kMount].get();
            auto*      drgState = playerCam->cameraStates[RE::CameraState::kDragon].get();
            // Dialogue interaction: face-lock writes rotation only
            // (s_faceLockRenderMatrix at 4016/4197/4212), so the lag offset
            // doesn't drift the lock off NPC head â€” rotation retargets
            // every frame from wherever the camera ended up. The camera-
            // POSITION follow-lag is faded to 0 in dialogue by sDlgTighten
            // (looseFade below), so the conversation framing stays rigid even
            // if you move during the conversation (this mod unlocks dialogue
            // movement), with no visible slide on enter/exit.
            const bool eligible = curState && (curState == tpsState ||
                                                curState == mntState ||
                                                curState == drgState);

            // Lag only the PLAYER-FOLLOW component, not the camera's whole world
            // position. Smoothing the raw camera position also lags the orbit you
            // get from mouse-look (the camera circles the player), which makes the
            // view rubber-band. Instead we smooth the PLAYER'S position and offset
            // the engine camera by however far that lags: the orbit moves the
            // engine camera position instantly and passes straight through, while
            // only your translational movement trails. Result: loose follow with
            // crisp mouse-look. (delta is a pure world-space translation, so it
            // shifts the camera position without touching its rotation.)
            static RE::NiPoint3 sLaggedPlayer{};
            static bool         sLooseInited = false;
            static std::chrono::steady_clock::time_point sLooseLastTime{};
            // [LOOSEDRG] terms, hoisted so the probe at the bottom of this
            // block can report them (they are computed in the integrate
            // branch below and would otherwise be out of scope).
            float dbgLagDist = 0.0f;
            float dbgRate    = 0.0f;
            float dbgSpeed   = 0.0f;
            float dbgLeashS  = 0.0f;
            // Player speed, one-poled. The leash below is a TIME window
            // now, so it needs to know how fast the world is going past.
            static RE::NiPoint3 sLooseSpeedPrev{};
            static float        sLooseSpeedSm  = 0.0f;
            static bool         sLooseSpeedHad = false;

            auto* loosePlayer = RE::PlayerCharacter::GetSingleton();
            if (eligible && loosePlayer) {
                const RE::NiPoint3 engineCam = cameraNI->world.translate;
                const RE::NiPoint3 playerPos = loosePlayer->GetPosition();
                const auto         now       = std::chrono::steady_clock::now();

                // On lock-OFF, snap the lag tracker to the player so the loose
                // trail rebuilds from 0 as you keep moving, instead of fading in
                // the (unapplied) offset that piled up during locked movement â€”
                // that pile-up was the ~80u backward lurch on unlock. Harmless
                // visually: looseFade is ~0 at the unlock instant, so resetting
                // sLaggedPlayer here changes nothing on-screen that frame.
                static bool sLooseWasLocked = false;
                const bool  looseLocked = TDMIntegration::GetSingleton().IsTargetLocked();
                if (sLooseWasLocked && !looseLocked) sLaggedPlayer = playerPos;
                sLooseWasLocked = looseLocked;

                // Same idea on dialogue EXIT: while in dialogue the applied
                // offset is held at ~0 by sDlgTighten, but the tracker keeps
                // trailing if you walk during the conversation (movement is
                // unlocked). Snap it to the player on the close edge so the
                // loose trail rebuilds from 0 instead of drifting the piled-up
                // offset back in. looseFade is ~0 at the close instant
                // (sDlgTighten still ~1), so this is a no-op on-screen.
                static bool sLooseWasInDlg = false;
                const bool  looseInDlg =
                    RE::UI::GetSingleton() &&
                    RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
                if (sLooseWasInDlg && !looseInDlg) sLaggedPlayer = playerPos;
                sLooseWasInDlg = looseInDlg;

                bool followReinitialized = !sLooseInited;
                if (!sLooseInited) {
                    sLaggedPlayer  = playerPos;
                    sLooseLastTime = now;
                    sLooseInited   = true;
                } else {
                    const RE::NiPoint3 d{
                        playerPos.x - sLaggedPlayer.x,
                        playerPos.y - sLaggedPlayer.y,
                        playerPos.z - sLaggedPlayer.z
                    };
                    const float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);

                    // Speed of the player through the world this frame.
                    {
                        float sdt = std::chrono::duration<float>(now - sLooseLastTime).count();
                        sdt = std::clamp(sdt, 0.0001f, 0.1f);
                        if (sLooseSpeedHad) {
                            const RE::NiPoint3 sd{ playerPos.x - sLooseSpeedPrev.x,
                                                   playerPos.y - sLooseSpeedPrev.y,
                                                   playerPos.z - sLooseSpeedPrev.z };
                            const float step = std::sqrt(sd.x*sd.x + sd.y*sd.y + sd.z*sd.z);
                            // A teleport frame must not spike the window.
                            const float inst = (std::min)(step / sdt, 6000.0f);
                            sLooseSpeedSm += (inst - sLooseSpeedSm) *
                                             (1.0f - std::exp(-sdt / 0.25f));
                        }
                        sLooseSpeedPrev = playerPos;
                        sLooseSpeedHad  = true;
                    }

                    // Teleport / cell-change re-init. Has to sit ABOVE whatever
                    // trail the leash legitimately allows, or a fast flight would
                    // re-init every frame and render no lag at all. Scales with
                    // the same speed the leash does (2026-09-06); a real teleport
                    // is a single-frame jump of many thousands of units and still
                    // trips it. Never below the old fixed 3000.
                    const float kReinitDist =
                        (std::max)(3000.0f, (std::max)(sLooseSpeedSm, 600.0f) * (2000.0f / 600.0f) + 1000.0f);
                    if (dist > kReinitDist) {
                        // Cell change / teleport â€” snap so there's no huge lag.
                        sLaggedPlayer = playerPos;
                        followReinitialized = true;
                    } else {
                        const float looseness = std::clamp(smoothedLooseness, 0.0f, 1.0f);
                        // Felt lag is ~ 1/rate, so a LINEAR rate made the slider
                        // feel dead from 0-50% and explode near the top. Interpolate
                        // the rate GEOMETRICALLY between 1.0 (rigid) and the minimum
                        // (very loose): rate = kLooseRateMin^looseness. Each slider
                        // step multiplies the lag by a constant, so the change feels
                        // even across the whole range. Hold this near-constant rate
                        // for normal play and only ramp toward rigid once the player
                        // has out-run the camera far, so it can't drift unbounded.
                        //
                        // Even in log space the raw slider's bottom ~60% produced so
                        // little lag (100^x is only ~4 at x=0.3) that it read as dead.
                        // Spread the perceptible range across the WHOLE slider with a
                        // sqrt remap: it lifts the low/mid values while pinning the
                        // endpoints (0 -> rigid; 1.0 -> the identical max as before).
                        const float looseCurve = std::sqrt(looseness);
                        // Single-sourced in Defaults.h so the SmoothCam importer's
                        // inverse can't drift (was 0.03; lowered to 0.01 for ~3x
                        // looser at max â€” which is what broke the importer before).
                        constexpr float kLooseRateMin = Defaults::LooseRateMin;
                        const float baseRate = std::pow(kLooseRateMin, looseCurve);
                        // Leash: hold the loose rate until the player is well out
                        // ahead of the camera, then ramp toward rigid so the camera
                        // doesn't drift unbounded. Old window (200..600) saturated
                        // at sprint distance, making max looseness feel rigid in
                        // normal play. Then (500..2000) gave ~3x more leash.
                        //
                        // A FIXED DISTANCE WINDOW IS THE WRONG UNIT. What a player
                        // feels as "loose" is a TIME lag, and a distance leash
                        // converts to less and less time the faster you go. On foot
                        // it never bit; on a dragon it is the only thing that
                        // matters. Measured 2026-09-06, [LOOSEDRG] cruising at max
                        // looseness: baseRate is 0.010 but the APPLIED rate was
                        // 0.052-0.060 and lagDist pinned at 564-576u — the leash
                        // ramping the rate 5.5x above what the slider asked for.
                        // At the implied ~1880 u/s that is 0.30 s of trail, against
                        // 0.86 s for the same slider at a 600 u/s sprint: the same
                        // setting delivers ~2.9x less felt looseness on a dragon.
                        //
                        // This also disposes of "dragons need a 5.00 max instead of
                        // 1.00". With the leash binding, the equilibrium solves
                        // L*(L-500)/1500 = v/60 almost independently of baseRate:
                        // looseness 5.0 would give 581u against 1.0's 570u. A 2%
                        // change. The slider range was never the constraint.
                        //
                        // So the window scales with speed, anchored so that at or
                        // below a sprint (600 u/s) it is EXACTLY the old 500..2000
                        // and nothing on foot moves.
                        constexpr float kLeashRefSpeed = 600.0f;   // sprint
                        const float leashRef   = (std::max)(sLooseSpeedSm, kLeashRefSpeed);
                        const float leashStart = leashRef * (500.0f  / kLeashRefSpeed);
                        const float leashEnd   = leashRef * (2000.0f / kLeashRefSpeed);
                        const float t        = std::clamp((dist - leashStart) /
                                                          (leashEnd - leashStart), 0.0f, 1.0f);
                        const float rate     = baseRate + (1.0f - baseRate) * t;
                        dbgLagDist = dist;
                        dbgRate    = rate;
                        dbgSpeed   = sLooseSpeedSm;
                        dbgLeashS  = leashStart;

                        float dt = std::chrono::duration<float>(now - sLooseLastTime).count();
                        dt       = std::clamp(dt, 0.0001f, 0.1f);
                        const float lambda = 1.0f - std::pow(1.0f - rate, dt * 60.0f);

                        sLaggedPlayer.x += d.x * lambda;
                        sLaggedPlayer.y += d.y * lambda;
                        sLaggedPlayer.z += d.z * lambda;
                    }
                    sLooseLastTime = now;
                }

                // Flee Framing (Cinematic Effects): a THIRD tighten factor
                // beside the lock/dialogue ones. When the player is moving
                // TOWARD the camera (fleeing while facing back at enemies â€”
                // the geometry where high Looseness pushes the follow-lag
                // offset into the approaching player and shoves them off
                // screen), fade the applied offset toward rigid, scaled by
                // approach speed and the user slider. Purely geometric: it
                // engages exactly in that circumstance (velocity against the
                // camera forward), regardless of what caused it, and springs
                // back out after a turn and its fast momentum settle. A fresh
                // Whirlwind Sprint does not inherit the previous dash's hold.
                // Release retires the hidden trail instead of revealing it.
                {
                    const float elapsed = sFleeInit
                        ? std::chrono::duration<float>(now - sFleePrevT).count() : 1.0f / 60.0f;
                    const float fdt = std::clamp(elapsed, 0.0001f, 0.1f);
                    const bool continuous = sFleeInit && !followReinitialized &&
                        sFleePrevState == curState && elapsed >= 0.0f && elapsed <= 0.1f;
                    float approach = 0.0f;
                    float speed = 0.0f;
                    const float fleeStr =
                        std::clamp(settings.fleeFramingStrength, 0.0f, 1.0f);
                    // NOT WHILE RIDING A DRAGON (2026-09-06). Flee Framing
                    // exists for one geometry: the player on foot running
                    // BACK at the camera, where the follow-lag offset would
                    // shove them off screen. Its test is purely kinematic —
                    // world velocity against camera forward — and dragon
                    // flight trips it constantly, because a banking turn
                    // swings the camera through the flight vector several
                    // times a pass. Measured: [LOOSEDRG] 17:39:43-59 shows
                    // fleeT swinging 0.00 -> 1.00 -> 0.24 -> 0.69 -> 0.89 ->
                    // 0.90 across a single flight with no lock and no
                    // dialogue, dragging fade to 0.10 and the applied trail
                    // from 264u down to 8u. That is the "looseness keeps
                    // cutting out" half of the report, and it would have
                    // survived any amount of tuning on the Looseness slider
                    // itself. Mounts are deliberately NOT excluded here: a
                    // horse galloping at the camera is the geometry the
                    // effect is for, and no one has reported it misfiring.
                    const bool fleeOnDragon = (curState && curState == drgState);
                    if (!continuous || fleeOnDragon) {
                        // Do not carry momentum across POV/menu/state changes,
                        // teleports or frame gaps. Keep the currently visible
                        // trail when clearing a still-active release blend.
                        FleeFraming::RetireHiddenLag(sLaggedPlayer, playerPos, sFlee.tighten, 0.0f);
                        sFlee.Reset();
                    }
                    if (continuous && fleeStr > 0.001f && !fleeOnDragon) {
                        // Per-frame velocity; clamped so a teleport frame's
                        // position spike can't slam the factor to full.
                        RE::NiPoint3 vel{
                            (playerPos.x - sFleePrevPos.x) / fdt,
                            (playerPos.y - sFleePrevPos.y) / fdt,
                            (playerPos.z - sFleePrevPos.z) / fdt };
                        speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
                        if (speed > 800.0f) {
                            const float scl = 800.0f / speed;
                            vel.x *= scl; vel.y *= scl; vel.z *= scl;
                        }
                        // Camera forward = column 0 of the render matrix (the
                        // same extraction the pre-yaw capture below uses).
                        const float fx = cameraNI->world.rotate.entry[0][0];
                        const float fy = cameraNI->world.rotate.entry[1][0];
                        const float fz = cameraNI->world.rotate.entry[2][0];
                        // u/s of motion toward the camera plane. Walking
                        // sideways or away contributes nothing.
                        approach = -(vel.x * fx + vel.y * fy + vel.z * fz);
                    }
                    sFleePrevPos = playerPos;
                    sFleePrevT   = now;
                    sFleePrevState = curState;
                    sFleeInit    = true;
                    const auto& fleeResolver = StateResolver::GetSingleton();
                    if (fleeResolver.GetActiveShoutId() == ShoutId::WhirlwindSprint) {
                        // Both the windup and actual fire are new movement
                        // boundaries. Residual approach during the windup can
                        // legitimately re-engage; it must not survive the next
                        // launch if that launch is now heading away.
                        sFleeMovementImpulse.Observe(
                            static_cast<std::uint64_t>(fleeResolver.GetShoutStartTime().time_since_epoch().count()),
                            static_cast<std::uint64_t>(fleeResolver.GetShoutFireTime().time_since_epoch().count()));
                    }
                    // Preserve the unclamped speed for the fast-motion hold;
                    // the 800-unit approach cap cannot distinguish a dash.
                    sFlee.Update(sLaggedPlayer, playerPos, approach, speed,
                        fleeOnDragon ? 0.0f : fleeStr, fdt, sFleeMovementImpulse.serial);
                    fleeUpdated = true;

                    static unsigned sFleeLogs = 0;
                    static std::chrono::steady_clock::time_point sFleeLogTime{};
                    if (spdlog::should_log(spdlog::level::debug) &&
                        sFleeLogs < 128 && (speed > 800.0f || sFlee.heldApproach > 0.0f) &&
                        now - sFleeLogTime >= std::chrono::milliseconds(100)) {
                        sFleeLogTime = now;
                        ++sFleeLogs;
                        spdlog::debug("[FLEE] speed={:.0f} approach={:.0f} held={:.2f} "
                                     "tighten={:.3f} lag=({:.1f},{:.1f},{:.1f}) impulse={} ({}/128)",
                            speed, approach, sFlee.heldApproach, sFlee.tighten,
                            sLaggedPlayer.x - playerPos.x, sLaggedPlayer.y - playerPos.y,
                            sLaggedPlayer.z - playerPos.z, sFleeMovementImpulse.serial, sFleeLogs);
                    }
                }

                // Camera = engine position shifted back by the player-follow lag,
                // faded by the lock-tighten factor (offset -> 0 while locked, so
                // the camera dollies smoothly to a tight target framing and back).
                // Fade the applied follow-lag while EITHER target-locked or in
                // dialogue (product: either one tightens; both => still tight),
                // and by the Flee Framing factor when running toward the camera.
                const float lockDialogueFade = (1.0f - sLockTighten) * (1.0f - sDlgTighten);
                const float looseFade = lockDialogueFade * (1.0f - sFlee.tighten);
                const RE::NiPoint3 looseCamera{
                    engineCam.x + (sLaggedPlayer.x - playerPos.x) * lockDialogueFade,
                    engineCam.y + (sLaggedPlayer.y - playerPos.y) * lockDialogueFade,
                    engineCam.z + (sLaggedPlayer.z - playerPos.z) * lockDialogueFade
                };

                // Ease toward the active profile/zoom/collision position. Both
                // engagement and release go through the spring; do not impose
                // a second distance constraint that can move the camera instantly.
                const RE::NiPoint3 rendered = FleeFraming::ApplyTighten(engineCam, looseCamera, sFlee.tighten);
                ParaglideTrace::Follow(engineCam, rendered, smoothedLooseness,
                    (sLaggedPlayer - playerPos).Length(), dbgRate, sFlee.tighten, followReinitialized);

                // Same lockstep write set as before (SmoothCam camera.cpp:217-222):
                // renderer, audio listener, occlusion and the engine's next-frame
                // derivation must all see the same value. delta -> 0 at rest, so
                // these converge to the true engine position when you stop.
                TweenCameraTrace::Sample("follow-before");
                cameraNI->world.translate   = rendered;
                cameraRoot->world.translate = rendered;
                cameraRoot->local.translate = rendered;
                static_cast<RE::ThirdPersonState*>(curState)->translation = rendered;
                TweenCameraTrace::Sample("follow-after");
                // Publish the split for [SNAP]: what the engine composed and
                // what this block put on top of it.
                {
                    const float lsy = std::sin(loosePlayer->data.angle.z);
                    const float lcy = std::cos(loosePlayer->data.angle.z);
                    const auto  lLat = [&](const RE::NiPoint3& w) {
                        return (w.x - playerPos.x) * lcy - (w.y - playerPos.y) * lsy;
                    };
                    s_tweenLooseEngLat  = lLat(engineCam);
                    s_tweenLooseRendLat = lLat(rendered);
                    s_tweenLooseLagLat  = (sLaggedPlayer.x - playerPos.x) * lcy -
                                          (sLaggedPlayer.y - playerPos.y) * lsy;
                    s_tweenLooseFade    = looseFade;
                    s_tweenLooseRan     = true;
                }
                s_looseEngineCam = engineCam;
                s_looseApplied   = RE::NiPoint3{ rendered.x - engineCam.x,
                                                 rendered.y - engineCam.y,
                                                 rendered.z - engineCam.z };
                s_looseValid     = true;

                // [LOOSEDRG] — "dragon riding looseness doesn't work, or
                // isn't strong enough even at max" (user, 2026-09-06).
                // Looseness is a product of five independent terms and the
                // screen only shows the product, so the report cannot say
                // which one is zero. One line, only while the DRAGON camera
                // state is current, capped, throttled — it names all five:
                //
                //   cfgLoose  the value the block actually ran on. If this
                //             is low while the menu says max, the RESOLVED
                //             PROFILE is overriding the global (setLoose=1
                //             says so) — e.g. a dragon SUB-state that never
                //             had its own Looseness opt-in ticked, so it
                //             falls back past the entry you edited.
                //   lagDist   how far the tracker is trailing the player.
                //             Arithmetic says this should settle ~500-600u
                //             at max looseness at ANY speed, because the
                //             leash clamps it. Near 0 = the tracker is not
                //             lagging at all.
                //   rate      the follow rate. ~1.00 = rigid: the leash
                //             window (500..2000u) has saturated, which is
                //             the on-foot calibration failing at flight
                //             speed.
                //   fade      lock x dialogue x flee tighten. ~0 means the
                //             offset is computed and then multiplied away —
                //             a target lock held while flying would do it.
                //   appliedR  the offset that actually reached the camera.
                //             Large here with no visible trail = something
                //             downstream of this write is overwriting it.
                {
                    static int  sLooseDrgLogs = 0;
                    static std::chrono::steady_clock::time_point sLooseDrgLast{};
                    if (curState == drgState && sLooseDrgLogs < 40 &&
                        now - sLooseDrgLast > std::chrono::milliseconds(500)) {
                        sLooseDrgLast = now;
                        ++sLooseDrgLogs;
                        auto* dbgLp = CameraController::GetLastResolvedProfile();
                        // WHICH dragon profile resolved. The base is CRUISING
                        // and there are eleven sub-states beside it, each with
                        // its OWN Looseness opt-in. If a sub-state resolves
                        // with setLoose=0 while the BASE has it ticked, then
                        // "Base blankets the entries below unless overridden"
                        // — the house rule everywhere else in Categories —
                        // is not being honoured for the transition overrides,
                        // and that is a different bug from an unticked box.
                        struct DrgName { const char* n; const CameraProfile* p; };
                        const DrgName kDrg[] = {
                            { "Base/Cruising",   &settings.mountsDragonRiding },
                            { "Perched",         &settings.mountsDragonRidingPerched },
                            { "Hovering",        &settings.mountsDragonRidingHovering },
                            { "Takeoff",         &settings.mountsDragonRidingTakeoff },
                            { "Landing",         &settings.mountsDragonRidingLanding },
                            { "AttackGrounded",  &settings.mountsDragonRidingAttackGrounded },
                            { "AttackHovering",  &settings.mountsDragonRidingAttackHovering },
                            { "AttackFlying",    &settings.mountsDragonRidingAttackFlying },
                            { "BreathGrounded",  &settings.mountsDragonRidingBreathGrounded },
                            { "BreathHovering",  &settings.mountsDragonRidingBreathHovering },
                            { "BreathFlying",    &settings.mountsDragonRidingBreathFlying },
                        };
                        const char* drgName = "OTHER(not a dragon profile)";
                        for (const auto& e : kDrg) {
                            if (dbgLp == e.p) { drgName = e.n; break; }
                        }
                        const float appliedMag = std::sqrt(
                            s_looseApplied.x * s_looseApplied.x +
                            s_looseApplied.y * s_looseApplied.y +
                            s_looseApplied.z * s_looseApplied.z);
                        spdlog::debug(
                            "[LOOSEDRG] resolved={} setLoose={} profLoose={:.2f} | "
                            "BASE setLoose={} loose={:.2f} | global={:.2f} cfgLoose={:.2f} "
                            "smoothed={:.2f} lagDist={:.0f} rate={:.3f} spd={:.0f} leashStart={:.0f} "
                            "trailSec={:.2f} | lockT={:.2f} dlgT={:.2f} "
                            "fleeT={:.2f} fade={:.2f} appliedR={:.0f} ({}/40)",
                            drgName,
                            (dbgLp && dbgLp->transitionSetLooseness) ? 1 : 0,
                            dbgLp ? dbgLp->transitionLooseness : -1.0f,
                            settings.mountsDragonRiding.transitionSetLooseness ? 1 : 0,
                            settings.mountsDragonRiding.transitionLooseness,
                            settings.cameraLooseness,
                            configuredLooseness, smoothedLooseness,
                            dbgLagDist, dbgRate, dbgSpeed, dbgLeashS,
                            dbgSpeed > 1.0f ? (dbgLagDist / dbgSpeed) : 0.0f,
                            sLockTighten, sDlgTighten, sFlee.tighten, looseFade,
                            appliedMag, sLooseDrgLogs);
                    }
                }
            } else {
                sLooseInited = false;
            }
        }

        if (!fleeUpdated) {
            sFlee.Reset();
            sFleeInit = false;
            sFleePrevState = nullptr;
        }

        // ===== Stair Smoothing (Cinematic Effects, experimental) =====
        //
        // Bethesda's character controller resolves a step by moving the body up
        // the whole riser in ONE frame, so a staircase is a hard vertical
        // sawtooth and the camera, which follows the body exactly, does it too.
        //
        // The first attempt low-passed the camera's height continuously. That
        // cancelled the stairs but it also touched every other vertical thing
        // the player does - walking a slope, cresting a hill, stepping off a
        // kerb - because a low-pass has no idea which of those it is looking
        // at. It was "affecting too many other things", and correctly so: it
        // was smoothing ALL vertical motion to fix one kind.
        //
        // This version only ever reacts to the artefact itself. A step is a
        // DISCONTINUITY: one frame's height delta is far larger than movement
        // could account for. Real ground - however steep - moves you a little
        // each frame in proportion to your speed:
        //     sprinting (~600 u/s) up a 30 degree slope is ~5.8 u/frame at 60fps
        //     a stair riser is a single ~17 u jump in ONE frame
        // So a threshold cleanly separates them. On anything continuous this
        // code contributes EXACTLY ZERO - the offset never leaves 0 and the
        // camera is untouched. Only a real discontinuity produces an offset,
        // which is then eased away.
        //
        // Each step: absorb the jump into an offset (camera stays put), then
        // decay that offset to nothing over the Strength time constant, so the
        // camera climbs the riser smoothly instead of teleporting up it.
        {
            auto& stairSet = SettingsManager::GetSingleton();
            // Stair smoothing is ALWAYS ON at full strength now (user ruling
            // 2026-08-15: "more of a fix than a user preference") â€” the
            // Cinematic Effects entry and its sliders are gone. The settings
            // fields still exist for old presets but are no longer read.
            const float strength = 1.0f;

            static float sStairOffset = 0.0f;   // current camera Z correction
            static float sStairPrevZ  = 0.0f;
            static float sStairPrevX  = 0.0f;
            static float sStairPrevY  = 0.0f;
            static bool  sStairInit   = false;
            static std::chrono::steady_clock::time_point sStairLast{};

            auto* stairPlayer = RE::PlayerCharacter::GetSingleton();

            if (strength <= 0.001f || !playerCam || !cameraNI || !stairPlayer ||
                stairSet.diagnosticSuspendOverrides)
            {
                sStairInit   = false;
                sStairOffset = 0.0f;
            } else {
                auto* curState = playerCam->currentState.get();
                const bool eligibleStair =
                    curState &&
                    (curState == playerCam->cameraStates[RE::CameraState::kThirdPerson].get() ||
                     curState == playerCam->cameraStates[RE::CameraState::kMount].get());

                // Grounded only. kOnGround is the controller's own answer, so
                // this tracks whatever movement mod is driving the player, and
                // jumping / falling are left completely alone (the Jumping
                // effect owns those and must not be fought).
                bool grounded = true;
                if (auto* cc = stairPlayer->GetCharController()) {
                    grounded = cc->context.currentState == RE::hkpCharacterStateType::kOnGround;
                }

                const RE::NiPoint3 rawPos = stairPlayer->GetPosition();
                const float rawZ = rawPos.z;
                const auto  now  = std::chrono::steady_clock::now();
                float dt = sStairInit
                    ? std::chrono::duration<float>(now - sStairLast).count() : (1.0f / 60.0f);
                sStairLast = now;
                dt = std::clamp(dt, 0.0001f, 0.1f);

                if (!eligibleStair || !sStairInit) {
                    // Not our business this frame: drop any correction rather
                    // than carry it, so nothing is owed when we resume.
                    sStairOffset = 0.0f;
                    sStairPrevZ  = rawZ;
                    sStairPrevX  = rawPos.x;
                    sStairPrevY  = rawPos.y;
                    sStairInit   = true;
                } else {
                    const float dz  = rawZ - sStairPrevZ;
                    const float dxs = rawPos.x - sStairPrevX;
                    const float dys = rawPos.y - sStairPrevY;
                    const float dxy = std::sqrt(dxs * dxs + dys * dys);
                    sStairPrevZ = rawZ;
                    sStairPrevX = rawPos.x;
                    sStairPrevY = rawPos.y;

                    // Below the threshold this is ordinary ground and nothing
                    // happens at all. Above the teleport bound it is a load
                    // door or a script move, which must not be smoothed either.
                    //
                    // The floor is SPEED-PROPORTIONAL, not fixed (werewolf
                    // fix, 2026-08-15): continuous ground â€” however steep â€”
                    // gains height in proportion to horizontal travel
                    // (|dz| <= dxyÂ·tan(slope), and walkable slopes stay under
                    // ~50Â°, tan â‰ˆ 1.2). A fixed 8u floor was tuned for human
                    // sprint; a sprinting werewolf covers enough ground per
                    // frame that ordinary slope motion crossed it and got
                    // "absorbed" frame after frame â€” rough exactly at beast
                    // speed. A real riser is a jump ABOVE what this frame's
                    // travel can account for, at any speed and framerate.
                    constexpr float kTeleportMin = 200.0f;
                    const float stepMin = (std::max)(8.0f, dxy * 1.2f);
                    const float mag = std::abs(dz);

                    // Absorb only while grounded: airborne frames (including
                    // the run-down-a-staircase micro-hops the controller
                    // reports as kInAir) neither absorb nor RELEASE â€” the
                    // offset keeps easing through air frames (tau <= 0.30s,
                    // so nothing lingers into a real fall; the Jumping/
                    // Falling effects own those).
                    //
                    // HISTORY (2026-08-16): two attempts to smooth the
                    // downstairs micro-fall sawtooth were REVERTED the same
                    // day â€” a binary "descent mode" (its engage/disengage
                    // edges were themselves jerks: EMA engagement latency
                    // let the first risers pass raw, landings/turns dropped
                    // the mode mid-flight), then an always-on ramp-deviation
                    // absorb (its 0.35s rate-EMA lag manufactured phantom
                    // deviation at every grade CHANGE â€” a dip at the bottom
                    // of every staircase, float at the top, swimmy camera Z
                    // on uneven terrain; the original design note's warning
                    // about smoothing ALL vertical motion, reproduced in a
                    // fancier form). Downstairs smoothing needs a DATA-FIRST
                    // retry: read the [STAIR] capture of a real descent
                    // before theorizing again.
                    if (grounded && mag >= stepMin && mag < kTeleportMin) {
                        sStairOffset -= dz;   // hold the camera where it was
                    } else if (mag >= kTeleportMin) {
                        sStairOffset = 0.0f;
                    }

                    // Bounded stair-motion capture for Verbose Logging.
                    if (spdlog::should_log(spdlog::level::debug)) {
                        static int sStairDiagBurst = 0;
                        static int sStairDiagLines = 0;
                        if (std::abs(dz) > 4.0f && dxy > 2.0f && sStairDiagLines < 360)
                            sStairDiagBurst = 45;   // ~0.75s window per trigger
                        if (sStairDiagBurst > 0 && sStairDiagLines < 360) {
                            --sStairDiagBurst;
                            ++sStairDiagLines;
                            // Compare the head and camera against the root to
                            // separate animation motion from camera motion.
                            float headRel = std::numeric_limits<float>::quiet_NaN();
                            if (auto* p3 = stairPlayer->Get3D())
                                if (auto* h = p3->GetObjectByName("NPC Head [Head]"))
                                    headRel = h->world.translate.z - rawZ;
                            const float camRel = cameraNI->world.translate.z - rawZ;
                            spdlog::debug("[STAIR] dz={:+.1f} dxy={:.1f} grounded={} "
                                         "stepMin={:.1f} off={:+.1f} dt={:.4f} | "
                                         "head-root={:+.2f} cam-root={:+.2f}",
                                         dz, dxy, grounded ? 1 : 0,
                                         stepMin, sStairOffset, dt, headRel, camRel);
                        }
                    }

                    // Ease the absorbed step away.
                    //
                    // RETUNED 2026-08-19 ("still feels terrible and ineffective
                    // and worse than it was in the beginning"). The two numbers
                    // below were inherited from the old slider's MAXIMUM, and a
                    // maximum is the wrong default for a correction that is now
                    // always on:
                    //   limit 80 let the camera sit 80 units â€” two thirds of the
                    //     player's height â€” BELOW where it belongs while running
                    //     up a flight, because each riser piles on before the
                    //     last has decayed. That sag is its own artefact, and a
                    //     bigger one than the sawtooth it was hiding.
                    //   tau 0.30s meant it never settled between risers at any
                    //     realistic climbing pace, so the sag was the steady
                    //     state rather than a transient.
                    // 24 units is about one and a half risers â€” enough headroom
                    // to swallow a step and a bit of overlap, not enough to read
                    // as the camera sinking. 0.12s returns it between risers
                    // taken at a run, so each step is a short ramp instead of a
                    // sawtooth tooth, and the flight as a whole tracks the
                    // player. Absorption stays FULL: the complaint is that the
                    // steps still come through, so this trades none of the
                    // cancellation, only the lag it was paying for it with.
                    const float limit = 24.0f;
                    (void)stairSet;
                    sStairOffset = std::clamp(sStairOffset, -limit, limit);
                    const float tau = 0.12f;
                    sStairOffset *= std::exp(-dt / tau);
                    if (std::abs(sStairOffset) < 0.01f) sStairOffset = 0.0f;

                    if (sStairOffset != 0.0f) {
                        // Same lockstep write set the Looseness block uses:
                        // renderer, audio listener, occlusion and the engine's
                        // next-frame derivation must all agree. Read the live
                        // translate back so this composes with Looseness rather
                        // than overwriting it.
                        RE::NiPoint3 pos = cameraNI->world.translate;
                        pos.z += sStairOffset;
                        cameraNI->world.translate = pos;
                        if (cameraRoot) {
                            cameraRoot->world.translate = pos;
                            cameraRoot->local.translate = pos;
                        }
                        if (curState == playerCam->cameraStates[RE::CameraState::kThirdPerson].get())
                            static_cast<RE::ThirdPersonState*>(curState)->translation = pos;

                        static bool sStairLogged = false;
                        if (!sStairLogged) {
                            sStairLogged = true;
                            spdlog::debug("[STAIRS] step absorbed (dz={:.1f}, strength={:.2f}, limit={:.0f})",
                                         dz, strength, limit);
                        }
                    }
                }
            }
        }

        // Capture cameraNI->world.rotate's forward column BEFORE any of our
        // writes so we can log what the engine wrote (or our previous-frame
        // write that persisted) into the matrix entering this hook.
        float sNiPreYaw   = 0.0f;
        float sNiPrePitch = 0.0f;
        if (cameraNI) {
            const float fx = cameraNI->world.rotate.entry[0][0];
            const float fy = cameraNI->world.rotate.entry[1][0];
            const float fz = cameraNI->world.rotate.entry[2][0];
            sNiPreYaw   = std::atan2(fx, fy);
            sNiPrePitch = std::asin(std::clamp(-fz, -1.0f, 1.0f));
        }

        if (!settings.dialogueFirstPersonEnabled) {
            s_dialogueFaceLockWasActive = false;
            s_preDialogueValid          = false;
            s_entryTransActive          = false;
            s_faceLockShouldRender      = false;
            return;
        }

        if (!playerCam || !cameraRoot || !playerCam->currentState ||
            playerCam->currentState->id != RE::CameraState::kFirstPerson) {
            s_dialogueFaceLockWasActive = false;
            s_preDialogueValid          = false;
            s_entryTransActive          = false;
            s_faceLockShouldRender      = false;
            return;
        }

        // (Removed weapons-drawn gate. Body-rotation mode works regardless
        // of weapon state because we don't override the camera matrix.)

        // Gate: speaker, OR lastSpeaker when the dialogue menu is open AND
        // not in goodbye state. The lastSpeaker fallback catches transient
        // null-speaker windows during an active conversation. The
        // s_dialogueMenuOpen guard (driven by MenuOpenCloseEvent) ensures
        // the fallback is suppressed once the user exits â€” face-lock then
        // releases the instant the player presses Goodbye/TAB/walks away,
        // regardless of whether the NPC's voice line is still playing.
        // Dialogue "speakers" aren't always Actors â€” talking activators (the
        // Alternate Start Mara statue, shrine/altar activators, etc.) are plain
        // TESObjectREFRs. The old `As<RE::Actor>()` nulled those, so the
        // face-lock never engaged on them at all ("statue won't stay locked
        // on"; the log showed mtm->speaker set but our Actor* null). Take the
        // reference as-is â€” the target-resolve below already handles a missing
        // head bone via the world-bound fallback, and the one Actor-only call
        // (GetMiddleHighProcess) is guarded.
        auto*              mtm     = RE::MenuTopicManager::GetSingleton();
        RE::TESObjectREFR* speaker = nullptr;
        if (mtm) {
            if (auto p = mtm->speaker.get(); p) {
                speaker = p.get();
            } else if (s_dialogueMenuOpen && !mtm->forceGoodbye) {
                if (auto p2 = mtm->lastSpeaker.get(); p2) {
                    speaker = p2.get();
                }
            }
        }


        // Periodic MTM state dump while not face-locked so we can diagnose
        // edge cases (re-entry timing, flag states). Every 500ms.
        {
            static auto lastStateLog = std::chrono::steady_clock::now();
            const auto  nowStateTp   = std::chrono::steady_clock::now();
            if (!speaker && mtm &&
                std::chrono::duration<float>(nowStateTp - lastStateLog).count() > 0.5f) {
                const bool hasSpeaker     = mtm->speaker.get().get()     != nullptr;
                const bool hasLastSpeaker = mtm->lastSpeaker.get().get() != nullptr;
                if (hasSpeaker || hasLastSpeaker ||
                    mtm->isGreetingPlayer || mtm->forceGoodbye) {
                    lastStateLog = nowStateTp;
                    spdlog::info("MTMState: speaker={} lastSpeaker={} greet={} goodbye={}",
                                 hasSpeaker, hasLastSpeaker,
                                 mtm->isGreetingPlayer, mtm->forceGoodbye);
                }
            }
        }

        // Edge-triggered MTM log: fires on any transition of speaker /
        // lastSpeaker / currentTopicInfo / lastTopicInfo / isGreetingPlayer /
        // isSayingGoodbye. Captures the dialogue lifecycle so we can correlate
        // engine signals to face-lock behavior. Tag prefix [DLG] for grep.
        if (mtm) {
            static bool sLogInit         = false;
            static bool sPrevSpeaker     = false;
            static bool sPrevLastSpeaker = false;
            static bool sPrevTopic       = false;
            static bool sPrevLastTopic   = false;
            static bool sPrevGreet       = false;
            static bool sPrevGoodbye     = false;
            const bool curSpeaker     = mtm->speaker.get().get()     != nullptr;
            const bool curLastSpeaker = mtm->lastSpeaker.get().get() != nullptr;
            const bool curTopic       = mtm->currentTopicInfo        != nullptr;
            const bool curLastTopic   = mtm->lastTopicInfo           != nullptr;
            const bool curGreet       = mtm->isGreetingPlayer;
            const bool curGoodbye     = mtm->forceGoodbye;
            if (sLogInit && (curSpeaker     != sPrevSpeaker     ||
                             curLastSpeaker != sPrevLastSpeaker ||
                             curTopic       != sPrevTopic       ||
                             curLastTopic   != sPrevLastTopic   ||
                             curGreet       != sPrevGreet       ||
                             curGoodbye     != sPrevGoodbye)) {
                spdlog::debug("[DLG] MTM edge: spk={} last={} topic={} lastTopic={} greet={} bye={} | active={} releasing={}",
                             curSpeaker, curLastSpeaker, curTopic, curLastTopic, curGreet, curGoodbye,
                             s_dialogueFaceLockWasActive, s_dialogueFaceLockReleasing);
            }
            sPrevSpeaker     = curSpeaker;
            sPrevLastSpeaker = curLastSpeaker;
            sPrevTopic       = curTopic;
            sPrevLastTopic   = curLastTopic;
            sPrevGreet       = curGreet;
            sPrevGoodbye     = curGoodbye;
            sLogInit         = true;
        }

        // Detect the speaker-edge so re-entry during a lingering release
        // (or during a previous NPC's still-playing greeting) forces a
        // clean spring re-init instead of carrying over stale state.
        static bool sLastSpeakerWasSet = false;
        const bool  speakerIsSet       = speaker != nullptr;
        const bool  speakerEdge        = speakerIsSet && !sLastSpeakerWasSet;
        sLastSpeakerWasSet             = speakerIsSet;
        if (speakerEdge && mtm) {
            spdlog::info("GateEdge speaker set: isGreetingPlayer={} isSayingGoodbye={}",
                         mtm->isGreetingPlayer, mtm->forceGoodbye);
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            s_dialogueFaceLockWasActive = false;
            s_dialogueFaceLockReleasing = false;
            s_faceLockShouldRender      = false;
            return;
        }

        const auto nowTpTop = std::chrono::steady_clock::now();

        // Release "grace period". When the speaker gate clears but we
        // were just active, keep writing the frozen exit matrix (camera
        // stays locked on the face) AND keep writing data.angle to the
        // same face orientation. This pins the engine's pitch spring at
        // face during the window so when we finally stop overriding,
        // engine's natural render already matches our last write â€” no
        // snap at handoff. Crossfading to data.angle failed because the
        // engine drifts data.angle.x during the window (idle anim or
        // similar), pulling the camera upward visibly.
        bool releaseGrace = false;
        if (!speaker) {
            if (!s_dialogueFaceLockWasActive) {
                s_dialogueFaceLockReleasing = false;
                return;
            }
            if (!s_dialogueFaceLockReleasing) {
                s_dialogueFaceLockReleasing    = true;
                s_dialogueFaceLockReleaseStart = nowTpTop;
                s_dialogueFaceLockExitYaw      = s_dialogueFaceLockYaw;
                s_dialogueFaceLockExitPitch    = s_dialogueFaceLockPitch;
                spdlog::debug("[DLG] grace BEGIN exitYaw={:.3f} exitPitch={:.3f} | spring=({:.3f},{:.3f}) vel=({:.3f},{:.3f}) | dataAngle=({:.3f},{:.3f}) niPre=({:.3f},{:.3f})",
                             s_dialogueFaceLockExitYaw, s_dialogueFaceLockExitPitch,
                             s_dialogueFaceLockYaw, s_dialogueFaceLockPitch,
                             s_dialogueFaceLockVelYaw, s_dialogueFaceLockVelPitch,
                             player->data.angle.x, player->data.angle.z,
                             sNiPreYaw, sNiPrePitch);
            }
            const float releaseElapsed = std::chrono::duration<float>(nowTpTop - s_dialogueFaceLockReleaseStart).count();
            if (releaseElapsed >= kDialogueFaceLockReleaseSec || !cameraNI) {
                spdlog::debug("[DLG] grace END after {:.3f}s | spring=({:.3f},{:.3f}) preDialogue=({:.3f},{:.3f}) | dataAngle=({:.3f},{:.3f}) niPre=({:.3f},{:.3f})",
                             releaseElapsed,
                             s_dialogueFaceLockYaw, s_dialogueFaceLockPitch,
                             s_preDialogueYaw, s_preDialoguePitch,
                             player->data.angle.x, player->data.angle.z,
                             sNiPreYaw, sNiPrePitch);
                s_dialogueFaceLockWasActive = false;
                s_dialogueFaceLockReleasing = false;
                s_preDialogueValid          = false;
                s_entryTransActive          = false;
                s_faceLockShouldRender      = false;
                // Arm the noise release-tail: keep noise composited on the
                // engine-controlled camera for a short settle window so it
                // doesn't flick off as control hands back.
                s_faceLockNoiseTailUntil    = nowTpTop + std::chrono::milliseconds(400);
                return;
            }
            releaseGrace = true;
        }

        // Compute target yaw/pitch for the spring. During active face-lock
        // the target is the NPC head bone, with an entry-window blend
        // from entryStart â†’ face for the first 300ms (smooth pan-in).
        // During release grace the target is frozen at the exit orientation
        // â€” the spring sits still and data.angle stays pinned to that
        // orientation, so the engine takes over at the same value when
        // grace ends. Camera does not actively move during exit.
        float targetYaw   = 0.0f;
        float targetPitch = 0.0f;
        if (speaker) {
            RE::NiPoint3 facePos;
            bool         haveFacePos = false;
            const char*  faceSrc     = "head";
            auto*        npcRoot  = speaker->Get3D();
            auto*        headNode = npcRoot ? npcRoot->GetObjectByName("NPC Head [Head]") : nullptr;
            // Dragons/creatures lack a "NPC Head [Head]" bone; fall back to the
            // engine's race-independent cached head node so they lock to the head.
            // Actor-only call â€” talking-activator speakers (statues) skip it and
            // fall through to the world-bound aim below.
            if (!headNode) {
                if (auto* spkActor = speaker->As<RE::Actor>()) {
                    if (auto* mh = spkActor->GetMiddleHighProcess()) headNode = mh->headNode;
                }
            }
            if (headNode) {
                facePos     = headNode->world.translate;
                haveFacePos = true;
            } else if (npcRoot) {
                // Headless target â€” e.g. the Alternate Start Mara statue: an actor
                // with a static mesh and no skeleton/head bone. The old
                // GetPosition()+120 fallback aimed a fixed 120u above the ref
                // ORIGIN, which on a tall statue (origin at its base) lands in
                // empty air partway up, so the lock held the view on nothing and
                // fought you. Aim at the model's world-bound CENTER instead (the
                // geometric middle of the visible mesh) biased up toward the top
                // third, so any headless object reads as "looking at it".
                const auto& wb = npcRoot->worldBound;
                if (wb.radius > 1.0f) {
                    facePos     = wb.center;
                    facePos.z  += wb.radius * 0.35f;
                    haveFacePos = true;
                    faceSrc     = "worldbound";
                }
            }
            if (!haveFacePos) {
                facePos    = speaker->GetPosition();
                facePos.z += 120.0f;
                faceSrc    = "pos+120";
            }
            // One-shot-per-speaker diagnostic so a future "still wrong on X"
            // report pins which fallback fired and where it aimed.
            {
                static RE::TESObjectREFR* sLastFaceSrcActor = nullptr;
                if (speaker != sLastFaceSrcActor) {
                    sLastFaceSrcActor = speaker;
                    const char* nm = speaker->GetName();
                    spdlog::debug("[DLG-FACE] speaker='{}' src={} target=({:.0f},{:.0f},{:.0f})",
                                 nm ? nm : "?", faceSrc, facePos.x, facePos.y, facePos.z);
                }
            }
            const auto&  camPos = cameraRoot->world.translate;
            RE::NiPoint3 toFace{ facePos.x - camPos.x, facePos.y - camPos.y, facePos.z - camPos.z };
            const float  flen   = std::sqrt(toFace.x * toFace.x + toFace.y * toFace.y + toFace.z * toFace.z);
            if (flen < 1.0f) return;
            toFace.x /= flen; toFace.y /= flen; toFace.z /= flen;
            const float faceYaw   = std::atan2(toFace.x, toFace.y);
            const float facePitch = std::asin(std::clamp(-toFace.z, -1.0f, 1.0f));

            // Force-1p entry snap: bypass the entry lerp AND seed the
            // spring's position/velocity to the face target. Combined
            // with the targetYaw=faceYaw write below (entry-trans off
            // branch), the spring is at-target from frame zero â€” no
            // visible motion, instant face center. One-shot.
            if (s_dialogueSnapFaceLockNextFrame) {
                s_entryTransActive          = false;
                s_dialogueFaceLockYaw       = faceYaw;
                s_dialogueFaceLockPitch     = facePitch;
                s_dialogueFaceLockVelYaw    = 0.0f;
                s_dialogueFaceLockVelPitch  = 0.0f;
                s_dialogueSmoothedTargetYaw   = faceYaw;
                s_dialogueSmoothedTargetPitch = facePitch;
                s_dialogueAimYawVel           = 0.0f;
                s_dialogueAimPitchVel         = 0.0f;
                s_dialogueSnapFaceLockNextFrame = false;
            }

            if (s_entryTransActive) {
                // Entry transition is intentionally MUCH shorter than the
                // FOV transition. Reference mods (ICSE-NG, SmoothCam) all
                // decouple these: face-lock engages fast (~100-250ms), FOV
                // takes longer (~1-1.4s). The user perceives "lock first,
                // then watch the world zoom" rather than "camera + FOV
                // both moving together for a long time" (which reads as
                // panning, not locked). 200ms is well above the snap
                // threshold (~50-100ms) but short enough that the FOV
                // transition is mostly observed with camera already locked.
                constexpr float entryDuration = 0.2f;
                const float elapsed = std::chrono::duration<float>(nowTpTop - s_entryTransStart).count();
                if (elapsed >= entryDuration) {
                    s_entryTransActive = false;
                    targetYaw   = faceYaw;
                    targetPitch = facePitch;
                } else {
                    const float t     = elapsed / entryDuration;
                    const float blend = t * t * (3.0f - 2.0f * t);  // smoothstep
                    // Yaw shortest-arc unwrap before lerp.
                    float ydiff = faceYaw - s_entryStartYaw;
                    while (ydiff >  3.14159265f) ydiff -= 6.28318530f;
                    while (ydiff < -3.14159265f) ydiff += 6.28318530f;
                    const float unwrappedFaceYaw = s_entryStartYaw + ydiff;
                    targetYaw   = std::lerp(s_entryStartYaw,   unwrappedFaceYaw, blend);
                    targetPitch = std::lerp(s_entryStartPitch, facePitch,        blend);
                }
            } else {
                targetYaw   = faceYaw;
                targetPitch = facePitch;
            }
        } else if (releaseGrace) {
            // Frozen-hold release: target stays at the exit orientation,
            // spring sits still (target == current state), data.angle stays
            // pinned to that orientation via the spring write below. After
            // grace ends, we stop overriding and the engine renders from
            // the still-pinned data.angle â€” no camera motion. User feedback
            // explicitly: exit "doesn't need to move the camera at all";
            // earlier attempt to lerp toward a captured pre-dialogue
            // snapshot moved the camera to the lower-left because pre-
            // dialogue orientation was not the user's expected resting
            // place by the time they exited.
            targetYaw   = s_dialogueFaceLockExitYaw;
            targetPitch = s_dialogueFaceLockExitPitch;
        }

        // Initialize the spring at the ENGINE'S CURRENT rendered camera
        // orientation on the first active frame (read from cameraNI's
        // col0 = forward direction in world). Using player->data.angle
        // here produced a one-frame jump â€” the engine's 1p pitch spring
        // lags data.angle by ~1s, so data.angle â‰  the orientation the
        // player actually sees on screen. Sampling the NiCamera directly
        // means the spring starts exactly where the pixels were last frame.
        // Force re-init on speaker-edge OR if we were in the release grace
        // (a new activation arrived mid-release-window) so re-entry during
        // a previous greeting's goodbye voice always captures a fresh
        // spring state â€” no stale data carried over from the prior dialogue.
        const bool forceInit = speakerEdge || s_dialogueFaceLockReleasing;
        if (speaker && (!s_dialogueFaceLockWasActive || forceInit)) {
            if (cameraNI) {
                const float viewX = cameraNI->world.rotate.entry[0][0];
                const float viewY = cameraNI->world.rotate.entry[1][0];
                const float viewZ = cameraNI->world.rotate.entry[2][0];
                s_dialogueFaceLockYaw   = std::atan2(viewX, viewY);
                s_dialogueFaceLockPitch = std::asin(std::clamp(-viewZ, -1.0f, 1.0f));
            } else {
                s_dialogueFaceLockYaw   = player->data.angle.z;
                s_dialogueFaceLockPitch = player->data.angle.x;
            }
            s_dialogueFaceLockVelYaw    = 0.0f;
            s_dialogueFaceLockVelPitch  = 0.0f;
            s_dialogueFaceLockLastFrame = nowTpTop;
            s_dialogueFaceLockWasActive = true;
            s_dialogueFaceLockReleasing = false;

            // Entry transition: capture current orientation as the start
            // of a 200ms target lerp toward the NPC face (duration hardcoded
            // in the entry-target branch below; intentionally short relative
            // to the FOV transition so face-lock engages well before FOV
            // settles). Spring stays at 7Ã— stiffness for tight tracking.
            s_entryStartYaw   = s_dialogueFaceLockYaw;
            s_entryStartPitch = s_dialogueFaceLockPitch;
            s_entryTransStart = nowTpTop;
            s_entryTransActive = true;

            // Pre-dialogue snapshot: capture once per conversation, on the
            // very first speaker-edge. Cleared on full grace-end, so a
            // re-entry during grace preserves the original pre-dialogue
            // value (it's still where the user was looking before this
            // conversation started).
            if (!s_preDialogueValid) {
                s_preDialogueYaw   = s_dialogueFaceLockYaw;
                s_preDialoguePitch = s_dialogueFaceLockPitch;
                s_preDialogueValid = true;
                spdlog::debug("[DLG] preDialogue captured yaw={:.3f} pitch={:.3f}",
                             s_preDialogueYaw, s_preDialoguePitch);
            }

            spdlog::info("UpdateCameraPost: face-lock engaged, spring init yaw={:.2f} pitch={:.2f}",
                         s_dialogueFaceLockYaw, s_dialogueFaceLockPitch);
        }

        // dt with EMA-style clamping (matches CameraController's smoothDt bounds).
        float dt = std::chrono::duration<float>(nowTpTop - s_dialogueFaceLockLastFrame).count();
        dt = std::clamp(dt, 0.001f, 0.1f);
        s_dialogueFaceLockLastFrame = nowTpTop;

        float yaw   = 0.0f;
        float pitch = 0.0f;

        // Critical-damped spring step (identical pattern to CameraController:574).
        // Runs during active face-lock AND release grace. The target was
        // computed above with the appropriate transition lerps. The spring
        // (7Ã— stiffness for steady-state face tracking) chases the
        // smoothly-moving target â†’ smooth motion in both directions.
        // Extra parens on std::max defeat Windows.h's max() macro (HookManager.cpp
        // doesn't #define NOMINMAX because other hooks pull in Windows.h first).
        if (speaker || releaseGrace) {
            // Face-lock fires only during dialogue. Reads the dialogue
            // menu's per-channel sliders, NOT the global Transitions
            // menu, so dialogue feel is independent. 4.0 = former
            // transitionBaseSpeed default (0.5) Ã— 8. The 1.5 inside
            // kFaceLockStiffnessBoost preserves the historical 1.5Ã—
            // pitch/rotation feel from before those mul defaults moved
            // to 1.0.
            constexpr float omegaBase               = 4.0f;
            constexpr float kFaceLockStiffnessBoost = 7.0f * 1.5f;
            const float omegaYaw     = omegaBase * (std::max)(0.05f, settings.dialogueMulRotation) * kFaceLockStiffnessBoost;
            const float omegaPitchCh = omegaBase * (std::max)(0.05f, settings.dialogueMulPitch)    * kFaceLockStiffnessBoost;

            // Yaw: unwrap shortest arc before stepping.
            {
                float diff = targetYaw - s_dialogueFaceLockYaw;
                while (diff >  3.14159265f) diff -= 6.28318530f;
                while (diff < -3.14159265f) diff += 6.28318530f;
                const float unwrappedTarget = s_dialogueFaceLockYaw + diff;
                CriticalDampedSpringExact(s_dialogueFaceLockYaw, s_dialogueFaceLockVelYaw, unwrappedTarget, omegaYaw, dt);
            }
            CriticalDampedSpringExact(s_dialogueFaceLockPitch, s_dialogueFaceLockVelPitch, targetPitch, omegaPitchCh, dt);

            yaw   = s_dialogueFaceLockYaw;
            pitch = s_dialogueFaceLockPitch;

            // Pin player->data.angle to the spring's CURRENT state (not to
            // a fixed face value). During active this still converges the
            // engine pitch spring toward face. During grace it follows the
            // spring along the lerp toward pre-dialogue, so at grace-end
            // data.angle == spring == pre-dialogue â†’ engine renders the
            // same orientation we just wrote, invisible handoff.
            player->data.angle.x = pitch;
            player->data.angle.z = yaw;
        }

        // Build orthonormal basis from yaw/pitch and write the matrix.
        RE::NiPoint3 forward{
            std::sin(yaw) * std::cos(pitch),
            std::cos(yaw) * std::cos(pitch),
            -std::sin(pitch)
        };
        RE::NiPoint3 right{
            forward.y * 1.0f - forward.z * 0.0f,
            forward.z * 0.0f - forward.x * 1.0f,
            forward.x * 0.0f - forward.y * 0.0f
        };
        const float rlen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
        if (rlen < 0.001f) return;
        right.x /= rlen; right.y /= rlen; right.z /= rlen;
        RE::NiPoint3 up{
            right.y * forward.z - right.z * forward.y,
            right.z * forward.x - right.x * forward.z,
            right.x * forward.y - right.y * forward.x
        };

        RE::NiMatrix3 m{};
        m.entry[0][0] = forward.x; m.entry[0][1] = up.x; m.entry[0][2] = right.x;
        m.entry[1][0] = forward.y; m.entry[1][1] = up.y; m.entry[1][2] = right.y;
        m.entry[2][0] = forward.z; m.entry[2][1] = up.z; m.entry[2][2] = right.z;

        // Compose this frame's 1p camera noise onto the face-lock orientation so
        // the locked dialogue view keeps its "life". Our outright world.rotate
        // assignment below (and the render-hook re-apply) would otherwise wipe
        // the noise CameraNoiseController wrote, leaving the camera dead-still â€”
        // exactly the "noise doesn't work in 1p dialogue" report. Noise rides
        // ONLY the rendered matrix, never player->data.angle, so the spring/
        // handoff target stays clean.
        //
        // Compose through BOTH active face-lock AND the release-grace window.
        // After grace ends the engine re-takes the camera and the direct 1p-noise
        // path applies this SAME delta to it, so the noise is present on both
        // sides of the handoff â€” keeping it on through grace makes the transition
        // continuous. (Gating it to active-only left a ~0.3s no-noise gap during
        // grace, so the noise popped back in when dialogue fully closed â€” the
        // "brief jitter on exit".) We're past the !speaker/!grace early-return
        // here, so this only runs while actively locked or in grace.
        {
            RE::NiMatrix3 noiseDelta;
            if (CameraNoiseController::GetSingleton().Get1pNoiseDelta(noiseDelta)) {
                m = m * noiseDelta;
            }
        }

        // Only write cameraNI->world.rotate. Writing cameraRoot->world.rotate
        // too was polluting the scene-graph recompose post-release: even
        // after we stopped overriding cameraNI, the engine recomputed
        // cameraNI->world = cameraRoot.world * cameraNI.local, and since
        // cameraRoot.world was still holding our face-locked matrix, the
        // recomposed cameraNI.world came out rotated incorrectly â€” the
        // "ceiling flash" on exit. Touching only cameraNI keeps cameraRoot
        // at its engine-natural value throughout.
        if (cameraNI) {
            cameraNI->world.rotate = m;
        }

        // Save the matrix for the NiCamera::UpdateWorldData renderer-side
        // override hook. The engine's scene-graph recompose runs after our
        // +0x1A6 hook returns and was rebuilding the rotation from data.angle
        // + an internal pitch source we couldn't suppress, defeating our
        // matrix write here. The NiCamera vtable hook re-applies this matrix
        // post-recompose so the rendered orientation is ours.
        s_faceLockRenderMatrix = m;
        s_faceLockShouldRender = true;

        // Per-frame state log, throttled to every 100ms while active or in
        // grace. Captures everything: gate, dt, target, current spring state,
        // exit values, velocities, data.angle, what cameraNI held at hook
        // entry (sNiPre = engine-natural OR our last-frame write, depends),
        // what we just wrote (yaw/pitch). Dumping niPre vs the written
        // matrix on consecutive frames is how we tell whether the engine
        // refreshes cameraNI between our writes.
    }

    void HookManager::InstallUpdateCameraPostHook()
    {
        auto& trampoline = SKSE::GetTrampoline();
        const auto callSite = RuntimeHooks::Get().cameraUpdate;
        RuntimeHooks::RequireCall(callSite);
        _originalUpdateCameraPost = trampoline.write_call<5>(
            callSite, &HookedUpdateCameraPost);
        spdlog::info("HookManager: UpdateCameraPost trampoline installed at 0x{:x}",
                     callSite);
    }

    // NiCamera::UpdateWorldData hook body. The engine calls this for every
    // NiCamera during scene-graph propagation; we run AFTER the original so
    // ===== [SSFIX2] SHIELD-SPRINT CAMERA INJECTION, CANCELLED AT THE ======
    // ===== STAGE THE 3P LAYERS WRITE ======================================
    //
    // The shield charge's camera track injects downward pitch (~15 deg,
    // measured) and a height lift into cameraRoot->local — the engine's own
    // post-compose transform. Three write points were tried and two are now
    // closed by measurement:
    //   - GetRotation: composes CLEAN; the injection lands after it.
    //   - NiCamera world, post-propagation: corrections reach scene geometry
    //     but the SKY samples the camera elsewhere — "the entire sky shakes".
    //   - HERE, cameraRoot->local from the TESCamera::Update thunk: the same
    //     stage every 3p noise layer writes, which renders world and sky
    //     together. This is the one that is left, and it is also where the
    //     injection itself was measured (rootLz swinging to -0.390 with both
    //     pitch inputs still).
    //
    // Scope is the resolver's own Shield Sprinting entry — outdoor or indoor
    // twin — so plain sprint bob and every other camera animation stay
    // vanilla (the user's explicit instruction after a sprint-wide version
    // ate the stride bob). Corrections are slew-limited, which is the
    // entry/exit answer: fast enough to track the charge's own ramp, so no
    // single-frame step in either direction, and after the scope drops they
    // keep tracking the injection until it has faded rather than releasing on
    // a flag edge.
    // ===== [ANIMCAM] the animated-camera window ============================
    //
    // Sink thread writes the flag; the camera thread reads it. Timestamp is
    // millis-since-epoch in an atomic so no lock is needed.
    static std::atomic<bool>      sAnimCamWindow{ false };
    static std::atomic<long long> sAnimCamT0{ 0 };

    void HookManager::NotifyAnimatedCameraEvent(bool a_start)
    {
        if (a_start) {
            sAnimCamT0.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_relaxed);
            sAnimCamWindow.store(true, std::memory_order_release);
        } else {
            sAnimCamWindow.store(false, std::memory_order_release);
        }
    }

    namespace
    {
        // Camera-thread state for the hold. Confirmed = the resolver said
        // Shield Sprinting at some frame of this window (sticky until the
        // window closes, so the resolver's 1-3 frame lag cannot re-open the
        // dip mid-charge). The pose is latched on the first confirmed frame —
        // the delta mode measures against its own start pose, so holding the
        // bone constant from here makes the applied delta ~zero.
        bool          sAnimCamConfirmed = false;
        bool          sAnimCamLatched   = false;
        RE::NiTransform sAnimCamPose{};
        int           sAnimCamToggleSeen = 0;   // frames toggleAnimCam read TRUE this window

        void AnimCamCloseWindowReport()
        {
            // Mirror the close into the event-thread atomics (suppress leg 3
            // reads them): a confirmed window closing arms the 1s exit
            // shield against the stop-animation's own Start event.
            if (sAnimCamConfirmed) {
                sShieldChargeOpen.store(false, std::memory_order_relaxed);
                sShieldChargeCloseMs.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_relaxed);
            }
            // The adjudication line — one per window, fires on close either
            // way, so a run that still tilts SAYS which mechanism survived.
            if (sAnimCamConfirmed) {
                static int sACSum = 0;
                if (sACSum < 12) {
                    ++sACSum;
                    spdlog::debug("[ANIMCAM] window closed — toggleAnimCam read TRUE on {} "
                                 "frame(s) this window (cleared pre-Update each time)",
                                 sAnimCamToggleSeen);
                }
            }
            sAnimCamToggleSeen = 0;
        }

        void AnimCamHoldTick(RE::ThirdPersonState* a_tps)
        {
            if (!sAnimCamWindow.load(std::memory_order_acquire)) {
                AnimCamCloseWindowReport();
                sAnimCamConfirmed = false;
                sAnimCamLatched   = false;
                return;
            }
            // 4s hard cap — an End event lost to a state purge must never
            // leave the hold latched on.
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - sAnimCamT0.load(std::memory_order_relaxed) > 4000) {
                sAnimCamWindow.store(false, std::memory_order_release);
                AnimCamCloseWindowReport();
                sAnimCamConfirmed = false;
                sAnimCamLatched   = false;
                return;
            }
            if (!a_tps || a_tps->id != RE::CameraState::kThirdPerson) return;

            if (!sAnimCamConfirmed) {
                auto& setAC = SettingsManager::GetSingleton();
                const CameraProfile* res = CameraController::GetLastResolvedProfile();
                if (res && (res == &setAC.weaponsBlockingShieldSprint ||
                            res == setAC.IndoorVariantOf(&setAC.weaponsBlockingShieldSprint))) {
                    sAnimCamConfirmed = true;
                    sShieldChargeOpen.store(true, std::memory_order_relaxed);
                    // The mid-sprint-block leak: this window opened from a
                    // FORWARDED plain-sprint Start and has just turned out
                    // to be a charge — the mode is live and no new Start is
                    // coming. Close it now, one-two frames after the block
                    // press, while the applied delta is still near zero.
                    if (sAnimCamLastStartForwarded.exchange(false, std::memory_order_relaxed)) {
                        if (auto* plyAC = RE::PlayerCharacter::GetSingleton()) {
                            CloseAnimatedCameraMode(*plyAC);
                            spdlog::debug("[ANIMCAM] window confirmed as a charge with a "
                                         "FORWARDED start — closed the live animated-camera "
                                         "mode (mid-sprint block raise)");
                        }
                    }
                }
            }
            if (!sAnimCamConfirmed) return;

            // ----- state-side clear: ADJUDICATED INERT (16:37 run) ---------
            // toggleAnimCam read TRUE on 0 frames across every window —
            // including the 16:37:57 leaked one, where the mode was live and
            // the full tilt rendered while this clear ran every frame. The
            // flag is NOT where the mode lives. Kept only as a zero-cost
            // probe: a nonzero count in the window-close report would mean
            // the engine changed shape.
            if (a_tps->toggleAnimCam) {
                a_tps->toggleAnimCam = false;
                ++sAnimCamToggleSeen;
            }
            a_tps->animationRotation.w = 1.0f;
            a_tps->animationRotation.x = 0.0f;
            a_tps->animationRotation.y = 0.0f;
            a_tps->animationRotation.z = 0.0f;

            auto* bone = a_tps->thirdPersonCameraObj;
            if (!bone) return;
            if (!sAnimCamLatched) {
                sAnimCamPose    = bone->local;
                sAnimCamLatched = true;
                static int sACLog = 0;
                if (sACLog < 12) {
                    ++sACLog;
                    spdlog::debug("[ANIMCAM] shield-charge camera window confirmed "
                                 "(Cam3 entry pose T=({:.1f},{:.1f},{:.1f}))",
                                 sAnimCamPose.translate.x, sAnimCamPose.translate.y,
                                 sAnimCamPose.translate.z);
                }
            }
            // THE BONE HOLD WRITE WAS REMOVED 2026-08-29. It was proven
            // useless for its purpose (the mode reads the graph's camera
            // track, not this node — measured 2026-08-27), but it kept
            // running anyway, pinning the bone all window and then RELEASING
            // it into its mid-animation pose: local.translate.y snapped
            // -0.3 → -43.8 in one frame at exactly the window close, the
            // one moment the user's "exit jerk" report names. Holding a bone
            // we know feeds nothing buys nothing; releasing it into a snap
            // is a discontinuity vanilla never makes. The window/confirm
            // machinery above stays — suppress leg 3 and the close report
            // depend on it.
        }
    }

    void HookManager::ApplyAnimCamBoneHold(RE::TESCamera* a_camera)
    {
        if (!a_camera || !a_camera->currentState) return;
        if (a_camera->currentState->id != RE::CameraState::kThirdPerson) return;
        AnimCamHoldTick(static_cast<RE::ThirdPersonState*>(a_camera->currentState.get()));
    }

    // The frame's latched correction, shared with HookedNiCameraUpdateWorldData.
    //
    // MEASURED 2026-08-27 21:01, and it is the whole architecture: the SKY and
    // the WORLD sample the camera from DIFFERENT stages. Writing the NiCamera's
    // world transform fixed the world's pitch and sheared the sky; writing
    // cameraRoot->local moved the sky and left the world's pitch untouched
    // ([SSFIX2] corr==dev every frame while [SSPRINT] showed the render pinned
    // at +0.2675 for 125 frames). So the SAME correction must land on BOTH
    // stages — computed ONCE per frame here, applied identically at every
    // NiCamera fire there. Recomputing per fire is what made the first version
    // shimmer: actor state moves between fires, so per-fire recompute gives
    // each consumer a slightly different pose. The 1p noise applier documents
    // the same rule ("every fire re-multiplies the same axes/thetas").
    static float sSSFramePhi = 0.0f;   // rotation, latched per frame

    void HookManager::EnforceShieldSprintCamera(RE::TESCamera* a_camera)
    {
        sSSFramePhi = 0.0f;
        if (!a_camera || !a_camera->cameraRoot || !a_camera->currentState ||
            a_camera->currentState->id != RE::CameraState::kThirdPerson) {
            return;
        }
        auto* tps = static_cast<RE::ThirdPersonState*>(a_camera->currentState.get());
        auto* ply = RE::PlayerCharacter::GetSingleton();
        if (!ply) return;

        auto& set = SettingsManager::GetSingleton();
        const CameraProfile* resolved = CameraController::GetLastResolvedProfile();
        const bool inScope = resolved &&
            (resolved == &set.weaponsBlockingShieldSprint ||
             resolved == set.IndoorVariantOf(&set.weaponsBlockingShieldSprint));

        auto&       L     = a_camera->cameraRoot->local;
        const float ident = ply->data.angle.x - tps->freeRotation.y;
        // Basis, measured not assumed: root local's z-row carries
        // (-~0, -sin(pitch), cos(pitch)) — column 1 is the pitch sine
        // (clean frames -0.133 = -sin(bodyX), charge frames -0.390), and the
        // check below refuses to touch a matrix that does not match the
        // model rather than rotating the wrong plane.
        const float p       = std::asin(std::clamp(-L.rotate.entry[2][1], -1.0f, 1.0f));
        const bool  basisOk = std::abs(L.rotate.entry[2][2] - std::cos(p)) < 0.06f &&
                              std::abs(L.rotate.entry[2][0]) < 0.06f;
        const float dev       = p - ident;
        const float heightNow = L.translate.z - ply->GetPosition().z;

        static float sPitchBase = 0.0f;  static bool sPBInit = false;
        static float sHeightBase = 0.0f; static bool sHBInit = false;
        static float sCorrR = 0.0f, sCorrH = 0.0f;   // slewed live corrections
        static bool  sHold  = false;                 // decay-out latch
        static auto  sPrevT = std::chrono::steady_clock::now();
        static auto  sExitT = sPrevT;

        const auto nowT = std::chrono::steady_clock::now();
        const float dt  = std::clamp(
            std::chrono::duration<float>(nowT - sPrevT).count(), 0.0001f, 0.05f);
        sPrevT = nowT;

        if (!inScope && !sHold) {
            // Clean frames: learn what "correct" looks like, write nothing.
            // The pitch baseline carries any deliberate pitch offset; the
            // height baseline tracks look pitch / zoom drift via the EMA.
            if (basisOk && std::abs(dev) < 0.15f) {
                sPitchBase = sPBInit ? sPitchBase + 0.05f * (dev - sPitchBase) : dev;
                sPBInit    = true;
            }
            if (heightNow > 10.0f && heightNow < 500.0f) {
                sHeightBase = sHBInit ? sHeightBase + 0.08f * (heightNow - sHeightBase)
                                      : heightNow;
                sHBInit     = true;
            }
        }

        bool active = false;
        if ((inScope || sHold) && sPBInit && sHBInit && basisOk) {
            if (inScope) {
                sHold  = true;
                sExitT = nowT;
            } else {
                const bool residual =
                    std::abs(dev - sPitchBase) > 0.012f ||
                    std::abs(heightNow - sHeightBase) > 1.5f;
                const float sinceExit =
                    std::chrono::duration<float>(nowT - sExitT).count();
                if (!residual || sinceExit > 1.5f) sHold = false;
            }
            active = inScope || sHold;
        }
        if (!basisOk) {
            static bool sWarned = false;
            if (!sWarned && inScope) {
                sWarned = true;
                spdlog::warn("[SSFIX2] root basis does not match the measured model "
                             "(z-row=({:+.3f},{:+.3f},{:+.3f})) — refusing to correct",
                             L.rotate.entry[2][0], L.rotate.entry[2][1],
                             L.rotate.entry[2][2]);
            }
        }

        // Slew toward the live targets. 3 rad/s and 400 u/s reach a full
        // charge-sized correction in ~0.1s — faster than the charge's own
        // blend-in, so it tracks rather than steps, in both directions.
        const float tgtR = active
            ? std::clamp((ident + sPitchBase) - p, -0.45f, 0.45f) : 0.0f;
        const float tgtH = active
            ? std::clamp(sHeightBase - heightNow, -120.0f, 120.0f) : 0.0f;
        sCorrR += std::clamp(tgtR - sCorrR, -3.0f * dt, 3.0f * dt);
        sCorrH += std::clamp(tgtH - sCorrH, -400.0f * dt, 400.0f * dt);
        if (std::abs(sCorrR) < 1.0e-4f && std::abs(sCorrH) < 0.05f) return;

        if (std::abs(sCorrR) > 1.0e-4f) {
            // Pitch-only rotation in root-local terms: mixes columns 1 and 2
            // (sine and cosine carriers), leaves column 0 untouched. This is
            // the SKY's stage; the world's stage gets the identical latched
            // value at the NiCamera hook.
            const float c = std::cos(sCorrR), s = std::sin(sCorrR);
            for (int r = 0; r < 3; ++r) {
                const float c1 = L.rotate.entry[r][1];
                const float c2 = L.rotate.entry[r][2];
                L.rotate.entry[r][1] = c1 * c - c2 * s;
                L.rotate.entry[r][2] = c1 * s + c2 * c;
            }
            sSSFramePhi = sCorrR;
        }
        if (std::abs(sCorrH) > 0.05f) {
            L.translate.z += sCorrH;
        }
        static int sLog = 0;
        if (sLog < 40 && (std::abs(tgtR) > 0.02f || std::abs(tgtH) > 3.0f)) {
            ++sLog;
            spdlog::debug("[SSFIX2] corr pitch={:+.4f} ({:+.2f} deg) height={:+.1f}u "
                         "| dev={:+.4f} lift={:+.1f} inScope={} hold={}",
                         sCorrR, sCorrR * 57.2958f, sCorrH,
                         dev, heightNow - sHeightBase,
                         inScope ? 1 : 0, sHold ? 1 : 0);
        }
    }

    // the engine's recompose has finished, then reapply our face-lock matrix
    // ONLY when (a) face-lock is active and (b) this NiCamera is the player's.
    void HookManager::HookedNiCameraUpdateWorldData(RE::NiCamera* a_this, RE::NiUpdateData* a_data)
    {
        TweenCameraTrace::RenderSample renderTrace(a_this);
        _originalNiCameraUpdateWorldData(a_this, a_data);

        // ----- [SSFIX2] WORLD-STAGE HALF ------------------------------------
        //
        // Applies the SAME rotation EnforceShieldSprintCamera latched this
        // frame — the world render does not read cameraRoot->local.rotate
        // (measured: root corrected, render pinned at +0.2675 for 125
        // frames), so the world's stage is corrected here and the sky's
        // stage there, with one identical per-frame value. Never recompute
        // from live state in this hook: it fires several times per frame and
        // actor state moves between fires, so per-fire recompute hands the
        // sky and the geometry different poses — that WAS the shimmer.
        // Height is deliberately absent here: the root-local write carries it.
        if (std::abs(sSSFramePhi) > 1.0e-4f && a_this) {
            auto* pcW = RE::PlayerCamera::GetSingleton();
            bool  isPlayerCamW = false;
            if (pcW && pcW->cameraRoot && pcW->currentState &&
                pcW->currentState->id == RE::CameraState::kThirdPerson) {
                if (auto* rootW = pcW->cameraRoot->AsNode()) {
                    for (auto& ch : rootW->GetChildren()) {
                        if (skyrim_cast<RE::NiCamera*>(ch.get()) ==
                            static_cast<RE::NiAVObject*>(a_this)) {
                            isPlayerCamW = true;
                            break;
                        }
                    }
                }
            }
            if (isPlayerCamW) {
                // World basis carries the pitch sine in COLUMN 0 (measured:
                // clean frames niRz = (-sin(bodyX), cos(bodyX), 0)) — a
                // different column than root local. Mix 0 and 1 here.
                auto&       RW = a_this->world.rotate;
                const float c = std::cos(sSSFramePhi), s = std::sin(sSSFramePhi);
                for (int r = 0; r < 3; ++r) {
                    const float c0 = RW.entry[r][0];
                    const float c1 = RW.entry[r][1];
                    RW.entry[r][0] = c0 * c - c1 * s;
                    RW.entry[r][1] = c0 * s + c1 * c;
                }
                using fnW2S = void(*)(RE::NiCamera*);
                static REL::Relocation<fnW2S> sUpdW2S2{ REL::RelocationID(69271, 70641) };
                sUpdW2S2(a_this);
            }
        }

        // Apply the menu's resolution-aware UI scale here so it lands
        // BEFORE the user first opens the SKSE Menu Framework picker
        // â€” eliminating the launch-time snap from MF's default style
        // to our scaled style each game session. Cached internally;
        // every call after first apply (and resolution-unchanged) is
        // a single float compare + early return.
        MenuUI::TickUiScale();

        // [TWEENCAM] SAMPLES FROM HERE, NOT ONLY FROM UpdateCameraPost.
        //
        // The probe's whole subject is what the camera does across
        // Tween -> Inventory / Magic, and UpdateCameraPost DOES NOT TICK while
        // those two menus are open (measured 2026-08-22). So the probe was
        // structurally blind to its own subject: it printed `menu=Tween` then
        // `menu=-`, never once `menu=Inventory`, and I read that absence as
        // "the user never opened it" â€” twice. They had. The 3D still renders
        // during Inventory (you can see your character), so NiCamera::
        // UpdateWorldData keeps firing there and CAN see it.
        //
        // Guarded to one sample per rendered frame: this hook runs for EVERY
        // NiCamera, and only the player camera's own child is the one we mean.
        if (a_this) {
            if (auto* pcT = RE::PlayerCamera::GetSingleton(); pcT && pcT->cameraRoot) {
                if (auto* rootT = pcT->cameraRoot->AsNode();
                    rootT && !rootT->GetChildren().empty() &&
                    rootT->GetChildren()[0].get() == static_cast<RE::NiAVObject*>(a_this))
                {
                    TweenCamTick();
                }
            }
        }

        // ---- LATE FOV HOLD -------------------------------------------------
        //
        // TWO cases, one write site, DIFFERENT sources: a menu is open, or
        // the bow zoom is held. Both are situations where something else
        // writes worldFOV AFTER CameraController::ApplyFOV has, so the
        // profile loses.
        //
        // The MENU case asserts s_menuFovFreeze — the rendered value latched
        // the frame before the window opened — because an unconfigured menu
        // behaves like a pause (user ruling 2026-08-31) and a resolver value
        // can belong to a state the menu wasn't opened from.
        //
        // The BOW-ZOOM case keeps GetLastAppliedWorldFov: ApplyFOV runs from
        // HookedThirdPersonUpdate â€” i.e. inside ThirdPersonState::Update —
        // and the engine's Eagle Eye zoom is applied later in the frame, so
        // it overwrote us every time and the vanilla zoom was what the player
        // saw ("when both drawing and zoomed are the same, it currently
        // seems to go back to the vanilla way" â€” 2026-08-23). Re-asserting
        // HERE, the last camera pass before the renderer, is what makes the
        // Zoomed profile actually replace the perk's zoom rather than merely
        // coexist with it; the last-applied value is TRANSITION-EASED, so
        // holding it keeps the ease instead of snapping to the target.
        // ---------------------------------------------------------------------
        //
        // MEASURED 2026-08-23. On a dragon, opening Inventory or Magic from the
        // Tween menu drops worldFOV to vanilla 90:
        //     camState=7  menu=Tween     spim=false  fov=100.00
        //     camState=12 menu=Inventory spim=false  fov= 90.00   <- the "zoom"
        // On FOOT the same sequence holds at 110 because Show Player In Menus is
        // active there (`spim=true`) and pins worldFOV itself. Mounted, SPIM
        // stands down, so nothing holds it â€” which is exactly why the user only
        // ever saw this while dragon riding.
        //
        // The existing kTween hold cannot catch it: Tween -> Inventory puts a
        // ridden dragon back into kDragon (12), not kTween (7). And it has to be
        // asserted from HERE rather than UpdateCameraPost, because that hook
        // does not tick while those two menus are open â€” the same blindness that
        // hid this for three rounds.
        //
        // FOV only. Position during a menu belongs to the engine; worldFOV is a
        // plain field on PlayerCamera, independent of which state drives
        // position, so holding it cannot snap anything.
        if (a_this) {
            auto* uiFov = RE::UI::GetSingleton();
            auto* pcFov = RE::PlayerCamera::GetSingleton();
            const float niIn = pcFov ? pcFov->worldFOV : -1.0f;
            const bool spimActive =
                ShowPlayerInMenusController::GetSingleton().IsActive();
            const bool menuFov = uiFov &&
                (uiFov->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
                 uiFov->IsMenuOpen(RE::MagicMenu::MENU_NAME)     ||
                 uiFov->IsMenuOpen(RE::TweenMenu::MENU_NAME));
            // Keep the freeze latched to the RENDERED value on every frame no
            // menu window is open (and SPIM isn't writing its own FOV), so
            // the holds below freeze exactly what was on screen when the
            // window opened — 1p, 3p, mounted alike. kTween frames are
            // excluded even when IsMenuOpen hasn't flipped yet: the engine
            // can park the camera (and write its menu FOV) a beat before the
            // menu registers open, and latching that frame would freeze the
            // engine's value instead of the player's.
            const bool tweenState = pcFov && pcFov->currentState &&
                pcFov->currentState->id == RE::CameraState::kTween;
            if (!menuFov && !tweenState && !spimActive && pcFov && pcFov->worldFOV > 1.0f)
                s_menuFovFreeze = pcFov->worldFOV;
            // The freeze only holds when the menu is a real PAUSE (or the
            // camera is parked in kTween): with an unpaused menu the live
            // state keeps ticking and its own applier (3p ApplyFOV / the 1p
            // writer) owns the FOV — holding the open-moment value against a
            // live pipeline is a fight, not a freeze.
            const bool pausedOrParked = tweenState ||
                (uiFov && uiFov->GameIsPaused());
            // The hold window is the MENU window plus the whole kTween state
            // plus a short tail after close. Gating on menuFov alone left
            // exactly one frame uncovered — menu just closed, state still
            // kTween, vanilla exit ramp mid-ease — and that frame rendered
            // wfov=99.48 against a held 80 ([TWEENSTUT] 2026-09-01): the
            // 1p "jerky" close. The tail covers the ramp's ease-down in
            // whichever mode it still runs.
            static std::chrono::steady_clock::time_point sMenuFovCloseTp{};
            static bool sMenuFovWasWindow = false;
            const bool windowNow = menuFov || tweenState;
            if (sMenuFovWasWindow && !windowNow)
                sMenuFovCloseTp = std::chrono::steady_clock::now();
            sMenuFovWasWindow = windowNow;
            const bool closeTail = !windowNow &&
                sMenuFovCloseTp.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - sMenuFovCloseTp).count() < 0.6f;
            if ((windowNow || closeTail) && pcFov && !spimActive &&
                !SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
                // DDC owns the menu-window FOV outright (user ruling
                // 2026-08-31: "we completely control its camera"). This is
                // the last write before the frustum build consumes the field
                // — proven by the [TWEENFOV] frustum column — so it wins
                // against menu mods (Tween Menu Overhaul) that rewrite
                // worldFOV after every earlier hook.
                //
                // Paused/parked window: the open-moment freeze (a pause
                // shows what was on screen). Unpaused window: the LIVE
                // pipeline's value — 1p republishes its own write, the 3p
                // family publishes last-applied — so the menu tracks
                // gameplay exactly and a menu mod's ramp never renders.
                float ownFov = -1.0f;
                if (pausedOrParked) {
                    ownFov = s_menuFovFreeze;
                } else if (pcFov->currentState) {
                    const auto csId = pcFov->currentState->id;
                    if (csId == RE::CameraState::kFirstPerson) {
                        ownFov = s_fpLastWrittenFov;
                    } else if (csId == RE::CameraState::kThirdPerson ||
                               csId == RE::CameraState::kMount ||
                               csId == RE::CameraState::kDragon) {
                        bool liveOk = false;
                        const float f = CameraController::GetLastAppliedWorldFov(liveOk);
                        if (liveOk) ownFov = f;
                    }
                }
                if (ownFov > 1.0f && pcFov->worldFOV != ownFov)
                    pcFov->worldFOV = ownFov;
            } else if (pcFov && !spimActive &&
                       !SettingsManager::GetSingleton().diagnosticSuspendOverrides) {
                // Bow-zoom (Eagle Eye) hold, unchanged: BOTH flags,
                // deliberately. The engine's covers the frames it is actively
                // writing its own zoom; the resolver's additionally covers
                // the ATTACK-LAG window after release (where the engine has
                // let go but Projectile Lag is still holding the shot
                // framing) and the menu hold (where the user is tuning the
                // zoom row live). The engine writes no zoom during either, so
                // asserting our value there costs nothing.
                const bool zoomFov = pcFov->bowZoomedIn ||
                                     StateResolver::GetSingleton().IsBowZoomed();
                if (zoomFov) {
                    bool fovOk = false;
                    const float heldFov = CameraController::GetLastAppliedWorldFov(fovOk);
                    if (fovOk && heldFov > 1.0f && pcFov->worldFOV != heldFov) {
                        pcFov->worldFOV = heldFov;
                    }
                }
            }

            // [TWEENFOV] one consolidated line per rendered frame while the
            // tween window is up: where in the frame the ramp value appears
            // decides which gap the writer lives in. statePre/statePost
            // bracket TweenMenuCameraState::Update; ucpIn/ucpOut bracket the
            // UpdateCameraPost trampoline and our kTween hold; niIn is this
            // pass's entry (before the menu hold above); frustum is what the
            // renderer actually has for this camera.
            bool isPlayerNiCam = false;
            if (pcFov && pcFov->cameraRoot) {
                if (auto* rootTf = pcFov->cameraRoot->AsNode();
                    rootTf && !rootTf->GetChildren().empty() &&
                    rootTf->GetChildren()[0].get() == static_cast<RE::NiAVObject*>(a_this))
                    isPlayerNiCam = true;
            }

            // ----- PARKED-TWEEN POSE HOLD (FIRST PERSON ONLY) ---------------
            //
            // First person parks in kTween like every other state now (see the
            // note in HookedTweenMenuUpdate — refusing to park is what caused
            // the frame-rate ping-pong with the world running). Parking means
            // FirstPersonState::Update stops running, so nothing keeps the
            // camera on the head: the tween state writes its own pose and the
            // view leaves the player. That was the original objection to
            // parking, and this is the answer to it — hold the LAST FIRST
            // PERSON POSE across the whole window, advanced by the player's
            // own movement, so the parked passes render exactly what first
            // person would have.
            //
            //   position: last 1p node position + every frame's player delta
            //             (rigid follow: zero when they stand still, so a
            //             paused window is frozen to the bit; 1:1 when the
            //             world is running, so an unpaused one tracks them)
            //   rotation: the last 1p matrix, verbatim — the vanilla kTween
            //             view-angle ease has no business turning the view
            //             while a menu is up
            //
            // The follow ANCHOR travels with it: s_fpSpringLastPos is a world
            // position, so without this the trail restored at the close would
            // be measured against wherever the player was when the menu
            // opened. Advancing it by the same delta keeps the trail VECTOR
            // intact, which is what the close hand-back needs.
            //
            // Third person is excluded throughout: its parked wheel view is
            // the framing the user approved.
            {
                static RE::NiPoint3  sFpPosePos{};
                static RE::NiMatrix3 sFpPoseRot{};
                static bool          sFpPoseValid = false;

                const int csHold = (pcFov && pcFov->currentState)
                    ? static_cast<int>(pcFov->currentState->id) : -1;
                const bool holdPose = isPlayerNiCam && pcFov && sFpPoseValid &&
                    csHold == static_cast<int>(RE::CameraState::kTween) &&
                    WasLastFrameFirstPerson() && !spimActive &&
                    !SettingsManager::GetSingleton().diagnosticSuspendOverrides;

                RE::NiPoint3 plyHold{};
                bool         havePlyHold = false;
                if (isPlayerNiCam) {
                    if (auto* plyH = RE::PlayerCharacter::GetSingleton()) {
                        plyHold     = plyH->GetPosition();
                        havePlyHold = true;
                    }
                }

                if (holdPose) {
                    if (s_tweenCarryPrimed && havePlyHold) {
                        const RE::NiPoint3 d{ plyHold.x - s_tweenCarryPrevPly.x,
                                              plyHold.y - s_tweenCarryPrevPly.y,
                                              plyHold.z - s_tweenCarryPrevPly.z };
                        const float dm = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                        // A load door / fast travel moves hundreds of units in
                        // one frame. That is not walking and must not be carried.
                        if (dm > 0.0001f && dm < 60.0f) {
                            sFpPosePos.x += d.x; sFpPosePos.y += d.y; sFpPosePos.z += d.z;
                            if (s_fpSpringSaved) {
                                s_fpSpringLastPos.x += d.x;
                                s_fpSpringLastPos.y += d.y;
                                s_fpSpringLastPos.z += d.z;
                            }
                        }
                    }
                    a_this->world.translate = sFpPosePos;
                    a_this->world.rotate    = sFpPoseRot;
                    if (auto* rootH = pcFov->cameraRoot.get()) {
                        rootH->world.translate = sFpPosePos;
                        rootH->local.translate = sFpPosePos;
                        rootH->world.rotate    = sFpPoseRot;
                    }
                } else if (isPlayerNiCam &&
                           csHold == static_cast<int>(RE::CameraState::kFirstPerson)) {
                    // Live first person: this pass IS the pose to hold if the
                    // camera parks. (A render-level duplicate-pass hold lived
                    // here for one build, keyed on the Main::Update frame
                    // counter. It caught most passes but not all — the frame
                    // boundary sometimes falls BETWEEN the two camera passes,
                    // which makes the second one look like the first of a new
                    // frame: distRoot held 17.1 with a 9.2 leaking through
                    // every few frames, 20:01:27.747 / .834 / .861. The double
                    // pump is now suppressed at its source instead, in
                    // HookedFirstPersonUpdate, where the test is exact.)
                    sFpPosePos   = a_this->world.translate;
                    sFpPoseRot   = a_this->world.rotate;
                    sFpPoseValid = true;
                }

                // Latched every frame this hook runs, so the delta above is
                // always exactly one pass old.
                if (isPlayerNiCam && havePlyHold) {
                    s_tweenCarryPrevPly = plyHold;
                    s_tweenCarryPrimed  = true;
                }
                // THE CLOCK, latched on the same schedule and from the same
                // hook — every rendered pass, whatever camera state is current.
                // The follow spring chases this node, not the player's data
                // position, and inside an unpaused window the two advance on
                // opposite passes (see s_tweenCarryPrevCam1).
                if (isPlayerNiCam) {
                    if (auto* plyC1 = RE::PlayerCharacter::GetSingleton()) {
                        if (auto* c1 = FirstPersonCamNode(plyC1)) {
                            s_tweenCarryPrevCam1   = c1->world.translate;
                            s_tweenCarryCam1Primed = true;
                        }
                    }
                }
                // Menu-window witness for the close grace. Stamped HERE
                // because this hook runs on every rendered pass whatever
                // camera state is current — a window that parks the camera in
                // kTween is invisible to HookedFirstPersonUpdate, which is
                // exactly where the grace has to be armed from.
                if (isPlayerNiCam && menuFov && uiFov && !uiFov->GameIsPaused())
                    s_menuWindowSeenTp = std::chrono::steady_clock::now();
            }

            // [TWEENSTUT] — once per rendered frame: frame dt + planar player
            // speed, pre-rolled 10 frames and printed live for 24 around each
            // Tween OPEN edge. Adjudicates the walking stutter: dt spike =
            // frame hitch, speed dip at steady dt = input drop, both steady =
            // camera-side artifact.
            if (isPlayerNiCam && pcFov) {
                const auto nowTs = std::chrono::steady_clock::now();
                float dtMs = 0.0f;
                if (s_tsPrevTp.time_since_epoch().count() != 0)
                    dtMs = std::chrono::duration<float, std::milli>(nowTs - s_tsPrevTp).count();
                s_tsPrevTp = nowTs;
                float spd = 0.0f;
                if (auto* plyTs = RE::PlayerCharacter::GetSingleton()) {
                    const auto pTs = plyTs->GetPosition();
                    const float dx = pTs.x - s_tsPrevPos.x;
                    const float dy = pTs.y - s_tsPrevPos.y;
                    if (dtMs > 0.01f)
                        spd = std::sqrt(dx * dx + dy * dy) / (dtMs / 1000.0f);
                    s_tsPrevPos = pTs;
                }
                const int csTs = pcFov->currentState
                    ? static_cast<int>(pcFov->currentState->id) : -1;
                // Per-frame CAMERA-NODE displacement — the jerk metric. A
                // smooth pan reads as near-constant camDp; a 1-frame jerk is
                // a single spiked row ("1st feels jerky", 2026-09-01).
                static RE::NiPoint3 sTsPrevCam{};
                const RE::NiPoint3 camW = a_this->world.translate;
                const float camDp = std::sqrt(
                    (camW.x - sTsPrevCam.x) * (camW.x - sTsPrevCam.x) +
                    (camW.y - sTsPrevCam.y) * (camW.y - sTsPrevCam.y) +
                    (camW.z - sTsPrevCam.z) * (camW.z - sTsPrevCam.z));
                sTsPrevCam = camW;
                const bool tweenOpenTs = uiFov &&
                    uiFov->IsMenuOpen(RE::TweenMenu::MENU_NAME);
                // Arm on CLOSE edges too — the exit is where the flicker
                // lives now that the open is quiet.
                if (tweenOpenTs != s_tsPrevOpen && s_tsWindows < 12) {
                    ++s_tsWindows;
                    for (int i = 0; i < 10; ++i) {
                        const auto& f = s_tsRing[(s_tsRingN + i) % 10];
                        spdlog::debug("[TWEENSTUT] pre  dt={:5.1f}ms spd={:4.0f} cam={} camDp={:7.2f}",
                                     f.dtMs, f.spd, f.cs, f.camDp);
                    }
                    s_tsLive = 24;
                }
                s_tsPrevOpen = tweenOpenTs;
                if (s_tsLive > 0) {
                    --s_tsLive;
                    // tm = the ENGINE's global time multiplier, paused = the
                    // world state. The unpaused-only skip has exactly two
                    // possible shapes and this pair separates them: a tm dip
                    // (or a fight between the engine's pause ramp and our pin
                    // below) moves the PLAYER in steps, while a clean tm=1.00
                    // with a spiked camDp is camera-side.
                    float tmTs = -1.0f;
                    if (auto* tmrTs = RE::BSTimer::GetSingleton())
                        tmTs = tmrTs->QGlobalTimeMultiplier();
                    spdlog::debug("[TWEENSTUT] live dt={:5.1f}ms spd={:4.0f} cam={} menu={} camDp={:7.2f} "
                                 "wfov={:.2f} hfov={:.2f} tm={:.3f} paused={}",
                                 dtMs, spd, csTs, tweenOpenTs ? 1 : 0, camDp,
                                 pcFov->worldFOV, pcFov->firstPersonFOV, tmTs,
                                 (uiFov && uiFov->GameIsPaused()) ? 1 : 0);
                } else {
                    s_tsRing[s_tsRingN % 10] = { dtMs, spd, csTs, camDp };
                    ++s_tsRingN;
                }
            }

            // Tween opening slow-mo counter (the ENGINE's — see the
            // s_tweenKeyTp note; unconfigured menus behave like a pause,
            // and a pause has no time dilation drifting the player under a
            // parked camera). Pinned from the keypress through the open
            // window — the dilation starts BEFORE the menu registers, so
            // the keypress stamp is the only usable arm. The death-cam
            // slow-mo is DDC's own dilation and is excluded.
            if (isPlayerNiCam && !spimActive && !s_deathSlowmoActive) {
                const bool keyWindow = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - s_tweenKeyTp).count() < 1.5f;
                const bool tweenUp = uiFov &&
                    uiFov->IsMenuOpen(RE::TweenMenu::MENU_NAME);
                if (keyWindow || tweenUp) {
                    if (auto* tmr = RE::BSTimer::GetSingleton();
                        tmr && tmr->QGlobalTimeMultiplier() != 1.0f) {
                        tmr->SetGlobalTimeMultiplier(1.0f, true);
                    }
                }
            }

            const bool tfWindow = menuFov ||
                (pcFov && pcFov->currentState &&
                 pcFov->currentState->id == RE::CameraState::kTween);
            if (tfWindow && s_tfLines < 240 && isPlayerNiCam) {
                {
                    float frustumDeg = 0.0f;
                    const auto& fr = a_this->viewFrustum;
                    if (fr.fNear > 0.0001f)
                        frustumDeg = (std::atan(-fr.fLeft / fr.fNear) +
                                      std::atan(fr.fRight / fr.fNear)) * 57.29578f;
                    ++s_tfLines;
                    // unk158/unk15C: PlayerCamera's two unmapped dwords, read
                    // as floats — the ramp writer keeps internal state and
                    // these are the only unidentified per-camera storage. If
                    // one of them carries the ramp (or its 0->10 offset),
                    // the writer is named and the fix is zeroing the source.
                    float u158 = 0.0f, u15C = 0.0f;
                    std::memcpy(&u158, &pcFov->unk158, sizeof(float));
                    std::memcpy(&u15C, &pcFov->unk15C, sizeof(float));
                    spdlog::debug("[TWEENFOV] statePre={:.2f} statePost={:.2f} ucpIn={:.2f} "
                                 "ucpOut={:.2f} niIn={:.2f} now={:.2f} frustum={:.2f} freeze={:.2f} "
                                 "u158={:.3f} u15C={:.3f}",
                                 s_tfStatePre, s_tfStatePost, s_tfUcpIn, s_tfUcpOut,
                                 niIn, pcFov->worldFOV, frustumDeg, s_menuFovFreeze,
                                 u158, u15C);
                    s_tfStatePre = s_tfStatePost = s_tfUcpIn = s_tfUcpOut = -1.0f;
                }
            }
        }

        // Dialogue stutter diagnostic: truly-last pre-render capture.
        // This hook fires AFTER NiCamera::UpdateWorldData rebuilds the
        // world matrix from local + parent, so a_this->world.rotate is
        // the matrix the renderer actually uses. Gate on first child of
        // cameraRoot (the conventional player NiCamera slot). No-op
        // outside the dialogue capture window; cheap when inactive.
        if (a_this) {
            if (auto* playerCamD = RE::PlayerCamera::GetSingleton();
                playerCamD && playerCamD->cameraRoot)
            {
                if (auto* asNodeD = playerCamD->cameraRoot->AsNode();
                    asNodeD && !asNodeD->GetChildren().empty())
                {
                    auto* firstChild = asNodeD->GetChildren()[0].get();
                    if (firstChild == static_cast<RE::NiAVObject*>(a_this)) {
                        CameraController::GetSingleton().CaptureDlgPerFrameLate(
                            playerCamD,
                            CameraController::DlgLateCaptureSite::kNiCameraUpdateWorldData);

                        // (dialogue-exit zoom ease moved to HookedThirdPersonUpdate,
                        // before the original Update â€” writing currentZoomOffset here,
                        // after UpdateWorldData, was too late to affect the render.)
                    }
                }
            }
        }

        // 1p camera noise â€” apply the pending rotation AFTER the
        // engine's UpdateWorldData runs (which during cast wind-up
        // re-derives world.rotate from the head bone and would
        // otherwise overwrite any noise written from
        // CameraNoiseController::OnCameraUpdate). Gate: the firing
        // NiCamera must be the player's main world NiCamera under
        // cameraRoot. Iterate children rather than checking [0] â€”
        // during animated cast wind-up the engine may insert other
        // nodes ahead of the NiCamera, so a fixed [0] check silently
        // drops the noise exactly in the states that need it most
        // (matches the iteration ApplyPerlin1p does on the writer
        // side). Sub-scene NiCameras (Barter inventory preview, etc.)
        // live in their own scene graph and won't match.
        if (a_this) {
            auto* playerCamN = RE::PlayerCamera::GetSingleton();
            const bool in1p = playerCamN && playerCamN->IsInFirstPerson();
            if (in1p && playerCamN->cameraRoot) {
                auto* asNodeN = playerCamN->cameraRoot->AsNode();
                if (asNodeN) {
                    for (auto& child : asNodeN->GetChildren()) {
                        if (auto* ni = child.get()) {
                            if (auto* nc = skyrim_cast<RE::NiCamera*>(ni)) {
                                if (a_this == nc) {
                                    auto& noise = CameraNoiseController::GetSingleton();
                                    noise.ApplyPending1pNoise(a_this);
                                    // Dialogue face-lock release tail: while the
                                    // lock is rendering it composes its own noise;
                                    // once it lets go (s_faceLockShouldRender false)
                                    // re-apply the noise here so it doesn't drop out
                                    // as the engine retakes the camera (this hook's
                                    // recompute would otherwise wipe the direct-path
                                    // write).
                                    //
                                    // The tail SLIDES: a fixed 400ms window after
                                    // release was too short â€” the engine keeps
                                    // re-deriving the camera transform (the face-lock
                                    // had been forcibly writing it), so this recompute
                                    // keeps firing and wiping the direct write well
                                    // past 400ms, and noise dropped out until the
                                    // camera finally went quiet. Re-arming the deadline
                                    // on every fire makes the tail last exactly as long
                                    // as the settling does, then expire ~400ms after
                                    // UpdateWorldData goes quiet (when the direct write
                                    // survives on its own again). Only ever armed at a
                                    // dialogue release, so normal 1p noise is untouched.
                                    const auto nowTail = std::chrono::steady_clock::now();

                                    // Release blend. The shake itself is constant
                                    // (verified), but at lock release the rendered
                                    // orientation steps ~0.1Â° from our spring to the
                                    // engine-native value in ONE frame â€” that step
                                    // momentarily overshadows the subtle shake and is
                                    // the residual "handoff". For the first few frames
                                    // after release, smear the step (smoothstep from
                                    // the spring orientation toward engine-native) so
                                    // the per-frame motion drops to shake level, AND
                                    // compose this frame's noise on top so the shake
                                    // stays continuous through the blend. After the
                                    // blend, the sliding tail re-applies noise as before.
                                    static bool          sPrevShouldRenderRB = false;
                                    static int           sReleaseBlendLeft   = 0;
                                    static RE::NiMatrix3 sReleaseBlendFrom{};
                                    constexpr int        kReleaseBlendFrames = 6;
                                    if (sPrevShouldRenderRB && !s_faceLockShouldRender) {
                                        sReleaseBlendLeft = kReleaseBlendFrames;
                                        sReleaseBlendFrom = s_faceLockRenderMatrix;
                                    }
                                    sPrevShouldRenderRB = s_faceLockShouldRender;

                                    if (!s_faceLockShouldRender &&
                                        nowTail < s_faceLockNoiseTailUntil) {
                                        if (sReleaseBlendLeft > 0 && a_this) {
                                            auto toYP = [](const RE::NiMatrix3& m, float& y, float& p) {
                                                y = std::atan2(m.entry[0][0], m.entry[1][0]);
                                                p = std::asin(std::clamp(-m.entry[2][0], -1.0f, 1.0f));
                                            };
                                            // Same basis as the face-lock writer (~line 4733).
                                            auto fromYP = [](float y, float p) {
                                                RE::NiPoint3 f{ std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), -std::sin(p) };
                                                RE::NiPoint3 r{ f.y, -f.x, 0.0f };
                                                const float rl = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
                                                if (rl > 0.001f) { r.x /= rl; r.y /= rl; r.z /= rl; }
                                                RE::NiPoint3 u{ r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
                                                RE::NiMatrix3 mm{};
                                                mm.entry[0][0] = f.x; mm.entry[0][1] = u.x; mm.entry[0][2] = r.x;
                                                mm.entry[1][0] = f.y; mm.entry[1][1] = u.y; mm.entry[1][2] = r.y;
                                                mm.entry[2][0] = f.z; mm.entry[2][1] = u.z; mm.entry[2][2] = r.z;
                                                return mm;
                                            };
                                            const float t  = 1.0f - static_cast<float>(sReleaseBlendLeft) /
                                                                    static_cast<float>(kReleaseBlendFrames);
                                            const float sm = t * t * (3.0f - 2.0f * t);  // smoothstep
                                            float fy, fp, ty, tp;
                                            toYP(sReleaseBlendFrom, fy, fp);
                                            toYP(a_this->world.rotate, ty, tp);   // engine-native target
                                            float dyaw = ty - fy;
                                            while (dyaw >  3.14159265f) dyaw -= 6.28318530f;
                                            while (dyaw < -3.14159265f) dyaw += 6.28318530f;
                                            RE::NiMatrix3 bm = fromYP(fy + dyaw * sm, fp + (tp - fp) * sm);
                                            RE::NiMatrix3 nd;
                                            if (noise.Get1pNoiseDelta(nd)) bm = bm * nd;
                                            a_this->world.rotate = bm;
                                            if (auto* fps = static_cast<RE::FirstPersonState*>(
                                                    playerCamN->cameraStates[RE::CameraState::kFirstPerson].get());
                                                fps && fps->firstPersonCameraObj) {
                                                fps->firstPersonCameraObj->world.rotate = bm;
                                            }
                                            using fn_t = void(*)(RE::NiCamera*);
                                            static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                                            updateW2S(a_this);
                                            --sReleaseBlendLeft;
                                        } else {
                                            noise.Reapply1pNoiseOnTop(a_this);
                                        }
                                        s_faceLockNoiseTailUntil =
                                            nowTail + std::chrono::milliseconds(400);
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Per-frame ShowPlayerInMenus framing â€” runs from the renderer
        // hook (not the third-person update hook) because some menus
        // (Barter, Container) suspend ThirdPersonState::Update entirely.
        //
        // We do TWO things here:
        //   1. ApplyFraming writes the source fields (freeRotation,
        //      posOffsetExpected, FOV) â€” used by menus where the
        //      engine still ticks the third-person update.
        //   2. ApplyRenderMatrix writes the NiCamera's world matrix
        //      DIRECTLY â€” used by menus where the engine has frozen
        //      the third-person pipeline so writes to source fields
        //      are dead. Same approach as the dialogue face-lock
        //      matrix override.
        if (auto& spim = ShowPlayerInMenusController::GetSingleton();
            spim.IsActive() && !spim.IsActiveCameraControlEnabled())
        {
            if (auto* playerCam = RE::PlayerCamera::GetSingleton()) {
                if (auto* tps = skyrim_cast<RE::ThirdPersonState*>(
                        playerCam->cameraStates[RE::CameraState::kThirdPerson].get())) {
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        spim.ApplyFraming(playerCam, tps, player);

                        // Direct NiCamera->world override. Apply to
                        // whichever NiCamera fires this hook during the
                        // menu â€” Container uses cameraRoot's first
                        // child, but BarterMenu uses a separate render
                        // NiCamera that ISN'T a child of cameraRoot.
                        // Diagnostic logging confirmed only one NiCamera
                        // fires UpdateWorldData per frame while a
                        // relevant menu is open, so writing
                        // unconditionally is safe.
                        // Only write when the firing NiCamera IS the
                        // player's main world NiCamera (cameraRoot's
                        // first child). Other NiCameras hitting this
                        // hook are sub-scene cameras (e.g., Barter's
                        // inventory item-preview), which live in their
                        // own coordinate space and would render junk
                        // if we wrote our world coordinates to them.
                        // The main-camera write site is in
                        // HookedUpdateCameraPost; this hook stays as
                        // a backup so the override survives any
                        // mid-frame engine recompose targeting the
                        // main NiCamera.
                        if (a_this && playerCam->cameraRoot) {
                            auto* asNode = playerCam->cameraRoot->AsNode();
                            if (asNode && !asNode->GetChildren().empty() &&
                                a_this == asNode->GetChildren()[0].get())
                            {
                                if (spim.ApplyRenderMatrix(a_this, player)) {
                                    using fn_t = void(*)(RE::NiCamera*);
                                    static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
                                    updateW2S(a_this);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!s_faceLockShouldRender || !a_this) return;

        // Suppress the dialogue face-lock matrix override while a
        // Show-Player-In-Menus override is active (e.g., Barter, which
        // runs over an active DialogueMenu). Without this gate the
        // face-lock would overwrite our menu framing every frame on
        // the renderer side, after our pre-render writes have already
        // landed.
        if (ShowPlayerInMenusController::GetSingleton().IsActive()) return;

        auto* playerCam = RE::PlayerCamera::GetSingleton();
        if (!playerCam || !playerCam->cameraRoot) return;
        auto* asNode = playerCam->cameraRoot->AsNode();
        if (!asNode || asNode->GetChildren().empty()) return;
        if (a_this != asNode->GetChildren()[0].get()) return;

        a_this->world.rotate = s_faceLockRenderMatrix;

        // Also override the first-person hands viewmodel's parent transform.
        // The viewmodel (hands holding weapon) is parented under
        // FirstPersonState::firstPersonCameraObj â€” a different scene-graph
        // branch than cameraNI. The engine's lagged pitch state (which we
        // can't easily suppress at its source) writes that node's rotation
        // and produces a "hands snap up" at the ~1s transition end with
        // weapons drawn. Copying our face-lock matrix into the same node
        // makes the hands track the camera frame-by-frame. Sheathed has
        // no observable issue, but keeping the write unconditional avoids
        // a special-case path and any latent IK quirks.
        auto* fpStateRaw = playerCam->cameraStates[RE::CameraState::kFirstPerson].get();
        if (auto* fpState = static_cast<RE::FirstPersonState*>(fpStateRaw)) {
            if (fpState->firstPersonCameraObj) {
                fpState->firstPersonCameraObj->world.rotate = s_faceLockRenderMatrix;
            }
        }
    }

    void HookManager::InstallNiCameraUpdateWorldDataHook()
    {
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_NiCamera[0] };
        _originalNiCameraUpdateWorldData = vtbl.write_vfunc(0x30, &HookedNiCameraUpdateWorldData);
        spdlog::info("HookManager: NiCamera::UpdateWorldData vtable hook installed (slot 0x30)");
    }

    // [DLGSNEAK] Verbose-only evidence line for the in-dialogue sneak relabel.
    // Logged on the press edge only. sneakingBefore is the player's state as
    // our hook sees it; PlayerControls runs AFTER MenuControls, so two
    // consecutive presses that log the same sneakingBefore mean the relabel
    // fired but SneakHandler refused the toggle (mount, swim, disabled
    // controls, ...). No line at all means the press never matched the
    // Gameplay-context Sneak mapping for that device.
    static void NoteDialogueSneakRelabel(RE::InputEvent* a_event, RE::INPUT_DEVICE a_device, std::uint32_t a_code)
    {
        const auto* btn = a_event ? a_event->AsButtonEvent() : nullptr;
        if (!btn || !btn->IsDown()) return;
        const auto* player = RE::PlayerCharacter::GetSingleton();
        const bool sneakingBefore = player && player->IsSneaking();
        spdlog::debug("[DLGSNEAK] Sneak relabelled in dialogue: device={} idCode={} sneakingBefore={}",
                      static_cast<int>(a_device), a_code, sneakingBefore ? 1 : 0);
    }

    RE::BSEventNotifyControl HookManager::HookedMenuControlsProcessEvent(RE::MenuControls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source)
    {
        auto& settings = SettingsManager::GetSingleton();
        if (!a_event || !*a_event || a_this->remapMode) {
            return _originalMenuControlsProcessEvent(a_this, a_event, a_source);
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return _originalMenuControlsProcessEvent(a_this, a_event, a_source);

        // Tween-open stamp for the tween time-dilation pin (see s_tweenKeyTp).
        for (auto* e = *a_event; e; e = e->next) {
            if (const auto* btn = e->AsButtonEvent(); btn && btn->IsDown()) {
                if (const auto* ue = RE::UserEvents::GetSingleton();
                    ue && btn->QUserEvent() == ue->tweenMenu) {
                    s_tweenKeyTp = std::chrono::steady_clock::now();
                }
            }
        }

        // Two paths trigger the WASD / left-stick relabel:
        //   1. Dialogue movement (DME-compat): user is in dialogue and the
        //      dialogueMovementEnabled toggle is on.
        //   2. Unpaused menu with allowMovement on: Tween / Inventory /
        //      Magic / Container / Barter that the user has unpaused and
        //      explicitly allowed movement for.
        const bool dialogueRelabel = settings.dialogueMovementEnabled &&
                                     ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        const bool unpausedMoveRelabel =
            UnpauseManager::IsAnyUnpausedMenuAllowingMovement();
        const bool unpausedCameraRelabel =
            UnpauseManager::IsAnyUnpausedMenuAllowingCameraControl();
        // We also need to fire the hook when an unpaused menu is open
        // WITHOUT allowCameraControl, so we can consume the gamepad
        // "rotate" event before it reaches the menu's player-avatar
        // rotation handler. Otherwise right-stick in Tween / Inventory /
        // Magic / Container spins the player model.
        const bool unpausedMenuOpen =
            UnpauseManager::GetUnpausedMenuCount() > 0;
        // Barter rides the dialogue camera (no free-look to orbit) and the
        // player is locked in conversation, so the right-stick "Rotate" event
        // there is purely the 3D item-inspection rotation â€” it must NOT be
        // consumed below or item rotation breaks while bartering unpaused.
        const bool barterOpen = ui->IsMenuOpen("BarterMenu");
        if (!dialogueRelabel && !unpausedMoveRelabel &&
            !unpausedCameraRelabel && !unpausedMenuOpen)
        {
            return _originalMenuControlsProcessEvent(a_this, a_event, a_source);
        }

        auto* controlMap  = RE::ControlMap::GetSingleton();
        auto* userEvents  = RE::UserEvents::GetSingleton();
        if (!controlMap || !userEvents) {
            return _originalMenuControlsProcessEvent(a_this, a_event, a_source);
        }

        // [MENUBTN] What is the right-stick CLICK actually called in an item
        // menu? "Inspect an item with R3" has now been chased twice from
        // reasoning: once by letting TogglePOVHandler through (which handed the
        // press to True Directional Movement and locked onto whatever was
        // outside the menu), once by leaving the Rotate label alone. Whether
        // the zoom rides a PlayerInputHandler or the menu's own handler is
        // answerable from a log and not from the outside, so log it: every
        // distinct BUTTON user-event seen while an item-preview menu is up,
        // once each, capped. One session's log names the event and the fix
        // becomes a one-liner instead of a third guess.
        if (UnpauseManager::IsItemPreviewMenuOpen()) {
            static std::unordered_set<std::string> sSeen;
            if (sSeen.size() < 48) {
                for (RE::InputEvent* ev = *a_event; ev; ev = ev->next) {
                    if (ev->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                    auto* be = ev->AsButtonEvent();
                    if (!be || !be->IsDown()) continue;
                    const char* ue = be->QUserEvent().c_str();
                    std::string key = std::string(ue ? ue : "(null)") + "|" +
                                      std::to_string(static_cast<int>(be->device.get())) + "|" +
                                      std::to_string(be->idCode);
                    if (sSeen.insert(key).second) {
                        spdlog::debug("[MENUBTN] item menu button: userEvent='{}' device={} idCode={}",
                                     ue ? ue : "(null)",
                                     static_cast<int>(be->device.get()), be->idCode);
                    }
                }
            }
        }

        // Walk the linked list, relabel any IDEvent whose idCode matches the
        // Gameplay-context mapping for forward/back/strafe/leftStick.
        // Matches DME's IsMappedToSameButton(...) approach.
        const auto ctx = RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
        for (RE::InputEvent* evn = *a_event; evn; evn = evn->next) {
            if (!evn->HasIDCode()) continue;
            auto* idEvent = static_cast<RE::IDEvent*>(evn);
            const auto device = idEvent->device.get();
            const auto code   = idEvent->idCode;

            auto matches = [&](const RE::BSFixedString& name) {
                return code == controlMap->GetMappedKey(name, device, ctx);
            };

            const bool wantMoveRelabel   = dialogueRelabel || unpausedMoveRelabel;
            const bool wantCameraRelabel = dialogueRelabel || unpausedCameraRelabel;

            if (device == RE::INPUT_DEVICE::kKeyboard || device == RE::INPUT_DEVICE::kMouse) {
                if (wantMoveRelabel) {
                    if      (matches(userEvents->forward))     idEvent->userEvent = userEvents->forward;
                    else if (matches(userEvents->back))        idEvent->userEvent = userEvents->back;
                    else if (matches(userEvents->strafeLeft))  idEvent->userEvent = userEvents->strafeLeft;
                    else if (matches(userEvents->strafeRight)) idEvent->userEvent = userEvents->strafeRight;
                    // Jump rides the dialogue Allow Movement toggle ONLY â€”
                    // not the unpaused-menu movement path (hopping around
                    // inside Inventory was never asked for). The DialogueMenu
                    // context has no Jump binding, so without the relabel the
                    // ControlMap drops the event before JumpHandler sees it.
                    else if (dialogueRelabel && matches(userEvents->jump))
                        idEvent->userEvent = userEvents->jump;
                    // Sneak rides the same toggle (2026-09-11): enter and
                    // leave sneak without closing the conversation. The
                    // DialogueMenu context has no Sneak binding either, so
                    // SneakHandler never sees the press unless it is
                    // relabelled here. DialogueMovementEnabler upstream does
                    // exactly this for Sneak behind its own setting, so the
                    // engine handler is known to accept it in dialogue.
                    else if (dialogueRelabel && matches(userEvents->sneak)) {
                        idEvent->userEvent = userEvents->sneak;
                        NoteDialogueSneakRelabel(evn, device, code);
                    }
                }
            } else if (device == RE::INPUT_DEVICE::kGamepad) {
                // Controller left-stick reports as a single event tagged
                // "Left Stick" in menu context; relabel to "Move" so the
                // gameplay MovementHandler consumes it.
                if (wantMoveRelabel && idEvent->userEvent == userEvents->leftStick) {
                    idEvent->userEvent = userEvents->move;
                }
                // Gamepad jump button (matched by its Gameplay-context
                // mapping â€” the menu context labels it differently or not
                // at all). Dialogue Allow Movement only, same as keyboard.
                if (dialogueRelabel && matches(userEvents->jump)) {
                    idEvent->userEvent = userEvents->jump;
                }
                // Gamepad sneak (L3 by default), same rules as keyboard:
                // dialogue Allow Movement only, matched by the Gameplay
                // mapping. The stick MOVE event carries a different idCode
                // (kLeftThumb) from the stick CLICK, so this cannot steal
                // the left-stick relabel above.
                if (dialogueRelabel && matches(userEvents->sneak)) {
                    idEvent->userEvent = userEvents->sneak;
                    NoteDialogueSneakRelabel(evn, device, code);
                }
                // Controller right-stick is labeled "Rotate" in item-
                // menu context (it rotates the inventory 3D preview /
                // Tween's player avatar). Three cases:
                //   * allowCameraControl on â†’ relabel to "Look" so the
                //     gameplay LookHandler orbits the camera.
                //   * allowCameraControl off but unpaused menu open â†’
                //     consume the event entirely (empty userEvent)
                //     so the engine doesn't spin the player model.
                //   * neither â†’ leave untouched (vanilla menu behavior).
                // Right-stick in menu context: rotate the inventory 3D
                // preview ("Rotate") or, in TweenMenu, orbit the player
                // avatar ("Look"). When allowCameraControl is on we let
                // "Look" pass through to LookHandler so the camera
                // orbits; "Rotate" gets relabeled the same way. When
                // allowCameraControl is OFF but an unpaused menu is
                // open, we consume BOTH labels so neither the camera
                // nor the player avatar moves.
                if (idEvent->userEvent == userEvents->rotate ||
                    idEvent->userEvent == userEvents->look)
                {
                    // "Rotate" while an item is being INSPECTED is the 3D item
                    // turning on its stand, not the camera. Relabelling it to
                    // "Look" handed the stick to the gameplay LookHandler, so
                    // the view swung around behind the menu and the item sat
                    // still â€” "I can't rotate items when inspecting them".
                    //
                    // The gate is the ZOOM, not the menu. Keying it on "an item
                    // menu is open" surrendered the stick for the entire time
                    // the inventory was up, so the camera could not be moved at
                    // all while browsing â€” which is the whole point of an
                    // unpaused menu. IsInventoryItemZoomed() is the same
                    // predicate LookHandler's carve-out and
                    // ShowPlayerInMenusController::ApplyFraming already use, so
                    // all three hand the stick to the item and take it back
                    // together: R3 to inspect gives it to the item, backing out
                    // of the inspection gives it back to the camera.
                    const bool itemRotate =
                        idEvent->userEvent == userEvents->rotate &&
                        UnpauseManager::IsInventoryItemZoomed();
                    if (itemRotate) {
                        // leave untouched
                    } else if (wantCameraRelabel) {
                        idEvent->userEvent = userEvents->look;
                    } else if (unpausedMenuOpen) {
                        idEvent->userEvent = "";
                    }
                }
            }
        }
        return _originalMenuControlsProcessEvent(a_this, a_event, a_source);
    }

    void HookManager::UpdateCameraCasterPatch()
    {
        if (!cameraCasterAddr) return;

        auto& settings = SettingsManager::GetSingleton();

        // Death free-look forces a full collision-off pass-through and ignores
        // the per-layer exceptions â€” the orbit must not be blocked by anything.
        const bool forceOff = s_forceCasterPatchForDeathFreeLook;
        // indoorMode is refreshed before this call, so door transitions select
        // the other environment's complete collision settings on the next frame.
        const auto& collision = settings.cameraCollision.ForEnvironment(settings.indoorMode);
        const bool disable = collision.disable || forceOff;
        const bool exceptions = !forceOff && collision.disable && collision.keep.Any();

        CasterPatchState desired =
            !disable     ? CasterPatchState::kOriginal :
            exceptions   ? CasterPatchState::kDivert
                         : CasterPatchState::kFalseStub;

        // Divert needs the abs-jump stub; if it failed to allocate, fall back to
        // full pass-through so the master toggle still does something sane.
        if (desired == CasterPatchState::kDivert && !cameraCasterDivertStub) {
            desired = CasterPatchState::kFalseStub;
        }

        if (desired == cameraCasterState) return;

        switch (desired) {
        case CasterPatchState::kOriginal:
            REL::safe_write(cameraCasterAddr, cameraCasterOrigBytes, 6);
            break;
        case CasterPatchState::kFalseStub: {
            // xor eax,eax; ret; nop; nop; nop  (6 bytes, always "no hit")
            const std::uint8_t patch[6] = { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90 };
            REL::safe_write(cameraCasterAddr, patch, 6);
            break;
        }
        case CasterPatchState::kDivert: {
            // E9 <rel32> ; nop   ->   jmp cameraCasterDivertStub
            const std::intptr_t rel =
                static_cast<std::intptr_t>(cameraCasterDivertStub) -
                static_cast<std::intptr_t>(cameraCasterAddr + 5);
            const std::int32_t rel32 = static_cast<std::int32_t>(rel);
            std::uint8_t patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
            std::memcpy(patch + 1, &rel32, sizeof(rel32));
            REL::safe_write(cameraCasterAddr, patch, 6);
            break;
        }
        }

        cameraCasterState = desired;
        spdlog::info("HookManager: CameraCaster patch -> {}",
            desired == CasterPatchState::kOriginal  ? "original" :
            desired == CasterPatchState::kFalseStub ? "false-stub (collision off)"
                                                    : "divert (contact-filtered)");
    }

    // Run the original swept-hull camera cast with contact collectors that
    // admit only the selected categories. No ray-bundle gate, whole-cast door
    // latch, or unrestricted second query: every hit decides for itself.
    bool HookManager::HookedCameraCaster(
        void* a_physics, RE::bhkWorld* a_world,
        CCVec4& a_start, CCVec4& a_end,
        std::uint32_t* a_resultInfo, RE::Character** a_hitChar,
        float a_hullSize)
    {
        if (a_hitChar) *a_hitChar = nullptr;
        const auto& settings = SettingsManager::GetSingleton();
        const auto selection = settings.cameraCollision.ForEnvironment(settings.indoorMode).keep;
        if (!selection.Any() || !a_world || !a_physics || !cameraCasterDivertStub) return false;

        CameraCollision::ScopedFilter filter(selection);
        using RawFn = bool (*)(void*, RE::bhkWorld*, CCVec4&, CCVec4&,
                               std::uint32_t*, RE::Character**, float);

        // Retain the existing verified original-entry call mechanism. Restore
        // the diversion on every C++ exit; no guessed relocated prologue.
        const auto rearm = [] {
            const auto rel = static_cast<std::intptr_t>(cameraCasterDivertStub) -
                             static_cast<std::intptr_t>(cameraCasterAddr + 5);
            const auto rel32 = static_cast<std::int32_t>(rel);
            std::uint8_t patch[6] = {0xE9, 0, 0, 0, 0, 0x90};
            std::memcpy(patch + 1, &rel32, sizeof(rel32));
            REL::safe_write(cameraCasterAddr, patch, 6);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(cameraCasterAddr), 6);
        };
        struct RestoreEntry { const decltype(rearm)& run; ~RestoreEntry() { run(); } } restore{rearm};
        REL::safe_write(cameraCasterAddr, cameraCasterOrigBytes, 6);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(cameraCasterAddr), 6);
        const bool hit = reinterpret_cast<RawFn>(cameraCasterAddr)(
            a_physics, a_world, a_start, a_end, a_resultInfo, a_hitChar, a_hullSize);

        static bool loggedCoverage = false, loggedMissing = false;
        if (filter.Queries() && !loggedCoverage) {
            loggedCoverage = true;
            spdlog::info("[COLLISION] camera swept-hull collector filtering active");
        } else if (hit && !filter.Queries() && !loggedMissing) {
            loggedMissing = true;
            spdlog::warn("[COLLISION] native hit bypassed the world/phantom collectors; retaining native result");
        }
        return hit;
    }

}
