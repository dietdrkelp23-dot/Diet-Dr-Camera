#include "PCH.h"
#include "Core/Version.h"
#include "Core/Diagnostics.h"
#include "Camera/VanityCamera.h"
#include <Windows.h>
#include "Camera/CameraNoiseController.h"
#include "Camera/HitShakeController.h"
#include "Camera/ArcheryHitShakeController.h"
#include "Camera/MagicHitShakeController.h"
#include "Camera/DamageReactionController.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/StateResolver.h"
#include "Hooks/ArrowPathDetour.h"
#include "Hooks/MissileProjectileDetour.h"
#include "Hooks/HookManager.h"
#include "Hooks/HookTrampoline.h"
#include "Hooks/RuntimeHooks.h"
#include "Hooks/RuntimeVersion.h"
#include "Input/DialogueInputSink.h"
#include "LockOn/EnemyDetector.h"
#include "LockOn/TDMIntegration.h"
#include "Menus/ShowPlayerInMenusController.h"
#include "Settings/PresetManager.h"
#include "Settings/SettingsManager.h"
#include "Shouts/ShoutRegistry.h"
#include "UI/CrosshairManager.h"
#include "UI/MenuUI.h"
#include "Unpause/UnpauseManager.h"

#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>

#include <filesystem>
#include <system_error>

namespace
{
    // True when this launch is an NGIO / NGIO-NG grass-cache PRE-GENERATION
    // run rather than normal play. Grass generation drives the engine far
    // outside anything a camera mod is built for: it teleports the player
    // across every cell of every worldspace as fast as they load, unattended,
    // for hours. Nothing we do is wanted there — no one is watching the camera
    // — and every per-frame hook we install is pure added risk to a process
    // that has to survive the whole of Tamriel to be worth anything.
    //
    // NGIO itself decides it's a precache run by the presence of
    // PrecacheGrass.txt in the GAME ROOT (its own log line for the other case
    // reads "Grass Cache is Enabled. PrecacheGrass.txt is not detected,
    // assuming normal usage"), so keying off the same file puts us in exact
    // lockstep with it — we go dormant on precisely the runs it generates on.
    // Resolved from the executable path, not the working directory, and
    // evaluated once before any hook is installed.
    bool IsGrassPrecacheRun()
    {
        static const bool sIsPrecache = [] {
            char exePath[MAX_PATH]{};
            if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return false;
            std::error_code ec;
            const auto root = std::filesystem::path{ exePath }.parent_path();
            return std::filesystem::exists(root / "PrecacheGrass.txt", ec);
        }();
        return sIsPrecache;
    }

    class MenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuCloseSink& GetSingleton()
        {
            static MenuCloseSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event) return RE::BSEventNotifyControl::kContinue;
            if (!a_event->opening && a_event->menuName == "Journal Menu") {
                spdlog::info("Journal menu closed, saving settings");
                DietDrCamera::SettingsManager::GetSingleton().Save();
            }
            // Main menu opening is the signal that bleedout has ended
            // (BleedoutCameraState::End never fires on the death-to-main-menu
            // transition). Release the death-free-look raw-input registration
            // here so Scaleform menu input routing is restored.
            if (a_event->opening && a_event->menuName == "Main Menu") {
                spdlog::info("Main Menu opening — releasing death free-look input");
                DietDrCamera::HookManager::ReleaseDeathFreeLookInput();
            }
            // Dialogue Menu open/close drives the face-lock gate's lastSpeaker
            // fallback. Fallback is valid only while the menu is open;
            // closing the menu suppresses it so face-lock releases on
            // user exit regardless of lingering voice lines.
            if (a_event->menuName == "Dialogue Menu") {
                DietDrCamera::HookManager::OnDialogueMenuOpenChange(a_event->opening);
            }
            // Map open/close drives the map FOV guard: with the map up none
            // of the FOV pipelines tick, so a latched dialogue/profile FOV
            // would render the map zoomed (seen with Better Carriage
            // Destinations, which opens the map mid-dialogue).
            if (a_event->menuName == "MapMenu") {
                DietDrCamera::HookManager::OnMapMenuOpenChange(a_event->opening);
            }

            // Show Player In Menus — Inventory / Container / Barter /
            // Magic / Tween / Wait. Controller filters by per-menu
            // enable toggle internally; we just forward every relevant
            // menu name and let it decide. Note: Barter is intentionally
            // listed so the controller can deactivate any prior framing
            // (e.g. Tween) before dialogue camera takes over — the
            // controller's EntryFor() returns nullptr for Barter so no
            // new framing is applied.
            {
                const std::string name{ a_event->menuName };
                if (name == "InventoryMenu"   || name == "ContainerMenu" ||
                    name == "BarterMenu"      || name == "MagicMenu"     ||
                    name == "TweenMenu"       || name == "Sleep/Wait Menu" ||
                    name == "FavoritesMenu")
                {
                    DietDrCamera::ShowPlayerInMenusController::GetSingleton()
                        .OnMenuOpenChange(name, a_event->opening);
                }

                // Dialogue starting UNDER an unpaused TweenMenu (an NPC
                // force-greet reaching the player mid-menu). The two menus'
                // exit inputs collide and the engine routes the cancel to
                // the DIALOGUE — the user is stuck in the tween and the
                // exit key ends the conversation instead (user report
                // 2026-08-15). Close the tween the moment dialogue opens so
                // the conversation proceeds normally; vanilla never allows
                // this overlap in the first place.
                if (a_event->opening && name == "Dialogue Menu") {
                    if (auto* ui = RE::UI::GetSingleton();
                        ui && ui->IsMenuOpen("TweenMenu")) {
                        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                            q->AddMessage("TweenMenu",
                                          RE::UI_MESSAGE_TYPE::kHide, nullptr);
                            spdlog::info("[Unpause] Dialogue opened under TweenMenu — closing the tween");
                        }
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        MenuCloseSink() = default;
    };

    void MessageCallback(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) return;
        // Grass cache pre-generation: install nothing and hook nothing. This
        // returns before HookManager, the projectile detours, the Scaleform
        // pre-warmer, the menu, and the state resolver ever come up, so the
        // run proceeds as if the mod weren't in the load order at all.
        if (IsGrassPrecacheRun()) {
            if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
                spdlog::warn("Grass cache pre-generation detected (PrecacheGrass.txt in the game "
                             "root) — Diet Dr Camera is staying dormant for this run: no hooks, "
                             "no menu, no camera. Delete PrecacheGrass.txt to play normally.");
            }
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            DietDrCamera::Diagnostics::Checkpoint("PostLoad received; recording loaded DLLs");
            DietDrCamera::Diagnostics::LogLoadedModules();
            DietDrCamera::Diagnostics::Checkpoint("Validating instruction patch sites");
            DietDrCamera::RuntimeHooks::Prepare();
            DietDrCamera::Diagnostics::Checkpoint("Reserving hook trampoline");
            DietDrCamera::HookTrampoline::Initialize();
            DietDrCamera::Diagnostics::Checkpoint("Installing camera hooks");
            DietDrCamera::HookManager::GetSingleton().Install();
            DietDrCamera::Diagnostics::Checkpoint("Initializing TDM integration");
            DietDrCamera::TDMIntegration::GetSingleton().OnPostLoad();
            // ArrowPathDetour rewritten as a vtable patch on
            // VTABLE_ArrowProjectile only — firebolt and other
            // non-arrow projectiles never enter our hook now, so the
            // previous function-entry trampoline crash on firebolt is
            // gone for good.
            DietDrCamera::Diagnostics::Checkpoint("Installing arrow observers");
            DietDrCamera::ArrowPathDetour::Install();
            DietDrCamera::ArcheryHitShakeController::Install();
            DietDrCamera::Diagnostics::Checkpoint("Installing spell observers");
            DietDrCamera::MissileProjectileDetour::Install();
            DietDrCamera::MagicHitShakeController::Install();
            DietDrCamera::Diagnostics::Checkpoint("Installing damage reaction observers");
            DietDrCamera::DamageReactionController::Install();
            DietDrCamera::Diagnostics::Checkpoint("Initializing crosshair integration");
            DietDrCamera::CrosshairManager::GetSingleton().Init();
            DietDrCamera::Diagnostics::Checkpoint("PostLoad complete");
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            DietDrCamera::Diagnostics::Checkpoint("PostPostLoad received");
            break;
        case SKSE::MessagingInterface::kInputLoaded:
            DietDrCamera::Diagnostics::Checkpoint("InputLoaded received");
            // Input sink goes in as EARLY as the input system allows.
            // BSTEventSource notifies sinks in REGISTRATION order, and DDC has
            // to see the right-stick click before True Directional Movement's
            // own sink does (see R3ReleaseSink::ProcessEvent). Registering at
            // kDataLoaded — where this used to live — is too late: kInputLoaded
            // fires first, so any plugin registering there was already ahead of
            // us no matter what the DLLs are called.
            DietDrCamera::HookManager::InstallR3ReleaseSink();
            DietDrCamera::Diagnostics::Checkpoint("InputLoaded complete");
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            spdlog::info("Diet Dr Camera: data loaded");
            DietDrCamera::Diagnostics::Checkpoint("DataLoaded: initializing game forms");
            DietDrCamera::ShoutRegistry::GetSingleton().Init();
            DietDrCamera::EnemyDetector::GetSingleton().Init();
            DietDrCamera::Diagnostics::Checkpoint("DataLoaded: loading settings and active preset");
            DietDrCamera::SettingsManager::GetSingleton().Load();
            // Settings live in presets now — the global config holds only the
            // hotkeys + which preset was last active. Re-load that preset on
            // top so the user's saved camera comes back at launch. Hotkeys are
            // preserved across the load (they are global, not preset-scoped).
            // A missing/blank name is a no-op (LoadPreset bails before reset),
            // leaving whatever Load() applied (defaults, or a legacy full
            // config on first launch after this change — save it as a preset).
            {
                auto& s = DietDrCamera::SettingsManager::GetSingleton();
                if (!s.activePresetName.empty()) {
                    if (!DietDrCamera::PresetManager::GetSingleton().LoadPreset(s.activePresetName)) {
                        spdlog::warn("Startup preset could not be loaded; using defaults with no active preset");
                        s.activePresetName.clear();
                    }
                }
            }
            DietDrCamera::Diagnostics::Checkpoint("DataLoaded: initializing SKSE Menu Framework integration");
            DietDrCamera::MenuUI::GetSingleton().Init();
            RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&MenuCloseSink::GetSingleton());
            spdlog::info("MenuCloseSink: registered for Journal Menu close events");
            DietDrCamera::HookManager::InstallR3ReleaseSink();   // no-op if kInputLoaded already did it
            DietDrCamera::DialogueInputSink::Install();
            DietDrCamera::Diagnostics::Checkpoint("DataLoaded: installing unpaused-menu hooks");
            DietDrCamera::UnpauseManager::Install();
            // Scaleform views are created by the engine on its normal path.
            // Experimental async prewarming raced first opens and other views.
            DietDrCamera::StateResolver::GetSingleton().Register();
            DietDrCamera::Diagnostics::Checkpoint("DataLoaded complete");

            // Warn if doodlum's Camera Noise is also loaded — we both touch
            // the same cameraRoot->local and the same shake GMSTs, they'll
            // fight for dominance. User should pick one.
            if (GetModuleHandleA("skyrim-camera-noise.dll")) {
                spdlog::warn("Conflict: doodlum's Camera Noise plugin is loaded. Our Camera Noise feature and theirs both modify the camera root each frame — disable one to avoid stacking.");
            }
            break;
        case SKSE::MessagingInterface::kSaveGame:
            DietDrCamera::SettingsManager::GetSingleton().Save();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            DietDrCamera::Diagnostics::Checkpoint("PreLoadGame received; resetting camera state");
            DietDrCamera::CrosshairManager::GetSingleton().ResetTracing();
            DietDrCamera::HitShakeController::Reset();
            DietDrCamera::VanityCamera::Reset();
            DietDrCamera::CameraEffectClock::Resume();
            DietDrCamera::HookManager::ResetMenuCameraAnimation();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            DietDrCamera::Diagnostics::Checkpoint("PostLoadGame received");
            break;
        case SKSE::MessagingInterface::kNewGame:
            DietDrCamera::Diagnostics::Checkpoint("NewGame received");
            break;
        }
    }
}

SKSE_PLUGIN_VERSION = [] {
    SKSE::PluginVersionData version;
    version.PluginVersion({ DDC_VERSION_MAJOR, DDC_VERSION_MINOR, DDC_VERSION_PATCH });
    version.PluginName("Diet Dr Camera");
    version.UsesAddressLibrary();
    version.UsesNoStructs();  // NG accessors handle the runtime-dependent layouts.
    return version;        // Includes the Address Library v5 flag for Skyrim 1.7.
}();

SKSE_PLUGIN_QUERY(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info)
{
    info->infoVersion = SKSE::PluginInfo::kVersion;
    info->name = SKSEPlugin_Version.pluginName;
    info->version = SKSEPlugin_Version.pluginVersion;
    return !skse->IsEditor();
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    if (!DietDrCamera::Diagnostics::InitializeLog()) return false;
    if (!a_skse) {
        spdlog::critical("[Startup] SKSE load interface is null; initialization stopped");
        return false;
    }
    // Record the loader's version before NG initialization or any relocations
    // can fail. The exact DDC PDB identity distinguishes builds with the same version.
    DietDrCamera::Diagnostics::LogEnvironment(*a_skse);
    DietDrCamera::Diagnostics::Checkpoint("SKSE initialization begin");

    // Keep our filename, format and flush policy; NG's default logger
    // would replace it and split startup diagnostics between two different files.
    SKSE::Init(a_skse, SKSE::InitInfo{ .log = false });
    DietDrCamera::Diagnostics::Checkpoint("SKSE initialization complete");

    const auto runtime = REL::Module::get().version();
    if (!DietDrCamera::RuntimeVersion::IsKnown({runtime[0], runtime[1], runtime[2], runtime[3]})) {
        spdlog::critical("Skyrim {} has not been reviewed for engine layout compatibility; update Diet Dr Camera before using this runtime.", runtime.string());
        return false;
    }

    // Runtime classification helps identify a bug report's environment.
    // Address-library resolution alone does not validate hook offsets or
    // engine layouts on another runtime; each advertised version needs testing.
    {
        const auto v = REL::Module::get().version();
        const char* rt = REL::Module::IsVR() ? "VR"
                       : (REL::Module::IsAE() ? "AE" : "SE");
        spdlog::info("Diet Dr Camera v" DDC_VERSION_STRING " | Skyrim {}.{}.{}.{} ({})",
                     v[0], v[1], v[2], v[3], rt);
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        spdlog::critical("[Startup] SKSE messaging interface is unavailable; no startup hooks can be registered");
        return false;
    }
    DietDrCamera::Diagnostics::Checkpoint("Registering SKSE message listener");
    if (!messaging->RegisterListener(MessageCallback)) {
        spdlog::critical("[Startup] SKSE messaging listener registration failed; initialization stopped");
        return false;
    }
    DietDrCamera::Diagnostics::Checkpoint("SKSEPlugin_Load complete; waiting for PostLoad");
    return true;
}
