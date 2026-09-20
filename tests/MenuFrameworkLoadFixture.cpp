#include "UI/MenuFrameworkBinding.h"
// A tiny test DLL, never installed or packaged with the mod.
namespace { int contextStorage; bool ready = false; }
namespace { DietDrCamera::MenuFrameworkBinding::WindowInterface window; }

extern "C" __declspec(dllexport) void* igGetCurrentContext()
{
    return ready ? &contextStorage : nullptr;
}

extern "C" __declspec(dllexport) void SetFixtureContextReady()
{
    ready = true;
}

extern "C" __declspec(dllexport) DietDrCamera::MenuFrameworkBinding::WindowInterface* GetMainWindow() { return ready ? &window : nullptr; }
extern "C" __declspec(dllexport) void SetFixtureWindowOpen(bool open) { window.IsOpen.store(open); }
