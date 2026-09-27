#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Adapt only the generated wrapper's output-pointer signatures. Geometry and
// column clipping come from real ImGui, then use production item registration.
namespace ImGuiMCP {
    using ::ImVec2;
    namespace ImGui {
        using namespace ::ImGui;
        inline void GetItemRectMin(ImVec2* out) { *out = ::ImGui::GetItemRectMin(); }
        inline void GetItemRectMax(ImVec2* out) { *out = ::ImGui::GetItemRectMax(); }
        inline void GetWindowPos(ImVec2* out) { *out = ::ImGui::GetWindowPos(); }
        inline void GetWindowSize(ImVec2* out) { *out = ::ImGui::GetWindowSize(); }
        inline void GetWindowContentRegionMin(ImVec2* out) { *out = ::ImGui::GetWindowContentRegionMin(); }
        inline void GetWindowContentRegionMax(ImVec2* out) { *out = ::ImGui::GetWindowContentRegionMax(); }
    }
}
#include "UI/MenuNavigationItem.h"

using DietDrCamera::DirectionalNavigation;
using DietDrCamera::NavigationItem;
static std::vector<NavigationItem> items;
static std::vector<std::string> names;
static ImGuiWindow* boundWindow;

static void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Record(std::string name, int layer = 0)
{
    items.push_back(DietDrCamera::CurrentMenuNavigationItem(layer, true));
    names.push_back(std::move(name));
}

static int Index(const std::string& name)
{
    const auto found = std::find(names.begin(), names.end(), name);
    Require(found != names.end(), "Missing native control: " + name);
    return static_cast<int>(found - names.begin());
}

static void Expect(DirectionalNavigation& nav, const std::string& from, int dx, int dy, const std::string& to, int layer = 0)
{
    const int result = nav.Move(items, Index(from), dx, dy, layer);
    if (result < 0 || names[result] != to) {
        for (const auto& name : {from,to,result < 0 ? from : names[result]}) {
            const auto& item=items[Index(name)];
            std::cerr << name << " rect=" << item.x0 << ',' << item.y0 << ',' << item.x1 << ',' << item.y1
                << " region=" << item.RegionX0() << ',' << item.RegionY0() << ',' << item.RegionX1() << ',' << item.RegionY1() << '\n';
        }
    }
    Require(result >= 0 && names[result] == to,
        from + " should lead to " + to + ", got " + (result < 0 ? "no movement" : names[result]));
}

static int LastVisible(const std::string& prefix, float bottom = std::numeric_limits<float>::infinity())
{
    int result = -1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
        if (names[i].starts_with(prefix) && items[i].Visible() && items[i].y1 <= bottom &&
            (result < 0 || items[i].y0 > items[result].y0)) result = i;
    Require(result >= 0, "No visible control in " + prefix);
    return result;
}

static void Frame(float scale, int boundCount, float scroll = -1)
{
    items.clear(); names.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({50,40});
    ImGui::SetNextWindowSize({1500 * scale,860 * scale});
    ImGui::Begin("Navigation layout",nullptr,ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextUnformatted("Animation / weapon / NPC bindings");
    const float footerY = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y -
        ImGui::GetFrameHeight() - ImGui::GetStyle().CellPadding.y;
    ImGui::BeginTable("Columns",3,ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
    ImGui::TableSetupColumn("Library",ImGuiTableColumnFlags_WidthStretch,0.34f);
    ImGui::TableSetupColumn("Bound",ImGuiTableColumnFlags_WidthStretch,0.20f);
    ImGui::TableSetupColumn("Editor",ImGuiTableColumnFlags_WidthStretch,0.46f);
    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
    ImGui::Button("Browse",{-1,0}); Record("browse");
    const float top = ImGui::GetCursorPosY();
    const float height = footerY - top - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("Library rows",{0,footerY+ImGui::GetFrameHeight()-top},ImGuiChildFlags_Border);
    for (int row = 0; row < 120; ++row) {
        const auto label = "library:" + std::to_string(row);
        ImGui::Selectable(label.c_str()); Record(label);
    }
    ImGui::EndChild();
    ImGui::TableSetColumnIndex(1);
    ImGui::Button("Add binding",{-1,0}); Record("add");
    ImGui::SetCursorPosY(top);
    ImGui::BeginChild("Bound rows",{0,height},ImGuiChildFlags_Border);
    boundWindow = ImGui::GetCurrentWindow();
    if (scroll >= 0) ImGui::SetScrollY(scroll);
    for (int row = 0; row < boundCount; ++row) {
        const auto name = "bound:" + std::to_string(row);
        const auto label = name + " a very long animation or weapon name that exceeds the column's width";
        ImGui::Selectable(label.c_str()); Record(name);
    }
    ImGui::EndChild();
    ImGui::SetCursorPosY(footerY);
    ImGui::BeginDisabled(boundCount == 0);
    ImGui::Button("Remove",{-1,0});
    if (boundCount) Record("remove");
    ImGui::EndDisabled();
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted("Editor");
    for (int row = 0; row < 6; ++row) {
        ImGui::PushID(row);
        ImGui::SmallButton("Reset"); Record("reset:" + std::to_string(row));
        ImGui::Button("Value",{-1,24 * scale}); Record("slider:" + std::to_string(row));
        ImGui::PopID();
    }
    ImGui::EndTable(); ImGui::End(); ImGui::Render();
}

static void CheckCurrentLayout(int count)
{
    // Parent controls share the window, but must retain their table columns.
    const auto& browse = items[Index("browse")];
    const auto& add = items[Index("add")];
    Require(browse.SameWindow(add) && browse.RegionX1() <= add.RegionX0(),
        "Production registration lost native table column boundaries");
    DirectionalNavigation nav;
    if (count) {
        const int last = LastVisible("bound:");
        Expect(nav,"remove",0,-1,names[last]);
        const int lastRow = std::stoi(names[last].substr(6));
        if (lastRow + 1 < count)
            Expect(nav,names[last],0,1,"bound:" + std::to_string(lastRow+1));
        else
            Expect(nav,names[last],0,1,"remove");

        // Rows beside the bound list enter that list; the taller library's
        // bottom rows sit beside Remove now that binding is a row action.
        const int origin = LastVisible("library:", items[last].RegionY1());
        const int originRow = std::stoi(names[origin].substr(8));
        const auto target = "bound:" + std::to_string(std::min(originRow,count-1));
        nav.Reset();
        Expect(nav,names[origin],1,0,target);
        Expect(nav,target,-1,0,names[origin]);
        Expect(nav,names[origin],1,0,target);
        const int editor = nav.Move(items,Index(target),1,0,0);
        Require(editor >= 0 && (names[editor].starts_with("reset:") || names[editor].starts_with("slider:")),
            "Right could not enter the editor from a sparse list");
        Expect(nav,names[editor],-1,0,target);
        Expect(nav,target,-1,0,names[origin]);
        const int libraryBottom = LastVisible("library:");
        nav.Reset();
        Expect(nav,names[libraryBottom],1,0,"remove");
        Expect(nav,"remove",-1,0,names[libraryBottom]);
        for (const auto& item : items)
            Require(item.x0 >= item.clipX0 && item.x1 <= item.clipX1,
                "A long label escaped its column's visible bounds");
    } else {
        Require(std::find(names.begin(),names.end(),"remove") == names.end(),"Disabled Remove remained navigable");
    }
    // Up/down throughout the editor must never enter either neighboring list.
    for (int row = 0; row < 6; ++row) {
        nav.Reset();
        Expect(nav,"slider:" + std::to_string(row),0,-1,"reset:" + std::to_string(row));
        Expect(nav,"reset:" + std::to_string(row),0,1,"slider:" + std::to_string(row));
        if (row < 5) Expect(nav,"slider:" + std::to_string(row),0,1,"reset:" + std::to_string(row+1));
    }
}

// Categories, Target Lock, Noise and several editors use a short auto-sized
// child list beside controls in the parent window, rather than a full-height
// table column. Lower sliders must still reach that list and back in one step.
static void ShortListFrame(float scale, int count, bool mirrored)
{
    items.clear(); names.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({50,40});
    ImGui::SetNextWindowSize({1500 * scale,1000 * scale});
    ImGui::Begin("Short entry list",nullptr,ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    if (ImGui::BeginTabBar("Category tabs")) {
        for (const auto* label : {"Sheathed", "Melee", "Magic", "Ranged", "Specific Weapons"}) {
            const bool open = ImGui::BeginTabItem(label);
            Record(std::string("tab:") + label);
            if (open) ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Button("Outdoor"); Record("environment:outdoor");
    ImGui::SameLine();
    ImGui::Button("Indoor"); Record("environment:indoor");
    if (!mirrored) {
        ImGui::SameLine(0,8 * scale);
        ImGui::SetCursorPosX(328 * scale);
        ImGui::Button("Transitions"); Record("toolbar:transitions");
    }
    ImGui::BeginChild("Category body",{0,0});
    ImGui::SetWindowFontScale(1.4f);
    const float listWidth = 320 * scale, gap = 8 * scale;
    const float editorWidth = ImGui::GetContentRegionAvail().x - listWidth - gap;
    const auto list = [&] {
        ImGui::BeginGroup();
        ImGui::BeginChild("Entries",{listWidth,0},ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        for (int row = 0; row < count; ++row) {
            const auto name = "entry:" + std::to_string(row);
            ImGui::Selectable(name.c_str(),row == 0); Record(name);
        }
        ImGui::EndChild();
        ImGui::SmallButton("Reset All To Default"); Record("reset-all");
        ImGui::EndGroup();
    };
    const auto editor = [&] {
        ImGui::BeginGroup();
        static float value = 0;
        for (int row = 0; row < 6; ++row) {
            ImGui::PushID(row);
            ImGui::TextUnformatted("Camera setting"); ImGui::SameLine();
            ImGui::SmallButton("Reset"); Record("reset:" + std::to_string(row));
            ImGui::SetNextItemWidth(editorWidth);
            ImGui::SliderFloat("##value",&value,-100,100); Record("slider:" + std::to_string(row));
            ImGui::Dummy({0,24 * scale});
            ImGui::PopID();
        }
        ImGui::EndGroup();
    };
    if (mirrored) { editor(); ImGui::SameLine(0,gap); list(); }
    else { list(); ImGui::SameLine(0,gap); editor(); }
    ImGui::EndChild(); ImGui::End(); ImGui::Render();
}

static void CheckShortList(int count, bool mirrored)
{
    for (const auto& route : {
        std::pair{"tab:Sheathed","environment:outdoor"},
        std::pair{"tab:Melee","environment:indoor"},
        std::pair{"tab:Magic","environment:indoor"},
        std::pair{"tab:Ranged",mirrored ? "reset:0" : "toolbar:transitions"},
        std::pair{"tab:Specific Weapons",mirrored ? "reset:0" : "toolbar:transitions"}}) {
        DirectionalNavigation nav;
        Expect(nav,route.first,0,1,route.second);
        Expect(nav,route.second,0,-1,route.first);
    }
    for (const auto* toggle : {"environment:outdoor","environment:indoor"}) {
        DirectionalNavigation nav;
        Expect(nav,toggle,0,1,mirrored ? "slider:0" : count ? "entry:0" : "reset-all");
        if (!mirrored && count > 1) Expect(nav,"entry:0",0,1,"entry:1");
    }
    if (!mirrored) {
        DirectionalNavigation nav;
        // With no rows, Reset All sits immediately below the toolbar and
        // overlaps its left edge. Populated lists put that action farther down.
        Expect(nav,"toolbar:transitions",0,1,count ? "slider:0" : "reset-all");
    }
    if (!count) return;
    const auto destination = "entry:" + std::to_string(count - 1);
    for (int row : {4,5}) {
        const auto origin = "slider:" + std::to_string(row);
        Require(items[Index(origin)].y0 > items[Index(destination)].RegionY1(),
            "Short-list fixture does not reproduce a slider below the list");
        DirectionalNavigation nav;
        Expect(nav,origin,mirrored ? 1 : -1,0,destination);
        Expect(nav,destination,mirrored ? -1 : 1,0,origin);
        Expect(nav,origin,mirrored ? 1 : -1,0,destination);
    }
}

// The new inline pair must remain reachable in both directions, and a
// nested scope popup must keep navigation inside its own modal layer.
struct PopupReferenceMetrics { float fontSize = 0, buttonHeight = 0; };
static PopupReferenceMetrics resetPopupMetrics;
static PopupReferenceMetrics applyPopupMetrics;
static float applyPopupCenter = 0, applyHeadingCenter = 0, applySeparatorY = 0;

static void ResetConfirmationFrame(float scale)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({50,40});
    ImGui::SetNextWindowSize({1100 * scale,800 * scale});
    ImGui::Begin("Reset reference", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.4f);
    ImGui::OpenPopup("Reset reference confirmation");
    ImGui::SetNextWindowSizeConstraints({480 * scale,0}, {760 * scale,10000});
    if (ImGui::BeginPopupModal("Reset reference confirmation", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetWindowFontScale(1.4f);
        resetPopupMetrics = {ImGui::GetFontSize(), ImGui::GetFrameHeight()};
        ImGui::TextUnformatted("Reset all entries in this tab to default?");
        ImGui::TextUnformatted("Other tabs are not affected.");
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - 12 * scale) * 0.5f;
        ImGui::Button("Confirm", {buttonWidth,0}); ImGui::SameLine(0,12 * scale);
        ImGui::Button("Cancel", {buttonWidth,0});
        ImGui::EndPopup();
    }
    ImGui::End(); ImGui::Render();
}

static void ApplyActionsFrame(float scale, bool popup, int environment = 0, float parentScale = 2.0f)
{
    items.clear(); names.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({50,40});
    ImGui::SetNextWindowSize({1100 * scale,800 * scale});
    ImGui::Begin("Apply actions", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextUnformatted("FOV: 100.00"); ImGui::SameLine(0,16 * scale);
    ImGui::SetWindowFontScale(1.1f);
    ImGui::SmallButton("Apply"); Record("apply"); ImGui::SameLine(0,8 * scale);
    ImGui::SmallButton("Reset"); Record("reset");
    static float value = 100;
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##fov", &value, 50, 140); Record("slider");
    if (popup) {
        ImGui::SetWindowFontScale(parentScale);
        ImGui::OpenPopup("Apply scope");
        ImGui::SetNextWindowPos({600 * scale,400 * scale}, ImGuiCond_Always, {0.5f,0.5f});
        constexpr float confirmationScale = 1.4f * 1.4f;
        const float gap = 12 * scale;
        const ImVec2 padding(24 * scale,20 * scale);
        float widestLabel = 0;
        for (const char* label : {"Entire Tab + Overrides", "All Outdoor + Overrides", "All Indoor + Overrides"})
            widestLabel = std::max(widestLabel, ImGui::CalcTextSize(label).x * confirmationScale / parentScale);
        const float buttonMinimum = widestLabel + ImGui::GetStyle().FramePadding.x * 2 + 24 * scale;
        const float width = std::max(1000 * scale, buttonMinimum * 2 + gap + padding.x * 2);
        ImGui::SetNextWindowSizeConstraints({width,0}, {width,10000});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap,10 * scale));
        if (ImGui::BeginPopupModal("Apply scope", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::SetWindowFontScale(confirmationScale / parentScale);
            applyPopupMetrics = {ImGui::GetFontSize(), ImGui::GetFrameHeight()};
            applyPopupCenter = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f;
            const float available = ImGui::GetContentRegionAvail().x;
            const float titleWidth = ImGui::CalcTextSize("Apply FOV = 100.00").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - titleWidth) * 0.5f);
            ImGui::TextUnformatted("Apply FOV = 100.00");
            applyHeadingCenter = (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f;
            ImGui::Dummy({0,12 * scale});
            const float buttonWidth = (available - gap) * 0.5f;
            const auto button = [&](const char* label) {
                Require(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 <= buttonWidth,
                    std::string("Apply label clipped: ") + label);
                ImGui::Button(label, {buttonWidth,0}); Record(label,1);
            };
            button("Entire Tab"); ImGui::SameLine(0,gap); button("Entire Tab + Overrides");
            if (environment >= 0) {
                button(environment ? "All Indoor" : "All Outdoor"); ImGui::SameLine(0,gap);
                button(environment ? "All Indoor + Overrides" : "All Outdoor + Overrides");
            }
            button("All Tabs"); ImGui::SameLine(0,gap); button("All Tabs + Overrides");
            ImGui::Dummy({0,8 * scale});
            applySeparatorY = ImGui::GetCursorScreenPos().y;
            ImGui::Separator();
            ImGui::Dummy({0,8 * scale});
            const float cancelWidth = std::max(220 * scale,
                ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2 + 32 * scale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - cancelWidth) * 0.5f);
            ImGui::Button("Cancel", {cancelWidth,0}); Record("Cancel",1);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::End(); ImGui::Render();
}

static void CheckApplyActions(float scale)
{
    ResetConfirmationFrame(scale); ResetConfirmationFrame(scale);
    ImGui::NewFrame(); ImGui::ClosePopupsOverWindow(nullptr,false); ImGui::EndFrame();
    ApplyActionsFrame(scale,false); ApplyActionsFrame(scale,false);
    DirectionalNavigation nav;
    Expect(nav,"apply",1,0,"reset"); Expect(nav,"reset",-1,0,"apply");
    Require(items[Index("apply")].x1 < items[Index("reset")].x0, "Apply overlaps Reset");
    for (const int environment : {-1,0,1}) {
        for (const float parentScale : {1.0f,1.4f,2.0f}) {
            for (int frame = 0; frame < 3; ++frame) ApplyActionsFrame(scale,true,environment,parentScale);
            nav.Reset();
            Require(std::abs(applyPopupMetrics.fontSize - resetPopupMetrics.fontSize) < 0.01f &&
                std::abs(applyPopupMetrics.buttonHeight - resetPopupMetrics.buttonHeight) < 0.01f,
                "Apply does not match Reset All confirmation font/control scaling");
            Require(std::abs(applyHeadingCenter - applyPopupCenter) <= 1.0f,
                "Apply heading is not centered: heading=" + std::to_string(applyHeadingCenter) +
                " popup=" + std::to_string(applyPopupCenter) + " scale=" + std::to_string(scale));
            const auto& cancel = items[Index("Cancel")];
            Require(std::abs((cancel.x0 + cancel.x1) * 0.5f - applyPopupCenter) <= 1.0f,
                "Apply Cancel is not centered");
            Require(applySeparatorY > items[Index("All Tabs")].y1 && applySeparatorY < cancel.y0 &&
                cancel.y0 - items[Index("All Tabs")].y1 >= 30 * scale,
                "Cancel footer lacks separation from Apply choices");
            std::vector<std::array<const char*,2>> rows{{"Entire Tab", "Entire Tab + Overrides"}};
            if (environment >= 0)
                rows.push_back({environment ? "All Indoor" : "All Outdoor",
                    environment ? "All Indoor + Overrides" : "All Outdoor + Overrides"});
            rows.push_back({"All Tabs", "All Tabs + Overrides"});
            for (std::size_t row = 0; row < rows.size(); ++row) {
                Expect(nav,rows[row][0],1,0,rows[row][1],1);
                Expect(nav,rows[row][1],-1,0,rows[row][0],1);
                const auto& left = items[Index(rows[row][0])];
                const auto& right = items[Index(rows[row][1])];
                Require(left.x1 < right.x0 && left.y0 == right.y0 && left.Visible() && right.Visible(),
                    "Apply choices are not visible side-by-side");
                if (row + 1 < rows.size()) {
                    for (int column = 0; column < 2; ++column) {
                        Expect(nav,rows[row][column],0,1,rows[row+1][column],1);
                        Expect(nav,rows[row+1][column],0,-1,rows[row][column],1);
                    }
                }
            }
            for (const char* origin : {"All Tabs", "All Tabs + Overrides"}) {
                nav.Reset();
                Expect(nav,origin,0,1,"Cancel",1);
                Expect(nav,"Cancel",0,-1,origin,1);
            }
            const auto next = nav.Move(items,Index("Cancel"),0,1,1);
            Require(next < 0 || items[next].layer == 1, "Apply popup navigation escaped to background controls");
            ImGui::NewFrame(); ImGui::ClosePopupsOverWindow(nullptr,false); ImGui::EndFrame();
        }
    }
}

// Extras uses a wide tab strip above a left-aligned disable toggle. The
// Vanity tab sits over the slider column, but Down must enter at its toggle.
static void VanityFrame(float scale, bool disabled)
{
    items.clear(); names.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({50,40});
    ImGui::SetNextWindowSize({1800 * scale,860 * scale});
    ImGui::Begin("Vanity navigation",nullptr,ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.5f);
    if (ImGui::BeginTabBar("Extras tabs")) {
        for (const char* label : {"Projectile Tracing", "Death Camera", "Ragdoll Camera",
                "Vanity Camera", "Menus", "Cinematic Effects"}) {
            const bool vanity = std::string_view(label) == "Vanity Camera";
            const bool open = ImGui::BeginTabItem(label,nullptr,vanity ? ImGuiTabItemFlags_SetSelected : 0);
            Record(std::string("tab:") + label);
            items.back().tab = true;
            if (open) {
                if (vanity) {
                    ImGui::SetWindowFontScale(1.4f);
                    ImGui::Dummy({0,10 * scale});
                    ImGui::SetWindowFontScale(2.1f);
                    ImGui::Checkbox("Disable Vanity Camera", &disabled);
                    Record("vanity:toggle");
                    items.back().entryTab = items[Index("tab:Vanity Camera")].id;
                    ImGui::SetWindowFontScale(1.4f);
                    if (!disabled) {
                        ImGui::Dummy({0,6 * scale});
                        ImGui::TextUnformatted("Idle Timer");
                        ImGui::SetNextItemWidth(-1);
                        float idle = 120;
                        ImGui::SliderFloat("##idle", &idle,5,600); Record("vanity:idle");
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::SetWindowFontScale(1.5f);
        }
        ImGui::EndTabBar();
    }
    ImGui::End(); ImGui::Render();
}

static void CheckVanityEntry(float scale)
{
    for (bool disabled : {false,true}) {
        VanityFrame(scale,disabled); VanityFrame(scale,disabled); VanityFrame(scale,disabled);
        DirectionalNavigation nav;
        Expect(nav,"tab:Vanity Camera",0,1,"vanity:toggle");
        Expect(nav,"vanity:toggle",0,-1,"tab:Vanity Camera");
        Expect(nav,"tab:Vanity Camera",0,1,"vanity:toggle");
        if (disabled) {
            Require(std::find(names.begin(),names.end(),"vanity:idle") == names.end(),
                "Disabled vanity camera retained a focusable slider");
            Require(nav.Move(items,Index("vanity:toggle"),0,1,0) < 0,
                "Down from the disabled vanity toggle reached hidden controls");
        } else {
            Expect(nav,"vanity:toggle",0,1,"vanity:idle");
        }
        nav.Reset();
        Expect(nav,"tab:Vanity Camera",-1,0,"tab:Ragdoll Camera");
        Expect(nav,"tab:Ragdoll Camera",1,0,"tab:Vanity Camera");
    }
}

int main() try
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={3840,2160}; io.DeltaTime=1.0f/60;
    ImFontConfig font; font.SizePixels=24; io.Fonts->AddFontDefault(&font);
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::GetStyle().WindowBorderSize=3; ImGui::GetStyle().ChildBorderSize=3;
    const auto style = ImGui::GetStyle();
    for (float scale : {0.5f,2.0f/3.0f,1.0f,1.2f}) {
        io.FontGlobalScale=scale; ImGui::GetStyle()=style; ImGui::GetStyle().ScaleAllSizes(scale);
        for (int count : {0,1,3,120}) {
            Frame(scale,count,0); Frame(scale,count); Frame(scale,count);
            CheckCurrentLayout(count);
        }
        for (int count : {0,1,3,6}) {
            for (bool mirrored : {false,true}) {
                ShortListFrame(scale,count,mirrored); ShortListFrame(scale,count,mirrored); ShortListFrame(scale,count,mirrored);
                CheckShortList(count,mirrored);
            }
        }
        CheckApplyActions(scale);
        CheckVanityEntry(scale);
        Frame(scale,120,boundWindow->ScrollMax.y*0.5f); Frame(scale,120); Frame(scale,120);
        DirectionalNavigation nav;
        const int last=LastVisible("bound:");
        Expect(nav,"remove",0,-1,names[last]);
        Expect(nav,names[last],0,1,"bound:"+std::to_string(std::stoi(names[last].substr(6))+1));
    }
    ImGui::DestroyContext();
    std::cout << "Native menu navigation checks passed (columns, sparse/dense lists, scroll, footer reentry, return paths, long labels, resets and UI scales).\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
