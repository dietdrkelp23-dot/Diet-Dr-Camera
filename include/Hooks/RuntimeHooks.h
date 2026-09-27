#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace DietDrCamera::RuntimeHooks
{
    struct Sites
    {
        std::uintptr_t cameraUpdate;
        std::uintptr_t enterFurniture;
        std::uintptr_t mainUpdate;
        std::uintptr_t dialogueTimer;
        std::uintptr_t uiJobBranch;
        std::size_t uiJobBranchLength;
        std::uintptr_t processMessages;
        std::uintptr_t advanceMovies;
        std::uintptr_t getConsole;
        std::uintptr_t executeConsole;
    };

    // Resolve and validate every instruction patch before installing any hooks.
    void Prepare();
    const Sites& Get();
    void RequireCall(std::uintptr_t address);
    // Used by the live collision toggle. Refuse to overwrite a patch whose
    // current bytes no longer match DDC's last owned state.
    bool TryReplaceCode6(std::uintptr_t address, const std::array<std::uint8_t, 6>& expected,
        const std::array<std::uint8_t, 6>& replacement);
    // Validate all five preflight bytes and retain DDC's own installed chain
    // as the expected bytes for a subsequent DDC hook at the same call site.
    std::uintptr_t InstallCall(std::uintptr_t address, std::uintptr_t hook);
    template <class T>
    std::uintptr_t InstallCall(std::uintptr_t address, T* hook)
    {
        return InstallCall(address, reinterpret_cast<std::uintptr_t>(hook));
    }
    void DisableUIJob();
    std::size_t InputSlot(std::size_t legacySlot);
}
