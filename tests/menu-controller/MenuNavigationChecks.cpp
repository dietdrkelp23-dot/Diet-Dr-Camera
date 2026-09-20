#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
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

static void Record(std::string name)
{
    items.push_back(DietDrCamera::CurrentMenuNavigationItem(0, true));
    names.push_back(std::move(name));
}

static int Index(const std::string& name)
{
    const auto found = std::find(names.begin(), names.end(), name);
    Require(found != names.end(), "Missing native control: " + name);
    return static_cast<int>(found - names.begin());
}

static void Expect(DirectionalNavigation& nav, const std::string& from, int dx, int dy, const std::string& to)
{
    const int result = nav.Move(items, Index(from), dx, dy, 0);
    if (result < 0 || names[result] != to) {
        for (const auto& name : {from,to}) {
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
        Frame(scale,120,boundWindow->ScrollMax.y*0.5f); Frame(scale,120); Frame(scale,120);
        DirectionalNavigation nav;
        const int last=LastVisible("bound:");
        Expect(nav,"remove",0,-1,names[last]);
        Expect(nav,names[last],0,1,"bound:"+std::to_string(std::stoi(names[last].substr(6))+1));
    }
    ImGui::DestroyContext();
    std::cout << "Native menu navigation checks passed (columns, sparse/dense lists, scroll, footer reentry, return paths, long labels, resets and UI scales).\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
