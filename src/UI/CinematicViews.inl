// Included in MenuUI.cpp so these controls share its scaling and pad navigation.
static void DrawCinematicSubjectMarker(const CinematicViewPicker::Subject& subject)
{
    using namespace ImGuiMCP;
    if (!subject.onScreen) return;
    const auto* io = ImGui::GetIO();
    auto* draw = ImGui::GetBackgroundDrawList();
    if (!io || !draw) return;
    const ImVec2 center{subject.screenX*io->DisplaySize.x,subject.screenY*io->DisplaySize.y};
    const float radius = Sx(12);
    ImGui::ImDrawListManager::AddCircle(draw,center,radius+Sx(2),0xE6000000,32,Sx(4));
    ImGui::ImDrawListManager::AddCircle(draw,center,radius,0xFF5CC7F2,32,Sx(2));
    ImGui::ImDrawListManager::AddLine(draw,{center.x-radius*1.5f,center.y},{center.x+radius*1.5f,center.y},0xFF5CC7F2,Sx(2));
    ImGui::ImDrawListManager::AddLine(draw,{center.x,center.y-radius*1.5f},{center.x,center.y+radius*1.5f},0xFF5CC7F2,Sx(2));
}

static void RenderCinematicViewsContent()
{
    using namespace ImGuiMCP;
    namespace Views = CinematicViews;
    namespace Controller = CinematicViewController;
    auto& views = SettingsManager::GetSingleton().cinematicViews;
    const auto editor = Controller::GetEditorState();
    static std::string selected;
    static std::string selectedSubject;
    static bool choosing = false;
    constexpr float textScale = 1.25f;
    constexpr float listScale = 1.5f;
    const bool wasCompact = sCompactScale;
    const bool wasMedium = sMediumScale;
    const bool wasBiggerLabel = sBiggerLabel;
    sCompactScale = true;
    sMediumScale = false;
    sBiggerLabel = false;
    if (views.empty()) choosing = true;
    if (choosing && !editor.scanned && !editor.scanPending) Controller::RefreshSubjects();
    static ScaledMenuSize leftWidthCache{420};
    float& leftWidth = leftWidthCache.Pixels(UiScale());
    // Child font sizes include their parent's scale. Keep this parent neutral.
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Dummy(ImVec2(0,Sx(4)));
    ImVec2 available{}; ImGui::GetContentRegionAvail(&available);
    leftWidth = std::clamp(leftWidth,Sx(240),(std::max)(Sx(240),available.x-Sx(400)));
    const float height = MenuScrollableBodyHeight(available.y,Sx(8));
    if (PadNavBeginChild("##views_list",ImVec2(leftWidth,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NavFlattened)) {
        ImGui::SetWindowFontScale(listScale);
        ImGui::TextUnformatted("Views");
        ImGui::Separator();
        if (views.size() < 128 && PadButton("+ Add",ImVec2(-1,0))) {
            choosing = true;
            Controller::RefreshSubjects();
        }
        ImGui::Dummy(ImVec2(0,Sx(4)));
        for (const auto& view : views) {
            ImGui::PushID(view.id.c_str());
            if (PadSelectable(view.name.c_str(),!choosing && selected == view.id)) { selected = view.id; choosing = false; }
            ImGui::PopID();
        }
        if (views.empty()) ImGui::TextUnformatted("No views");
        if (views.size() >= 128) ImGui::TextUnformatted("128-view limit reached");
    }
    PadNavEndChild();
    ImGui::SameLine(0,Sx(8));
    if (PadNavBeginChild("##views_editor",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NavFlattened)) {
        ImGui::SetWindowFontScale(textScale);
        auto found = std::find_if(views.begin(),views.end(),[&](const auto& v) { return v.id == selected; });
        if (found == views.end() && !views.empty()) { found = views.begin(); selected = found->id; }
        if (choosing) {
            ImGui::TextUnformatted("Choose Subject");
            ImGui::Separator();
            if (editor.scanPending) PadBeginDisabled();
            if (PadButton("Refresh")) Controller::RefreshSubjects();
            if (editor.scanPending) PadEndDisabled();
            if ((editor.scanPending || editor.subjects.empty()) && !editor.pickerStatus.empty())
                ImGui::TextWrapped("%s",editor.pickerStatus.c_str());
            if (!editor.status.empty()) ImGui::TextWrapped("%s",editor.status.c_str());
            if (views.size() >= 128) ImGui::TextUnformatted("128-view limit reached");
            const auto current = std::find_if(editor.subjects.begin(),editor.subjects.end(),
                [&](const auto& subject) { return subject.key == selectedSubject; });
            if (current == editor.subjects.end()) selectedSubject = editor.subjects.empty() ? "" : editor.subjects.front().key;
            const bool canBind = choosing && !editor.scanPending && views.size() < 128;
            if (!canBind) PadBeginDisabled();
            if (PadNavBeginChild("##view_subjects",ImVec2(0,0),ImGuiChildFlags_Border,ImGuiWindowFlags_NavFlattened)) {
                ImGui::SetWindowFontScale(listScale / textScale);
                for (const auto& subject : editor.subjects) {
                    ImGui::PushID(subject.key.c_str());
                    const std::string label = (subject.aimed ? "Aimed: " : "") + subject.view.name;
                    const bool clicked = PadSelectable(label.c_str(),selectedSubject == subject.key);
                    if (ImGui::IsItemHovered() || (sPadScopeOn && sPadHasCursor &&
                        PadSameItem(PadCurrentItemRect(),sPadCursorRect))) selectedSubject = subject.key;
                    if (clicked && canBind) {
                        const auto added = Controller::AddBinding(subject.key);
                        if (!added.empty()) { selected = added; choosing = false; }
                    }
                    ImGui::PopID();
                    if (!choosing) break;
                }
            }
            PadNavEndChild();
            if (!canBind) PadEndDisabled();
            const auto subject = std::find_if(editor.subjects.begin(),editor.subjects.end(),
                [&](const auto& candidate) { return candidate.key == selectedSubject; });
            if (choosing && subject != editor.subjects.end()) DrawCinematicSubjectMarker(*subject);
        } else if (found == views.end()) {
            ImGui::TextUnformatted("No view selected");
        } else {
            auto& view = *found;
            ImGui::PushID(view.id.c_str());
            char name[97]{};
            std::snprintf(name,sizeof(name),"%s",view.name.c_str());
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-1);
            if (PadInputText("##view_name",name,sizeof(name)) && name[0]) view.name = name;
            ImGui::Dummy(ImVec2(0,Sx(4)));
            ImGui::TextUnformatted("Camera");
            ImGui::Separator();
            RenderTransitionOverride(&view.profile,0.0f,textScale);
            ImGui::Dummy(ImVec2(0,Sx(4)));
            const float wasControlScale = sCompactControlScale;
            sCompactControlScale = 1.25f;
            ImGui::BeginGroup();
            RenderProfileBlock(&view.profile,CameraProfile::Default3p());
            ImGui::EndGroup();
            MarkEntryHover(EntryClipKind::Camera,&view.profile,view.name.c_str());
            RenderSlider("Lock-On Tightness",&view.lockOnTightness,0,1,.01f,Views::kDefaultLockOnTightness);
            ImGui::SetWindowFontScale(textScale);
            ImGui::TextUnformatted("Activation");
            ImGui::Separator();
            if (!editor.hasCamera) PadBeginDisabled();
            if (PadButton("Set Here")) Controller::SetTriggerHere(view.id);
            if (!editor.hasCamera) PadEndDisabled();
            ImGui::Dummy(ImVec2(0,Sx(4)));
            RenderSlider("Radius",&view.radius,50,12000,25,500);
            RenderSlider("Idle Timer",&view.idleDelay,.5f,600,.5f,5,
                nullptr,"Reset To Default",false,"%.1fs");
            sCompactControlScale = wasControlScale;
            ImGui::SetWindowFontScale(textScale);
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0,Sx(4)));
            ImVec2 actionSpace{}; ImGui::GetContentRegionAvail(&actionSpace);
            float rowWidth = 0;
            const auto actionButton = [&](const char* label) {
                ImVec2 text{}; ImGui::CalcTextSize(&text,label,nullptr,false,-1);
                const float width = text.x+ImGui::GetStyle()->FramePadding.x*2;
                const float gap = ImGui::GetStyle()->ItemSpacing.x;
                if (rowWidth > 0 && rowWidth+gap+width <= actionSpace.x) {
                    ImGui::SameLine(0,gap);
                    rowWidth += gap;
                } else rowWidth = 0;
                rowWidth += width;
                return PadButton(label);
            };
            if (Controller::HasSeen(view.id) && actionButton("Reset Encounter")) Controller::Forget(view.id);
            const bool remove = actionButton("Remove");
            if (!editor.status.empty()) ImGui::TextWrapped("%s",editor.status.c_str());
            ImGui::PopID();
            if (remove) { CameraController::InvalidateProfileReferences(); views.erase(found); selected.clear(); }
        }
    }
    PadNavEndChild();
    sCompactScale = wasCompact;
    sMediumScale = wasMedium;
    sBiggerLabel = wasBiggerLabel;
    ImGui::SetWindowFontScale(1.5f);
}
