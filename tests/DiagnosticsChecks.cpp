#include "PCH.h"
#include "Core/Diagnostics.h"

#include <Windows.h>
#include <process.h>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{stream}, {}};
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring_view(argv[1]) == L"--abrupt-exit") {
        DietDrCamera::Diagnostics::StartSession(argv[2]);
        spdlog::info("session-marker-{}", std::filesystem::path(argv[3]).string());
        spdlog::critical("fatal-marker");
        std::_Exit(0);  // No C++ destructors or stdio shutdown may rescue buffered output.
    }
    if (argc != 1) {
        std::cerr << "Invalid diagnostic test arguments\n";
        return 2;
    }
    const auto originalLogger = spdlog::default_logger();
    const auto originalCwd = std::filesystem::current_path();
    const auto directory = std::filesystem::temp_directory_path() /
        ("DDC-DiagnosticsChecks-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    try {
        Check(std::filesystem::create_directory(directory), "could not create unique test directory");
        wchar_t executable[32768]{};
        Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path unavailable");
        const auto quotedExecutable = L"\"" + std::wstring(executable) + L"\"";
        const auto quotedDirectory = L"\"" + directory.wstring() + L"\"";
        for (int session = 0; session < 5; ++session) {
            const auto marker = std::to_wstring(session);
            Check(_wspawnl(_P_WAIT, executable, quotedExecutable.c_str(), L"--abrupt-exit", quotedDirectory.c_str(), marker.c_str(),
                static_cast<const wchar_t*>(nullptr)) == 0,
                "abrupt-exit child failed");
            const auto current = ReadFile(directory / "DietDrCamera.log");
            Check(current.find("fatal-marker") != std::string::npos &&
                current.find("session-marker-" + std::to_string(session)) != std::string::npos,
                "startup or fatal output was lost after abrupt exit");
        }
        for (int previous = 1; previous <= 3; ++previous) {
            const auto contents = ReadFile(directory / ("DietDrCamera." + std::to_string(previous) + ".log"));
            Check(contents.find("session-marker-" + std::to_string(4 - previous)) != std::string::npos,
                "wrong previous session retained");
        }
        Check(!std::filesystem::exists(directory / "DietDrCamera.4.log"), "retention exceeded three previous sessions");

        using namespace DietDrCamera;
        Diagnostics::StartSession(directory / "details");
        Check(REL::Module::mock({1,6,659,0}), "mock runtime failed");
        std::filesystem::current_path(directory);
        std::filesystem::create_directories("Data/SKSE/Plugins");
        SKSE::Impl::SKSEInterface skse{};
        skse.skseVersion = (2u << 24) | (2u << 16) | (3u << 4);
        skse.runtimeVersion = (1u << 24) | (6u << 16) | (659u << 4);
        const auto& load = *reinterpret_cast<const SKSE::LoadInterface*>(&skse);
        Diagnostics::LogEnvironment(load);  // Missing Address Library must remain diagnosable.
        const auto library = directory / "Data/SKSE/Plugins/versionlib-1-6-659-0.bin";
        for (const auto format : {1u, 2u, 5u}) {
            const std::array<std::uint32_t, 5> header{format, 1, 6, 659, 0};
            { std::ofstream file(library, std::ios::binary); file.write(reinterpret_cast<const char*>(header.data()), sizeof(header)); }
            Diagnostics::LogEnvironment(load);
        }
        { std::ofstream file(library, std::ios::binary); file << 'x'; }
        Diagnostics::LogEnvironment(load);  // Malformed input must not crash diagnostics.
        const std::array<std::uint32_t, 5> wrongHeader{99, 1, 6, 1179, 0};
        { std::ofstream file(library, std::ios::binary); file.write(reinterpret_cast<const char*>(wrongHeader.data()), sizeof(wrongHeader)); }
        Diagnostics::LogEnvironment(load);
        Diagnostics::LogLoadedModules();
        Diagnostics::Checkpoint("test checkpoint");
        Diagnostics::LogBytes("Unreadable test address", 1);

        auto* stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        Check(stub != nullptr, "stub allocation failed");
        struct Allocation { void* address; ~Allocation() { VirtualFree(address, 0, MEM_RELEASE); } } allocation{stub};
        stub[0] = 0xFF;
        stub[1] = 0x25;
        const auto target = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
        Check(target != 0, "test branch target unavailable");
        std::memcpy(stub + 6, &target, sizeof(target));
        const auto description = Diagnostics::DescribeBranchTarget(reinterpret_cast<std::uintptr_t>(stub));
        Check(description.find(" -> " + Diagnostics::DescribeAddress(target)) != std::string::npos,
            "anonymous trampoline was not traced to its DLL");
        Diagnostics::LogBytes("Test stub", reinterpret_cast<std::uintptr_t>(stub), 4096);
        stub[0] = 0xE9;
        const std::int32_t cycle = -5;
        std::memcpy(stub + 1, &cycle, sizeof(cycle));
        Check(Diagnostics::DescribeBranchTarget(reinterpret_cast<std::uintptr_t>(stub)).find(" -> ") == std::string::npos,
            "cyclic branch was followed");

        const auto contents = ReadFile(directory / "details/DietDrCamera.log");
        for (const auto* required : {"Skyrim 1.6.659.0", "GOG (runtime version)", "SKSE 2.2.3", "DDC PE timestamp=",
                "Address Library candidate", "unavailable", "format=1", "format=2", "format=5", "runtime matches=true",
                "unreadable/truncated header", "runtime matches=false", "unsupported format", "End snapshot",
                "[Startup] test checkpoint", "Unreadable test address 0x1 (unmapped)", "read 96/96 bytes"}) {
            if (contents.find(required) == std::string::npos) throw std::runtime_error(std::string("missing diagnostic: ") + required);
        }
        spdlog::debug("debug-must-be-off-by-default");
        Check(ReadFile(directory / "details/DietDrCamera.log").find("debug-must-be-off-by-default") == std::string::npos,
            "verbose logging was enabled by default");
        const auto workingLogger = spdlog::default_logger();
        { std::ofstream file(directory / "blocked"); file << "preserve-me"; }
        bool rejected{};
        try { Diagnostics::StartSession(directory / "blocked"); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && ReadFile(directory / "blocked") == "preserve-me" && spdlog::default_logger() == workingLogger,
            "failed log setup destroyed the existing logger or file");
        std::filesystem::current_path(originalCwd);
        spdlog::set_default_logger(originalLogger);
        spdlog::drop("DietDrCamera");
        // workingLogger keeps its file open until this scope ends; cleanup below
        // runs after that reference is released.
    } catch (const std::exception& error) {
        std::filesystem::current_path(originalCwd);
        spdlog::set_default_logger(originalLogger);
        spdlog::drop("DietDrCamera");
        std::cerr << error.what() << "\nEvidence retained at " << directory.string() << '\n';
        return 1;
    }
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    if (cleanupError) {
        std::cerr << "Test cleanup failed: " << cleanupError.message() << '\n';
        return 1;
    }
    std::cout << "Diagnostics passed: abrupt-exit persistence, three-session retention, startup environment, malformed libraries, modules, bounded stub tracing, and failed log setup\n";
    return 0;
}
