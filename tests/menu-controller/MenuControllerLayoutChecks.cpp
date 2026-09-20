#include <atomic>
#include <cstddef>
#include <iostream>
namespace RE { class InputEvent; }
#include "UI/SKSEMenuFramework.h"
#undef IM_COL32
#undef IM_COL32_WHITE
#undef IM_COL32_BLACK
#undef IM_COL32_BLACK_TRANS
#include "imgui.h"
#include "imgui_internal.h"

// Never assume that the generated SDK describes the optional legacy IO arrays.
// These are the fields and Style-relative accesses used by capture and hints.
#define CHECK(T, F) static_assert(offsetof(T, F) == offsetof(ImGuiMCP::T, F), #T "::" #F)
CHECK(ImGuiIO, ConfigFlags);
CHECK(ImGuiIO, WantTextInput);
CHECK(ImGuiKeyData, Down);
CHECK(ImGuiKeyData, DownDuration);
CHECK(ImGuiKeyData, DownDurationPrev);
CHECK(ImGuiKeyData, AnalogValue);
static_assert(sizeof(ImGuiKeyData) == sizeof(ImGuiMCP::ImGuiKeyData));
CHECK(ImGuiWindow, Name);
CHECK(ImGuiWindow, ParentWindow);
CHECK(ImGuiWindow, DC);
CHECK(ImGuiWindow, NavLastIds);
CHECK(ImGuiWindow, Active);
CHECK(ImGuiWindow, Hidden);
CHECK(ImGuiWindow, LastFrameActive);
CHECK(ImGuiWindow, Pos);
CHECK(ImGuiWindow, Size);
CHECK(ImGuiWindow, SizeFull);
CHECK(ImGuiWindow, Flags);
CHECK(ImGuiWindow, WindowPadding);
CHECK(ImGuiWindow, FontWindowScale);
CHECK(ImGuiWindow, Scroll);
CHECK(ImGuiWindow, DecoOuterSizeX1);
CHECK(ImGuiWindow, DecoOuterSizeX2);
CHECK(ImGuiWindow, DecoOuterSizeY1);
CHECK(ImGuiWindow, DecoOuterSizeY2);
CHECK(ImGuiWindow, SetWindowPosVal); // Immediately follows the four size/position condition bitfields.
CHECK(ImGuiSizeCallbackData, UserData);
CHECK(ImGuiSizeCallbackData, DesiredSize);
static_assert(sizeof(ImGuiWindow) == sizeof(ImGuiMCP::ImGuiWindow));
static_assert(static_cast<int>(ImGuiCond_Always) == static_cast<int>(ImGuiMCP::ImGuiCond_Always));
CHECK(ImGuiWindow, ClipRect);
CHECK(ImGuiWindow, DrawList);
CHECK(ImGuiWindowTempData, ChildWindows);
CHECK(ImDrawList, Flags);
CHECK(ImFont, ContainerAtlas);
CHECK(ImFontAtlas, TexID);
static_assert(static_cast<int>(ImGuiNavLayer_Main) == static_cast<int>(ImGuiMCP::ImGuiNavLayer_Main));
static_assert(static_cast<int>(ImGuiItemFlags_NoNav) == static_cast<int>(ImGuiMCP::ImGuiItemFlags_NoNav));
static_assert(offsetof(ImGuiContext, ActiveIdAllowOverlap) - offsetof(ImGuiContext, Style) ==
    offsetof(ImGuiMCP::ImGuiContext, ActiveIdAllowOverlap) - offsetof(ImGuiMCP::ImGuiContext, Style));
static_assert(offsetof(ImGuiContext, CurrentWindow) - offsetof(ImGuiContext, Style) ==
    offsetof(ImGuiMCP::ImGuiContext, CurrentWindow) - offsetof(ImGuiMCP::ImGuiContext, Style));
int main() { std::cout << "Controller capture SDK layout checks passed\n"; }
