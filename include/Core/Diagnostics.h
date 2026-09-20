#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace SKSE { class LoadInterface; }

namespace DietDrCamera::Diagnostics
{
    // Also used by the standalone checks; starts a new session and retains
    // three previous logs. Throws if the destination cannot be used.
    void StartSession(const std::filesystem::path& directory);
    bool InitializeLog();
    std::string LogLocation();
    void LogEnvironment(const SKSE::LoadInterface& skse);
    void LogLoadedModules();
    void Checkpoint(std::string_view step);
    std::string DescribeAddress(std::uintptr_t address);
    std::string DescribeBranchTarget(std::uintptr_t address);
    void LogBytes(std::string_view label, std::uintptr_t address, std::size_t count = 32);
}
