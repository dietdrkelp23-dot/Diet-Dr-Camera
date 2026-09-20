// Included inside MenuUI's namespace: use the menu's existing pad widgets.
struct AnimationLibraryLayout { float top, buttonTop; };

static bool AnimationRowNearViewport()
{
    using namespace ImGuiMCP;
    ImVec2 lo, hi, pos, size;
    ImGui::GetItemRectMin(&lo); ImGui::GetItemRectMax(&hi);
    ImGui::GetWindowPos(&pos); ImGui::GetWindowSize(&size);
    // Keep neighboring rows navigable without filling the pad registry with
    // thousands of offscreen files. D-pad navigation scrolls each row into view.
    return hi.y >= pos.y - Sx(40.0f) && lo.y <= pos.y + size.y + Sx(40.0f);
}

static AnimationPlayerContext AnimationLibraryPlayerContext()
{
    AnimationPlayerContext context;
    const auto identity = [](RE::TESForm* form) {
        AnimationFormIdentity result;
        if (form) {
            if (auto* file = form->GetFile(0)) {
                result.plugin = file->fileName;
                result.localID = form->GetLocalFormID();
            }
            if (const auto* id = form->GetFormEditorID()) result.editorID = id;
        }
        return result;
    };
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* currentRace = player ? player->GetRace() : nullptr;
    if (auto* data = RE::TESDataHandler::GetSingleton()) {
        for (auto* race : data->GetFormArray<RE::TESRace>()) {
            if (!race) continue;
            const auto id = identity(race);
            // Playable races include custom character races. Keep the player's
            // current race and the two vanilla transformations available too.
            if (race == currentRace || race->data.flags.any(RE::RACE_DATA::Flag::kPlayable) ||
                id.editorID == "WerewolfBeastRace" || id.editorID == "DLC1VampireBeastRace") context.races.push_back(id);
        }
    }
    if (player) {
        context.reference = identity(player);
        context.actorBase = identity(player->GetActorBase());
    }
    return context;
}

static bool AnimationDropdown(const char* label, const std::string& key, std::set<std::string>& expanded)
{
    using namespace ImGuiMCP;
    const bool wasOpen = expanded.contains(key);
    ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Framed |
        ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopItemFlag();
    if (AnimationRowNearViewport() && PadHandleItem(PadRegisterItem(), false)) { open = !open; PadPressFlash(); }
    if (open) expanded.insert(key);
    else expanded.erase(key);
    return open;
}

static AnimationLibraryLayout RenderAnimationLibrary(SettingsManager& settings,
    AnimationCameraController& controller, std::uint32_t& selectedBinding, float bottom)
{
    using namespace ImGuiMCP;
    struct Submod { std::string name, key; std::vector<std::size_t> files; };
    struct Mod { std::string name, key; std::vector<Submod> submods; std::size_t count = 0; };
    struct Browser
    {
        AnimationCatalog catalog;
        std::size_t revision = 0, availableCount = 0;
        char search[160]{};
        std::vector<Mod> mods;
        std::set<std::string> openMods, openSubmods, boundPaths;
        bool dirty = true, recent = false;
    };
    static Browser browser;
    if (!browser.catalog.Started()) browser.catalog.Refresh(std::filesystem::absolute("Data/meshes"), AnimationLibraryPlayerContext());
    browser.catalog.Poll();
    if (browser.revision != browser.catalog.Revision()) {
        browser.revision = browser.catalog.Revision();
        browser.dirty = true;
    }
    // Compare paths, not just the entry count: switching presets can replace
    // bindings without changing their number. Removed bindings reappear too.
    std::set<std::string> boundPaths;
    for (const auto& entry : settings.animationCameras)
        boundPaths.insert(AnimationPathKey(entry.animationPath));
    if (browser.boundPaths != boundPaths) {
        browser.boundPaths = std::move(boundPaths);
        browser.dirty = true;
    }
    const auto& catalog = browser.catalog.Result();
    ImGui::TextUnformatted("Player Animation Library");
    ImGui::SetNextItemWidth(-1.0f);
    if (PadInputText("Search##anim_library", browser.search, sizeof(browser.search))) {
        browser.dirty = true;
        browser.openMods.clear(); browser.openSubmods.clear();
    }
    if (browser.dirty) {
        browser.mods.clear();
        browser.availableCount = 0;
        for (std::size_t i = 0; i < catalog.animations.size(); ++i) {
            const auto& animation = catalog.animations[i];
            if (browser.boundPaths.contains(animation.key)) continue;
            ++browser.availableCount;
            if (!AnimationSearchMatches(animation.search, browser.search)) continue;
            // Catalog order is mod name/key, submod name/key, then filename.
            if (browser.mods.empty() || browser.mods.back().key != animation.modKey)
                browser.mods.push_back({animation.mod, animation.modKey});
            auto& mod = browser.mods.back();
            if (mod.submods.empty() || mod.submods.back().key != animation.submodKey)
                mod.submods.push_back({animation.submod, animation.submodKey});
            mod.submods.back().files.push_back(i);
            ++mod.count;
            if (browser.search[0]) {
                browser.openMods.insert(mod.key);
                browser.openSubmods.insert(mod.submods.back().key);
            }
        }
        browser.dirty = false;
    }
    if (PadButton(browser.recent ? "Browse##anim_library" : "Recent##anim_library"))
        browser.recent = !browser.recent;
    if (browser.recent) {
        bool armed = controller.CaptureArmed();
        if (PadCheckbox("Record recent animations", &armed)) controller.SetCaptureArmed(armed);
    } else {
        ImGui::SameLine();
        if (browser.catalog.Busy()) ImGui::TextUnformatted("Scanning...");
        else ImGui::Text("%zu available", browser.availableCount);
    }
    if (!catalog.error.empty()) ImGui::TextWrapped("%s", catalog.error.c_str());
    if (catalog.skipped) ImGui::TextWrapped("Some files or metadata could not be read (%zu).", catalog.skipped);

    const float top = ImGui::GetCursorPosY();
    // The library fills the pane; only the bound column needs a Remove row.
    const float spacing = ImGui::GetStyle()->ItemSpacing.y;
    const float buttonTop = std::max(top + 4.0f + spacing, bottom - ImGui::GetFrameHeight());
    const float height = buttonTop + ImGui::GetFrameHeight() - top;
    const auto drawFile = [&](const CatalogAnimation& animation) {
        ImGui::PushID(animation.key.c_str());
        const bool replacement = animation.key.find("animationreplacer\\") != std::string::npos;
        const bool canBind = !animation.key.empty() && !animation.firstPerson && (!replacement || controller.OarAvailable());
        if (!canBind) PadBeginDisabled();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        bool selected = ImGui::Selectable(animation.filename.c_str(), false);
        ImGui::PopItemFlag();
        if (AnimationRowNearViewport() && PadHandleItem(PadRegisterItem(), false)) selected = true;
        if (selected && canBind) {
            const auto existing = std::find_if(settings.animationCameras.begin(), settings.animationCameras.end(),
                [&](const auto& entry) { return AnimationPathKey(entry.animationPath) == animation.key; });
            if (existing != settings.animationCameras.end()) selectedBinding = existing->uid;
            else {
                SettingsManager::AnimationCameraEntry entry;
                entry.name = animation.submod.empty() ? animation.filename : animation.submod + ": " + animation.filename;
                entry.animationPath = animation.key; entry.modName = animation.mod; entry.subModName = animation.submod;
                settings.animationCameras.push_back(std::move(entry));
                settings.AssignAnimationCameraUids();
                selectedBinding = settings.animationCameras.back().uid;
                controller.RebuildMatchIndex();
            }
            PadPressFlash();
        }
        if (!canBind) PadEndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (animation.firstPerson)
                ImGui::SetTooltip("This file animates first-person arms. These camera entries follow the player's body animations.");
            else if (replacement && !controller.OarAvailable())
                ImGui::SetTooltip("Open Animation Replacer is required to bind this replacement animation.");
            else ImGui::SetTooltip("%s / %s\n%s",
                animation.mod.c_str(), animation.submod.c_str(), animation.key.c_str());
        }
        ImGui::PopID();
    };
    if (PadNavBeginChild("##anim_library_rows", ImVec2(0, height),
        ImGuiChildFlags_Border, ImGuiWindowFlags_NavFlattened)) {
        if (browser.recent) {
            bool any = false;
            for (const auto& clip : controller.GetCapturedClips()) {
                if (browser.boundPaths.contains(clip.key)) continue;
                CatalogAnimation row;
                row.key = clip.key; row.filename = clip.display; row.mod = clip.mod; row.submod = clip.subMod;
                row.search = clip.key + " " + clip.display + " " + clip.mod + " " + clip.subMod;
                for (auto& ch : row.search) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (!AnimationSearchMatches(row.search, browser.search)) continue;
                drawFile(row); any = true;
            }
            if (!any) ImGui::TextWrapped("No matching unbound recent animations. Recording is optional; Browse lists installed player animations.");
        } else {
            if (browser.mods.empty()) ImGui::TextWrapped(browser.catalog.Busy() ? "Reading installed player animations..." :
                browser.search[0] ? "No unbound player animations match this search." :
                catalog.animations.empty() ? "No OAR or DAR player animations found." : "All player animations are already bound.");
            for (const auto& mod : browser.mods) {
                ImGui::PushID(mod.key.c_str());
                const auto label = mod.name + " (" + std::to_string(mod.count) + ")###mod";
                if (AnimationDropdown(label.c_str(), mod.key, browser.openMods)) {
                    ImGui::Indent(Sx(12.0f));
                    for (const auto& submod : mod.submods) {
                        ImGui::PushID(submod.key.c_str());
                        const auto subLabel = submod.name + " (" + std::to_string(submod.files.size()) + ")###submod";
                        if (AnimationDropdown(subLabel.c_str(), submod.key, browser.openSubmods)) {
                            ImGui::Indent(Sx(12.0f));
                            auto* clipper = ImGui::ImGuiListClipperManager::Create();
                            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                            ImGui::ImGuiListClipperManager::Begin(clipper, static_cast<int>(submod.files.size()), rowHeight);
                            ImVec2 cursor, pos, size;
                            ImGui::GetCursorScreenPos(&cursor);
                            ImGui::GetWindowPos(&pos); ImGui::GetWindowSize(&size);
                            const int count = static_cast<int>(submod.files.size());
                            const int first = std::clamp(static_cast<int>((pos.y - cursor.y) / rowHeight) - 2, 0, count);
                            const int last = std::clamp(static_cast<int>((pos.y + size.y - cursor.y) / rowHeight) + 3, first, count);
                            // Include neighbors for the custom d-pad navigator.
                            // The rest is clipped, not paginated: scrollbar size
                            // and alphabetical order cover the entire list.
                            if (first < last) ImGui::ImGuiListClipperManager::IncludeItemsByIndex(clipper, first, last);
                            while (ImGui::ImGuiListClipperManager::Step(clipper))
                                for (int f = clipper->DisplayStart; f < clipper->DisplayEnd; ++f)
                                    drawFile(catalog.animations[submod.files[f]]);
                            ImGui::ImGuiListClipperManager::Destroy(clipper);
                            ImGui::Unindent(Sx(12.0f));
                        }
                        ImGui::PopID();
                    }
                    ImGui::Unindent(Sx(12.0f));
                }
                ImGui::PopID();
            }
        }
    }
    PadNavEndChild();

    return {top, buttonTop};
}
