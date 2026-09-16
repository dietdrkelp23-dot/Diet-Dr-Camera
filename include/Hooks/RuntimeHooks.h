#pragma once

#include <cstddef>
#include <cstdint>

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
    void DisableUIJob();
    std::size_t InputSlot(std::size_t legacySlot);
}
