#include "PCH.h"
#include "Input/DialogueInputSink.h"
#include "Camera/VanityCamera.h"

#include "Dialogue/DialogueLookPicker.h"
#include "Hooks/HookManager.h"
#include "Settings/PresetManager.h"
#include "Settings/SettingsManager.h"
#include "UI/MenuUI.h"

#include <RE/A/Actor.h>
#include <RE/A/ActorState.h>
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BGSSaveLoadManager.h>
#include <RE/B/ButtonEvent.h>
#include <RE/C/ControlMap.h>
#include <RE/M/Misc.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/U/UI.h>
#include <RE/U/UserEvents.h>

namespace DietDrCamera
{
    namespace
    {
        // Ready Weapon pressed during dialogue — applied at the CLOSE edge
        // (see the deferral note at the press site). File-scope so the
        // dialogue-close hook can consume it.
        bool sPendingSheatheToggle = false;
    }

    void DialogueInputSink::ApplyPendingSheathe()
    {
        if (!sPendingSheatheToggle) return;
        sPendingSheatheToggle = false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        bool drawn = false;
        if (auto* aState = player->AsActorState()) drawn = aState->IsWeaponDrawn();
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([player, drawn]() { player->DrawWeaponMagicHands(!drawn); });
        }
        spdlog::info("DialogueInputSink: deferred sheathe applied at dialogue close "
                     "(was drawn={})", drawn);
    }

    DialogueInputSink& DialogueInputSink::GetSingleton()
    {
        static DialogueInputSink instance;
        return instance;
    }

    void DialogueInputSink::Install()
    {
        auto* idm = RE::BSInputDeviceManager::GetSingleton();
        if (!idm) {
            spdlog::warn("DialogueInputSink: BSInputDeviceManager not available, cycle hotkeys disabled");
            return;
        }
        idm->AddEventSink(&GetSingleton());
        spdlog::info("DialogueInputSink: registered for InputEvent");
    }

    void DialogueInputSink::BeginCapture(std::uint32_t* target)
    {
        _captureTarget.store(target);
    }

    RE::BSEventNotifyControl DialogueInputSink::ProcessEvent(
        RE::InputEvent* const* a_event,
        RE::BSTEventSource<RE::InputEvent*>* /*a_source*/)
    {
        if (!a_event || !*a_event) return RE::BSEventNotifyControl::kContinue;
        VanityCamera::OnInput(*a_event);

        for (RE::InputEvent* ev = *a_event; ev; ev = ev->next) {
            if (!ev) break;
            if (ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            auto* btn = ev->AsButtonEvent();
            if (!btn) continue;
            // Only fire on keydown (or held first frame). IsDown returns
            // true while pressed; IsPressed is the keydown edge.
            if (!btn->IsDown()) continue;

            const std::uint32_t rawCode = btn->GetIDCode();
            if (rawCode == 0) continue;

            // Encode device into the stored code. XInput button masks
            // (LB=0x100, RB=0x200, etc.) overlap keyboard/mouse codes, so
            // gamepad bindings get the high tag to disambiguate.
            const bool isGamepad = (btn->device.get() == RE::INPUT_DEVICE::kGamepad);
            const std::uint32_t encoded = isGamepad ? (rawCode | kInputGamepadTag) : rawCode;

            // Capture mode: bind this key to the configured target field.
            // Writes through the captured pointer, then clears capture.
            if (auto* target = _captureTarget.exchange(nullptr)) {
                *target = encoded;
                spdlog::info("DialogueInputSink: captured code 0x{:X} (gamepad={}) into binding", encoded, isGamepad);
                continue;
            }

            const auto& s = SettingsManager::GetSingleton();

            // Categories shoulder swap — fires anywhere in-game (not gated
            // on a menu) so the user can flip POV side mid-combat.
            if (s.categoriesShoulderSwapKey != 0 && encoded == s.categoriesShoulderSwapKey) {
                SettingsManager::GetSingleton().SwapCategoriesShoulders();
                continue;
            }

            // Quick Tune overlay — sink only OPENS. Close is handled by
            // the render-callback poll because the framework's
            // BlockUserInput=true stops the sink from receiving events
            // while the overlay is up. Splitting open/close between
            // sources prevents same-frame race spam.
            if (s.quickTuneHotkey != 0 && encoded == s.quickTuneHotkey) {
                MenuUI::OpenQuickTune();
                continue;
            }

            // Death camera skip — fires only while BleedoutCameraState
            // is the active camera state (player is dying / bleeding
            // out) AND no menu is open. Triggers immediate reload of
            // the most recent save so the user skips the rest of the
            // Hold Duration wait. The menu gate prevents the hotkey
            // from firing if the user opens ESC / inventory / etc.
            // during bleedout — without it the menu can't be used as
            // an "abort" since the key still triggers underneath.
            // Bleedout hotkey. The SAME physical key can be bound to both the
            // death-cam skip and the ragdoll slow-mo fade; we dispatch by context
            // (the engine routes both real death and ragdoll through kBleedout):
            //   real death + skip key   -> reload the most recent save
            //   ragdoll    + fade key   -> ease the ragdoll slow-mo out early
            const bool isDeathSkipKey   = (s.deathCameraSkipKey != 0 && encoded == s.deathCameraSkipKey);
            const bool isDeathFadeKey   = (s.deathCamFadeKey    != 0 && encoded == s.deathCamFadeKey);
            const bool isRagdollFadeKey = (s.ragdollCamFadeKey  != 0 && encoded == s.ragdollCamFadeKey);
            if (isDeathSkipKey || isDeathFadeKey || isRagdollFadeKey) {
                auto* uiMenu = RE::UI::GetSingleton();
                if (uiMenu && uiMenu->GameIsPaused()) continue;
                auto* pc = RE::PlayerCamera::GetSingleton();
                const bool inBleedout = pc && pc->currentState &&
                                        pc->currentState->id == RE::CameraState::kBleedout;
                if (inBleedout) {
                    bool realDeath = true;
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        realDeath = player->IsDead();
                        if (!realDeath)
                            if (auto* avo = player->AsActorValueOwner())
                                realDeath = avo->GetActorValue(RE::ActorValue::kHealth) <= 0.0f;
                    }
                    if (realDeath && isDeathSkipKey) {
                        if (auto* slm = RE::BGSSaveLoadManager::GetSingleton()) {
                            // LoadMostRecentSaveGame needs the main thread; SKSE's
                            // task interface marshals it across (calling it from the
                            // input thread can race the save-load worker).
                            if (auto* tasks = SKSE::GetTaskInterface()) {
                                tasks->AddTask([slm]() { slm->LoadMostRecentSaveGame(); });
                            } else {
                                slm->LoadMostRecentSaveGame();
                            }
                            spdlog::info("DeathCameraSkip: triggered LoadMostRecentSaveGame");
                        }
                    } else if (realDeath && isDeathFadeKey) {
                        // Same shared slow-mo fade driver the ragdoll uses —
                        // eases the death slow motion back to normal early
                        // (user request 2026-08-15).
                        HookManager::RequestRagdollSlowmoFade();
                        spdlog::info("DeathCam: slow-mo fade requested");
                    } else if (!realDeath && isRagdollFadeKey) {
                        HookManager::RequestRagdollSlowmoFade();
                        spdlog::info("RagdollCam: slow-mo fade requested");
                    }
                }
                continue;
            }

            // Preset cycle — fires anywhere in-game. Cycles to the next
            // saved preset (wraps around) and posts a corner notification.
            if (s.presetCycleNextKey != 0 && encoded == s.presetCycleNextKey) {
                auto& pm = PresetManager::GetSingleton();
                auto names = pm.ListPresets();
                if (!names.empty()) {
                    int curIdx = -1;
                    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                        if (names[i] == s.activePresetName) { curIdx = i; break; }
                    }
                    const int nextIdx = (curIdx + 1) % static_cast<int>(names.size());
                    const auto& nextName = names[nextIdx];
                    if (pm.LoadPreset(nextName)) {
                        // DebugNotification needs the main game thread to
                        // reach the HUD. InputEventSink runs on the input
                        // thread, so the call drops silently. SKSE's task
                        // interface marshals onto the main thread.
                        const std::string msg = "Preset: " + nextName;
                        if (auto* tasks = SKSE::GetTaskInterface()) {
                            tasks->AddTask([msg]() {
                                RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, false);
                            });
                        }
                    }
                }
                continue;
            }

            // Dialogue cycle — only while the Dialogue Menu is the
            // visible/active menu. Dialogue Menu STAYS in the menu stack
            // when a vendor's BarterMenu (or TrainingMenu, etc.) opens
            // over it — so `IsMenuOpen("Dialogue Menu")` alone is true
            // during barter, which let the cycle hotkey fire while the
            // user was trading. Explicitly exclude the known dialogue-
            // spawned child menus.
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->IsMenuOpen("Dialogue Menu")) continue;
            if (ui->IsMenuOpen("BarterMenu")    ||
                ui->IsMenuOpen("GiftMenu")      ||
                ui->IsMenuOpen("TrainingMenu")  ||
                ui->IsMenuOpen("ContainerMenu") ||
                ui->IsMenuOpen("Sleep/Wait Menu")) continue;

            if (s.dialogueCycleNextKey != 0 && encoded == s.dialogueCycleNextKey) {
                DialogueLookPicker::CycleActiveLook(+1);
            } else if (s.dialogueCyclePrevKey != 0 && encoded == s.dialogueCyclePrevKey) {
                DialogueLookPicker::CycleActiveLook(-1);
            }

            // Sheathe/unsheathe in dialogue. Vanilla blocks the Ready
            // Weapon action because DialogueMenu installs the kMenuMode
            // input context, which has no Ready Weapon binding — the
            // ControlMap stops routing the user event. The raw key
            // still reaches this sink (BSInputDeviceManager fires sinks
            // before the context filter), so we look up the player's
            // gameplay Ready Weapon binding for the current device and
            // dispatch the toggle directly via Actor::DrawWeaponMagicHands.
            // Marshals to the main thread via SKSE TaskInterface because
            // the call mutates animation state and must not run on the
            // input thread. Brick gate at HookManager.cpp:1852 already
            // covers weapon-drawn — IdleForceDefaultState is skipped
            // when weapon is drawn, so toggling here is safe.
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                if (auto* ue = RE::UserEvents::GetSingleton()) {
                    const std::uint32_t mapped = cm->GetMappedKey(
                        ue->readyWeapon,
                        btn->device.get(),
                        RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
                    if (mapped != RE::ControlMap::kInvalid && rawCode == mapped) {
                        // Double-duty guard. This whole block exists because
                        // vanilla routes Ready Weapon through kGameplay only,
                        // so the DialogueMenu's kMenuMode context normally has
                        // no binding for that physical key and we can safely
                        // act on the raw press. That assumption breaks the
                        // moment the SAME key is also bound in kMenuMode —
                        // then the press does two things at once: the menu
                        // consumes it (accept / next topic) AND we sheathe,
                        // which is exactly the "sheathing advances dialogue"
                        // report. We can't unsend the menu's copy (returning
                        // kStop here would kill the entire event list for the
                        // frame, thumbstick included), so we stand down
                        // instead: the menu keeps its meaning, and the key
                        // simply doesn't double as a weapon toggle.
                        const std::string_view menuEvent =
                            cm->GetUserEventName(rawCode, btn->device.get(),
                                                 RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode);
                        if (!menuEvent.empty()) {
                            static bool sLoggedDoubleDuty = false;
                            if (!sLoggedDoubleDuty) {
                                sLoggedDoubleDuty = true;
                                spdlog::info("DialogueInputSink: in-dialogue sheathe suppressed — "
                                             "Ready Weapon key 0x{:X} (device {}) is also bound to "
                                             "menu event '{}' in MenuMode, so acting on it would "
                                             "double-fire with the dialogue menu",
                                             rawCode, static_cast<int>(btn->device.get()),
                                             std::string(menuEvent));
                            }
                            continue;
                        }
                        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                            // LIVE AGAIN (2026-08-20). It was deferred to the
                            // dialogue-close edge on 2026-08-16 as the
                            // decisive experiment for "sheathing advances
                            // dialogue"; the 2026-08-20 capture ran that
                            // experiment and the presses at 17:02:57 /
                            // 17:02:58 / 17:03:14 did NOT advance or end the
                            // conversation with the toggle withheld. The user
                            // then reported the toggle itself missing, which
                            // is what the deferral cost. So: toggle live, keep
                            // the press claim below.
                            //
                            // If dialogue starts advancing again, the toggle
                            // IS the cause and the only honest options left
                            // are the deferral (this block's git history) or
                            // dropping the feature — see [[in-dialogue-sheathe]].
                            bool drawn = false;
                            if (auto* aState = player->AsActorState())
                                drawn = aState->IsWeaponDrawn();
                            if (auto* tasks = SKSE::GetTaskInterface()) {
                                tasks->AddTask([player, drawn]() {
                                    player->DrawWeaponMagicHands(!drawn);
                                });
                            }
                            spdlog::info("DialogueInputSink: in-dialogue sheathe dispatched "
                                         "(key 0x{:X}, device {}, was drawn={}, press claimed)",
                                         rawCode, static_cast<int>(btn->device.get()), drawn);
                            // Claim the press (blank the userEvent) — the same
                            // neutering the MenuControls hook uses for
                            // right-stick "Rotate". Removes the one remaining
                            // way this key could reach a later handler as a
                            // mapped event. Returning kStop instead would drop
                            // the whole frame's event list, thumbstick
                            // included, and break dialogue navigation.
                            btn->userEvent = RE::BSFixedString("");
                        }
                    }
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
}
