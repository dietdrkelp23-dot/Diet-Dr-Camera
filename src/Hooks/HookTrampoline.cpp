#include "PCH.h"
#include "Hooks/HookTrampoline.h"

#include <mutex>

namespace DietDrCamera::HookTrampoline
{
    void Initialize()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            auto& trampoline = SKSE::GetTrampoline();
            if (!trampoline.empty()) {
                SKSE::stl::report_and_fail("Diet Dr Camera's trampoline was initialized before its shared hook reservation.");
            }
            if (SKSE::GetTrampolineInterface()) {
                SKSE::AllocTrampoline(capacity);
            } else {
                // Historical SKSE releases may not expose the shared branch pool.
                // Allocate near Skyrim's code using CommonLib's local fallback.
                trampoline.create(capacity);
            }
            if (trampoline.capacity() < capacity) {
                SKSE::stl::report_and_fail("Diet Dr Camera could not reserve its shared hook trampoline.");
            }
            spdlog::info("[Hooks] Shared trampoline reserved: {} bytes", trampoline.capacity());
        });
    }
}
