#pragma once

#include <atomic>
#include <optional>
#include <Windows.h>

namespace DietDrCamera::MenuFrameworkBinding
{
    // Public SMF window ABI. Risa uses these same two atomics to open and close
    // the main window without calling the framework's event-dispatching path.
    struct WindowInterface {
        std::atomic<bool> IsOpen{false};
        std::atomic<bool> BlockUserInput{true};
    };
    inline HMODULE Module() noexcept
    {
        // SKSE 2.0.20 loads DDC before Menu Framework. Resolving during DDC's
        // static initialization would retain a null handle for the whole run.
        // Cache successful lookups only; a later-loaded dependency can recover.
        static std::atomic<HMODULE> cached{nullptr};
        auto module = cached.load(std::memory_order_acquire);
        if (!module) {
            module = GetModuleHandleW(L"SKSEMenuFramework.dll");
            if (module) cached.store(module, std::memory_order_release);
        }
        return module;
    }

    // Preserve the bundled SDK's handle-based GetProcAddress calls without
    // editing its generated bindings. Conversion happens at the point of use.
    struct DeferredModuleHandle
    {
        operator HMODULE() const noexcept { return Module(); }
    };

    inline bool HasContext() noexcept
    {
        const auto module = Module();
        if (!module) return false;
        using GetContext = void* (*)();
        const auto getContext = reinterpret_cast<GetContext>(
            GetProcAddress(module, "igGetCurrentContext"));
        return getContext && getContext() != nullptr;
    }

    inline std::optional<bool> MainWindowOpen() noexcept
    {
        const auto module = Module();
        if (!module) return std::nullopt;
        using GetWindow = WindowInterface* (*)();
        static const auto getWindow = reinterpret_cast<GetWindow>(GetProcAddress(module,"GetMainWindow"));
        if (!getWindow) return std::nullopt;
        const auto* window = getWindow();
        return window ? std::optional<bool>{window->IsOpen.load(std::memory_order_relaxed)} : std::nullopt;
    }
}

inline constexpr DietDrCamera::MenuFrameworkBinding::DeferredModuleHandle menuFramework{};
