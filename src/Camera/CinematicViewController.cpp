#include "PCH.h"
#include "Camera/CinematicViewController.h"
#include "Camera/CameraController.h"
#include "Camera/WorldCameraRay.h"
#include "Core/CinematicViewHistory.h"
#include "Core/CinematicViewSightline.h"
#include "RE/N/NiPick.h"
#include "RE/S/SendHUDMessage.h"
#include "LockOn/TDMIntegration.h"
#include "Settings/SettingsManager.h"
#include "UI/MenuUI.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

namespace DietDrCamera::CinematicViewController
{
    namespace
    {
        using namespace CinematicViews;
        constexpr std::uint32_t historyRecord = 0x56534545; // VSEE
        std::mutex guard;
        constexpr unsigned lookInput = 1, actionInput = 2, movementInput = 4;
        std::atomic_uint input{0};
        std::atomic_bool active{false};
        std::atomic_bool reserved{false};
        std::atomic_bool menuObserved{false};
        Playback playback;
        IdleTimer idle;
        ResumeGate resumeGate, quickTuneResume;
        History seen;
        EditorState editor;
        std::string pendingPreview;
        float encounterScanDelay = 0;
        std::uint64_t editorGeneration = 0;
        double lastTick = 0, lastStatusLog = 0;
        bool preview = false, baseValid = false, lastPositionValid = false;
        bool rendered = false, profileApplied = false;
        bool quickTuneWasOpen = false;
        RE::NiTransform base{};
        RE::NiFrustum baseFrustum{};
        RE::NiPoint3 lastPlayerPosition{}, focus{};

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        Point PointOf(RE::NiPoint3 p) { return {p.x,p.y,p.z}; }
        RE::NiPoint3 Native(Point p) { return {p.x,p.y,p.z}; }
        ItemBindings::FormIdentity Identity(RE::TESForm* form)
        {
            if (!form || form->IsDynamicForm()) return {};
            auto* file = form->GetFile(0);
            return file ? ItemBindings::FormIdentity{std::string{file->GetFilename()},form->GetLocalFormID()} : ItemBindings::FormIdentity{};
        }
        ItemBindings::FormIdentity Space(RE::PlayerCharacter* player, bool& exterior)
        {
            auto* world = player->GetWorldspace();
            exterior = world != nullptr;
            return Identity(world ? static_cast<RE::TESForm*>(world) : player->GetParentCell());
        }
        bool SameSpace(const View& view, RE::PlayerCharacter* player)
        {
            bool exterior;
            const auto space = Space(player,exterior);
            return MatchesSpace(view,space,exterior);
        }
        bool Resolve(const View& view, RE::PlayerCharacter* player, RE::NiPoint3& point, const char** reason = nullptr)
        {
            const auto fail = [&](const char* message) { if (reason) *reason = message; return false; };
            if (!SameSpace(view,player)) return fail("Return to this view's location.");
            if (!view.target.Valid()) { point = Native(view.point); return true; }
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* ref = data ? data->LookupForm<RE::TESObjectREFR>(view.target.localID,view.target.plugin) : nullptr;
            if (!ref || ref->IsDeleted() || ref->IsDisabled()) return fail("The bound subject is unavailable.");
            if (!ref->Get3D() || !ref->GetParentCell()) return fail("Move closer to load the bound subject.");
            if (view.exterior ? ref->GetWorldspace() != player->GetWorldspace() : ref->GetParentCell() != player->GetParentCell())
                return fail("The bound subject is in another location.");
            point = ref->GetPosition() + Native(view.targetOffset);
            return PointOf(point).Finite() || fail("The bound subject has no valid position.");
        }
        RE::NiCamera* PlayerNiCamera()
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            auto* root = cam && cam->cameraRoot ? cam->cameraRoot->AsNode() : nullptr;
            if (root) for (const auto& child : root->GetChildren())
                if (auto* ni = skyrim_cast<RE::NiCamera*>(child.get())) return ni;
            return nullptr;
        }
        bool MenuOpen()
        {
            const auto blocked = [] { menuObserved.store(true,std::memory_order_relaxed); return true; };
            auto* ui = RE::UI::GetSingleton();
            const bool tuning = MenuUI::IsQuickTuneWindowOpen() && !MenuUI::IsMainMenuOpen();
            if (!ui || (!tuning && (ui->GameIsPaused() || MenuUI::IsGameInputBlocked()))) return blocked();
            for (const char* name : {"Main Menu", "Loading Menu", "Dialogue Menu", "TweenMenu",
                 "InventoryMenu", "MagicMenu", "ContainerMenu", "BarterMenu", "GiftMenu",
                 "MapMenu", "Journal Menu", "Console", "Crafting Menu", "FavoritesMenu",
                 "Book Menu", "Sleep/Wait Menu", "RaceSex Menu"})
                if (ui->IsMenuOpen(name)) return blocked();
            return false;
        }
        const char* BlockReason(RE::PlayerCharacter* player, RE::PlayerCamera* camera, bool tuning)
        {
            if (!baseValid || !player || !player->Get3D() || !camera || !camera->currentState)
                return "Waiting for the gameplay camera.";
            if (SettingsManager::GetSingleton().diagnosticSuspendOverrides) return "Camera effects are suspended.";
            const auto state = camera->currentState->id;
            if (state != RE::CameraState::kThirdPerson)
                return "Cinematic Views use third person.";
            const auto* controls = RE::ControlMap::GetSingleton();
            const auto* actorState = player->AsActorState();
            if (!controls || (!tuning && !controls->IsMovementControlsEnabled()) || !actorState) return "Waiting for player controls.";
            if (actorState->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return "Stand up to play a view.";
            if (player->IsInCombat()) return "Views wait until combat ends.";
            if (player->IsOnMount()) return "Dismount to play a view.";
            if (player->IsDead() || player->IsInKillMove()) return "Views are unavailable in the current player state.";
            if (TDMIntegration::GetSingleton().IsTargetLocked()) return "Release target lock to play a view.";
            return nullptr;
        }
        bool Visible(RE::PlayerCharacter* player, const View& view, RE::NiPoint3 point)
        {
            const auto ray = point-base.translate;
            const float distance = ray.Length();
            const auto direction = distance > .01f ? ray/distance : RE::NiPoint3{};
            RE::NiPick::Ptr pick;
            return ClearSightline<RE::TESObjectREFR*>(distance,
                [&](std::span<RE::TESObjectREFR* const> excluded) {
                    return TraceCameraRay(player,base.translate,point,false,excluded);
                },
                [&](RE::TESObjectREFR* ref) {
                    return ref && view.target.Valid() && ItemBindings::FormKey(Identity(ref)) == ItemBindings::FormKey(view.target);
                },
                [&](RE::TESObjectREFR* ref) -> std::optional<float> {
                    auto* node = ref ? ref->Get3D() : nullptr;
                    if (!node) return std::nullopt;
                    if (!pick) {
                        pick = RE::NiPick::Create(8,8);
                        if (!pick) return std::nullopt;
                        pick->pickType = RE::NiPick::PickType::FIND_ALL;
                        pick->sortType = RE::NiPick::SortType::SORT;
                        pick->intersectType = RE::NiPick::IntersectType::TRIANGLE_INTERSECT;
                        pick->coordinateType = RE::NiPick::CoordinateType::WORLD_COORDINATES;
                        pick->frontOnly = false; pick->observeAppCullFlag = true;
                        pick->returnNormal = pick->returnSmoothNormal = pick->returnTexture = pick->returnColor = false;
                    }
                    pick->root.reset(node);
                    float nearest = std::numeric_limits<float>::infinity();
                    if (pick->PickObjects(base.translate,direction,false)) {
                        const auto count = (std::min)(pick->pickResults.resultsCount,static_cast<std::uint32_t>(pick->pickResults.capacity()));
                        for (std::uint32_t i=0; i<count; ++i) {
                            const auto* hit = pick->pickResults[static_cast<std::uint16_t>(i)];
                            if (!hit || !hit->object || !PointOf(hit->intersect).Finite()) continue;
                            const float along = (hit->intersect-base.translate).Dot(direction);
                            if (along >= 0) nearest = (std::min)(nearest,along);
                        }
                    }
                    return nearest;
                });
        }
        void SetPlaybackStatus(std::string status)
        {
            if (editor.playbackStatus == status) return;
            editor.playbackStatus = std::move(status);
            const double now = Now();
            if (now-lastStatusLog >= 3 && !SettingsManager::GetSingleton().cinematicViews.empty()) {
                spdlog::info("Cinematic Views: {}",editor.playbackStatus);
                lastStatusLog = now;
            }
        }
        void RejectPreview(const char* reason)
        {
            spdlog::info("Cinematic Views: preview '{}' blocked: {}",pendingPreview,reason);
            editor.status = std::string{"Preview: "}+reason;
            SetPlaybackStatus(editor.status);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([message=editor.status] { RE::SendHUDMessage::ShowHUDMessage(message.c_str(),nullptr,false); });
            }
            pendingPreview.clear();
        }
        void Start(const View& view, RE::NiPoint3 point, bool isPreview)
        {
            playback.Start(view,isPreview || !seen.contains(view.id));
            focus = point; preview = isPreview; active = reserved = true;
            rendered = profileApplied = false;
            editor.status = isPreview ? "Preview starting." : "View starting.";
            SetPlaybackStatus(editor.status);
            spdlog::info("Cinematic Views: starting {} '{}' ({}) radius={:.1f} idleTimer={:.1f} timed={} profile(side={:.1f}, height={:.1f}, zoom={:.1f}, fov={:.1f}, rotation={:.1f}, pitch={:.1f})",
                isPreview ? "preview" : playback.timed ? "first encounter" : "idle visit",view.name,view.id,
                view.radius,view.idleDelay,playback.timed,
                view.profile.sideOffset,view.profile.height,view.profile.zoom,view.profile.fov,view.profile.rotation,view.profile.pitchOffset);
        }
        void Finish(const char* reason)
        {
            if (!playback.tuning.id.empty())
                spdlog::info("Cinematic Views: finished '{}' after {:.2f}s (preview={}, rendered={}): {}",
                    playback.tuning.name,playback.age,preview,rendered,reason);
            playback = {}; active = reserved = false; profileApplied = false;
            idle.Reset(); encounterScanDelay = 0;
            editor.status = reason; SetPlaybackStatus(reason);
        }
        float Facing(RE::NiPoint3 point)
        {
            const auto ray = point-base.translate;
            const float length = ray.Length();
            return length > .01f ? ray.Dot({base.rotate.entry[0][0],base.rotate.entry[1][0],base.rotate.entry[2][0]})/length : 1;
        }
        void ResetRuntime()
        {
            playback = {}; active = reserved = false; preview = false;
            idle.Reset(); resumeGate.Reset(); quickTuneResume.Reset(); rendered = profileApplied = false;
            quickTuneWasOpen = false;
            pendingPreview.clear();
            encounterScanDelay = 0; lastTick = lastStatusLog = 0;
            ++editorGeneration;
            baseValid = lastPositionValid = false; editor = {}; input.store(0); menuObserved.store(false);
        }
        void Revert(SKSE::SerializationInterface*)
        {
            std::scoped_lock lock(guard); ResetRuntime(); seen.clear();
        }
        void Save(SKSE::SerializationInterface* intfc)
        {
            std::scoped_lock lock(guard);
            const auto bytes = EncodeHistory(seen);
            if (!intfc->WriteRecord(historyRecord,1,bytes.data(),static_cast<std::uint32_t>(bytes.size())))
                spdlog::warn("Cinematic Views: could not save encounter history");
        }
        void Load(SKSE::SerializationInterface* intfc)
        {
            std::scoped_lock lock(guard); ResetRuntime(); seen.clear();
            std::uint32_t type,version,length;
            while (intfc->GetNextRecordInfo(type,version,length)) {
                if (type != historyRecord || version != 1 || length > 266240) continue;
                std::vector<std::uint8_t> bytes(length);
                if ((length && intfc->ReadRecordData(bytes.data(),length) != length) || !DecodeHistory(bytes,seen))
                    spdlog::warn("Cinematic Views: ignored invalid encounter history");
            }
        }

    }

    void InstallSerialization()
    {
        if (auto* intfc = SKSE::GetSerializationInterface()) {
            intfc->SetUniqueID(0x44444356); // DDCV
            intfc->SetSaveCallback(Save); intfc->SetLoadCallback(Load); intfc->SetRevertCallback(Revert);
        }
    }
    void Reset() { std::scoped_lock lock(guard); ResetRuntime(); }
    bool IsActive() { return active.load(std::memory_order_relaxed); }
    bool WantsCamera() { return IsActive() || reserved.load(std::memory_order_relaxed); }
    void OnInput(RE::InputEvent* events)
    {
        if (MenuOpen() || MenuUI::IsQuickTuneWindowOpen()) return;
        unsigned bits = 0;
        for (auto* ev=events; ev; ev=ev->next) {
            if (auto* mouse=ev->AsMouseMoveEvent()) {
                if (mouse->mouseInputX || mouse->mouseInputY) bits |= lookInput;
            } else if (auto* stick=ev->AsThumbstickEvent()) {
                if (!StickMoved(stick->xValue,stick->yValue)) continue;
                const auto* mapped = stick->QUserEvent().c_str();
                const std::string_view name = mapped ? mapped : "";
                bits |= name == "Look" || (name != "Move" && stick->IsRight()) ? lookInput : movementInput;
            } else if (auto* button=ev->AsButtonEvent()) {
                const auto code = button->GetIDCode();
                const auto encoded = button->GetDevice() == RE::INPUT_DEVICE::kGamepad ? code | 0x10000000u : code;
                if (encoded && encoded == SettingsManager::GetSingleton().quickTuneHotkey) continue;
                const auto* mapped = button->QUserEvent().c_str();
                const std::string_view name = mapped ? mapped : "";
                const bool movement = name == "Forward" || name == "Back" || name == "Strafe Left" || name == "Strafe Right" ||
                    name == "Sprint" || name == "Jump" || name == "Sneak";
                if (movement && button->IsPressed()) bits |= movementInput;
                else if (button->IsDown() && (name == "Left Attack/Block" || name == "Right Attack/Block" || name == "Shout" ||
                    name == "Zoom In" || name == "Zoom Out" || name == "Toggle POV")) bits |= actionInput;
            }
        }
        input.fetch_or(bits,std::memory_order_relaxed);
    }
    void Tick()
    {
        std::scoped_lock lock(guard);
        const double now = Now(), elapsed = lastTick > 0 ? now-lastTick : 0;
        lastTick = now;
        const float dt = static_cast<float>(std::clamp(elapsed,0.0,.25));
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        const auto activity = input.exchange(0,std::memory_order_relaxed);
        const bool looking = (activity & lookInput) != 0;
        const bool action = (activity & actionInput) != 0;
        const bool quickTune = MenuUI::IsQuickTuneWindowOpen() && !MenuUI::IsMainMenuOpen();
        if (quickTune != quickTuneWasOpen) {
            if (playback.active) spdlog::info("Cinematic Views: Quick Tune {} for '{}' at {:.2f}s",
                quickTune ? "opened" : "closed",playback.tuning.name,playback.age);
            quickTuneWasOpen = quickTune;
        }
        const bool tuningHeld = !quickTuneResume.Step(quickTune,dt);
        editor.hasCamera = baseValid;
        const bool menuOpen = MenuOpen();
        const bool observed = menuObserved.exchange(false,std::memory_order_relaxed);
        if (observed) resumeGate.Reset();
        if (!menuOpen && editor.scanned) { editor.scanned = false; editor.subjects.clear(); }
        if (!resumeGate.Step(menuOpen,dt)) {
            if (playback.active) Finish("Menu opened.");
            active = false; reserved = !pendingPreview.empty(); editor.active = false;
            idle.Reset(); lastPositionValid = false; return;
        }
        // Tuning holds an existing view, never discovers a new one behind UI.
        if (quickTune && !playback.active) {
            idle.Reset(); lastPositionValid = false; reserved = !pendingPreview.empty(); return;
        }
        if (const auto* reason = BlockReason(player,camera,playback.active && tuningHeld)) {
            if (playback.active) Finish(reason);
            active = reserved = false; idle.Reset(); lastPositionValid = false;
            editor.active = false;
            if (!pendingPreview.empty()) RejectPreview(reason);
            else SetPlaybackStatus(reason);
            return;
        }
        const bool moved = (activity & movementInput) != 0 || (lastPositionValid &&
            (player->GetPosition()-lastPlayerPosition).Length() > (std::max)(.001f,dt));
        lastPlayerPosition = player->GetPosition(); lastPositionValid = true;
        idle.Step(activity != 0 || moved || tuningHeld,static_cast<float>(elapsed));
        auto& views = SettingsManager::GetSingleton().cinematicViews;
        const auto findView = [&](std::string_view id) -> const View* {
            for (const auto& v : views) if (v.id == id) return &v;
            return nullptr;
        };
        if (playback.active) {
            const auto* current = findView(playback.tuning.id);
            RE::NiPoint3 target;
            if (!current || (!preview && !current->enabled) || !Resolve(*current,player,target))
                Finish("The view or its subject is no longer available.");
            else {
                focus = target;
                playback.tuning = Sanitize(*current);
                // Radius/visibility/idle are acquisition conditions. Walking
                // beyond the radius or briefly behind scenery must not drop an
                // acquired lock. Native camera collision remains in charge.
                playback.Step(profileApplied ? dt : 0,looking,action,tuningHeld);
                if (!playback.active) Finish(looking ? "Camera movement released the view." :
                    action ? "Player input released the view." : "First encounter finished.");
            }
            editor.active = active; return;
        }
        if (tuningHeld) { reserved = !pendingPreview.empty(); return; }
        if (!pendingPreview.empty()) {
            const auto* view = findView(pendingPreview);
            RE::NiPoint3 point;
            const char* reason = nullptr;
            if (!view) RejectPreview("The view was removed or its preset changed.");
            else if (!Resolve(*view,player,point,&reason)) RejectPreview(reason);
            else { Start(*view,point,true); pendingPreview.clear(); }
        } else {
            encounterScanDelay -= dt;
            if (encounterScanDelay <= 0) {
                encounterScanDelay = .15f;
                const View* best = nullptr;
                RE::NiPoint3 bestPoint;
                reserved = false;
                float bestScore = -std::numeric_limits<float>::infinity();
                float nearestDistance = std::numeric_limits<float>::infinity();
                std::string waiting = "No enabled views in this location.";
                for (const auto& view : views) {
                    if (!view.enabled) continue;
                    RE::NiPoint3 point;
                    const char* reason = nullptr;
                    if (!Resolve(view,player,point,&reason)) {
                        if (!std::isfinite(nearestDistance)) waiting = view.name + ": " + reason;
                        continue;
                    }
                    const float distance = TriggerDistance(view,PointOf(player->GetPosition()),PointOf(point));
                    const bool encountered = seen.contains(view.id);
                    if (distance > view.radius) reason = "Move inside this view's Radius.";
                    else if (!Visible(player,view,point)) reason = "The subject is obstructed.";
                    if (!reason) {
                        reserved = true; // A subject's independent timer takes priority over vanity.
                        if (!Eligible(view,encountered,distance,true,idle.Seconds()))
                            reason = "Waiting for this view's Idle Timer.";
                    }
                    if (distance < nearestDistance) {
                        nearestDistance = distance;
                        waiting = view.name + ": " + (reason ? reason : "Preparing view.");
                    }
                    if (reason) continue;
                    const float score = (encountered ? 0.0f : 2.0f)+1-distance/(std::max)(view.radius,1.0f)+.1f*Facing(point);
                    if (score > bestScore) { bestScore = score; best = &view; bestPoint = point; }
                }
                SetPlaybackStatus(best ? best->name + ": Preparing view." : std::move(waiting));
                // Radius entry does not require aiming at the subject first.
                if (best && !looking && !action) Start(*best,bestPoint,false);
            }
        }
        editor.active = active;
    }
    std::optional<ActiveEntry> GetActiveEntry()
    {
        std::scoped_lock lock(guard);
        if (!playback.active || !active) return std::nullopt;
        return ActiveEntry{playback.tuning.id,PointOf(focus-base.translate)};
    }
    void ProfileApplied(std::string_view id)
    {
        std::scoped_lock lock(guard);
        if (id.empty() && playback.active) { Finish("Another camera entry has priority."); return; }
        profileApplied = playback.active && playback.tuning.id == id;
    }
    void CaptureBase(RE::NiCamera* camera)
    {
        // Observe the engine's composed pose for picking/visibility/history.
        // Cinematic Views never write NiCamera transforms or frustum fields.
        if (!camera || camera != PlayerNiCamera() || MenuOpen()) return;
        std::scoped_lock lock(guard);
        base = camera->world; baseValid = PointOf(base.translate).Finite();
        baseFrustum = camera->viewFrustum;
        if (playback.active && profileApplied) {
            if (!rendered) {
                rendered = true;
                editor.status = preview ? "Preview playing." : "View playing.";
                SetPlaybackStatus(editor.status);
                spdlog::info("Cinematic Views: native third-person entry '{}' rendered",playback.tuning.name);
            }
            // A real rendered first visit consumes the radius encounter even
            // when the player dismisses it early. Later visits use idle timing.
            if (!preview && playback.timed && seen.size() < 4096) seen.insert(playback.tuning.id);
        }
    }
    EditorState GetEditorState()
    {
        std::scoped_lock lock(guard);
        MenuOpen(); // Record paused editor visits even when no camera tick runs.
        return editor;
    }
    void RefreshSubjects()
    {
        std::scoped_lock lock(guard);
        if (editor.scanPending) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) { editor.scanned = true; editor.pickerStatus = "Picker unavailable."; return; }
        editor.scanPending = true;
        editor.pickerStatus = "Scanning...";
        editor.status.clear();
        const auto generation = editorGeneration;
        tasks->AddTask([generation] {
            std::scoped_lock lock(guard);
            if (generation != editorGeneration) return;
            editor.scanPending = false;
            editor.scanned = true;
            if (!baseValid) {
                editor.subjects.clear();
                editor.pickerStatus = "Close the menu and aim, then reopen.";
                spdlog::info("Cinematic Views: subject scan needs a gameplay camera pose");
                return;
            }
            auto result = CinematicViewPicker::Scan(RE::PlayerCharacter::GetSingleton(),base,baseFrustum);
            editor.subjects = std::move(result.subjects);
            editor.pickerStatus = std::move(result.status);
        });
    }
    std::string AddBinding(std::string_view subjectKey)
    {
        std::scoped_lock lock(guard);
        const auto candidate = std::find_if(editor.subjects.begin(),editor.subjects.end(),
            [&](const auto& subject) { return subject.key == subjectKey; });
        auto& views = SettingsManager::GetSingleton().cinematicViews;
        if (candidate == editor.subjects.end() || views.size() >= 128) return {};
        auto view = candidate->view;
        static std::uint64_t sequence = 0;
        view.id = fmt::format("{:016x}-{:x}",std::chrono::high_resolution_clock::now().time_since_epoch().count(),++sequence);
        view = Sanitize(view);
        if (!Valid(view)) { editor.status = "Could not add view. Refresh and retry."; return {}; }
        // A new binding is armed immediately. The menu/resume gate prevents
        // playback while editing; the author need not leave a landmark's range
        // before the first attempt can play.
        resumeGate.Reset();
        CameraController::InvalidateProfileReferences();
        views.push_back(view);
        const auto trigger = view.triggerPoint.value_or(view.point);
        spdlog::info("Cinematic Views: bound '{}' ({}) to {} focus=({:.1f}, {:.1f}, {:.1f}) trigger=({:.1f}, {:.1f}, {:.1f}) radius={:.1f}",
            view.name,view.id,view.target.Valid() ? ItemBindings::FormKey(view.target) : std::string{"scenery point"},
            view.point.x,view.point.y,view.point.z,trigger.x,trigger.y,trigger.z,view.radius);
        editor.status = "View added.";
        return view.id;
    }
    void SetTriggerHere(std::string_view id)
    {
        std::scoped_lock lock(guard);
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) { editor.status = "Location unavailable."; return; }
        const auto generation = editorGeneration;
        tasks->AddTask([id=std::string{id},generation] {
            std::scoped_lock lock(guard);
            if (generation != editorGeneration) return;
            auto& views = SettingsManager::GetSingleton().cinematicViews;
            auto found = std::find_if(views.begin(),views.end(),[&](const auto& view) { return view.id == id; });
            if (found == views.end()) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->GetParentCell() || !SameSpace(*found,player)) {
                editor.status = "Return to this view's location.";
                return;
            }
            const auto point = PointOf(player->GetPosition());
            auto updated = *found;
            updated.triggerPoint = point;
            if (!Valid(updated)) { editor.status = "Location unavailable."; return; }
            found->triggerPoint = point;
            seen.erase(id);
            idle.Reset(); resumeGate.Reset(); encounterScanDelay = 0;
            editor.status = "Trigger moved here.";
            spdlog::info("Cinematic Views: moved trigger '{}' ({}) to ({:.1f}, {:.1f}, {:.1f}) in {} radius={:.1f}",
                found->name,id,point.x,point.y,point.z,ItemBindings::FormKey(found->space),found->radius);
        });
    }
    void Preview(std::string_view id)
    {
        std::scoped_lock lock(guard);
        // Movement/camera state while the editor is open is not the resumed
        // gameplay state. Always queue the request and validate after closing.
        pendingPreview = id; playback = {}; active = false; reserved = true;
        resumeGate.Reset(); input.store(0,std::memory_order_relaxed);
        editor.status = "Close menu to preview.";
        editor.playbackStatus = editor.status;
        spdlog::info("Cinematic Views: preview '{}' queued for menu close",id);
    }
    bool HasSeen(std::string_view id) { std::scoped_lock lock(guard); return seen.contains(std::string{id}); }
    void Forget(std::string_view id)
    {
        std::scoped_lock lock(guard); seen.erase(std::string{id});
        editor.status = "Encounter reset.";
    }
}
