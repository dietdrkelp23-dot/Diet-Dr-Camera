// A tiny test DLL, never installed or packaged with the mod.
namespace { int contextStorage; bool ready = false; }

extern "C" __declspec(dllexport) void* igGetCurrentContext()
{
    return ready ? &contextStorage : nullptr;
}

extern "C" __declspec(dllexport) void SetFixtureContextReady()
{
    ready = true;
}
