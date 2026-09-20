#include "PCH.h"
#include "Core/Diagnostics.h"
#include "Core/Version.h"
#include "Hooks/RuntimeVersion.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace DietDrCamera::Diagnostics
{
    namespace
    {
        std::filesystem::path logPath;

        std::string Utf8(const std::filesystem::path& path)
        {
            const auto text = path.u8string();
            return {reinterpret_cast<const char*>(text.data()), text.size()};
        }

        std::filesystem::path ModulePath(HMODULE module)
        {
            std::wstring buffer(32768, L'\0');
            const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length || length == buffer.size()) return {};
            buffer.resize(length);
            return buffer;
        }

        template <class T>
        bool Read(std::uintptr_t address, T& value)
        {
            SIZE_T read{};
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                &value, sizeof(value), &read) && read == sizeof(value);
        }

        std::string FileVersion(const std::filesystem::path& path)
        {
            DWORD unused{};
            const auto size = GetFileVersionInfoSizeW(path.c_str(), &unused);
            if (!size || size > 1024 * 1024) return "unavailable";
            std::vector<std::uint8_t> buffer(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return "unavailable";
            VS_FIXEDFILEINFO* info{};
            UINT length{};
            if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
                length < sizeof(*info) || info->dwSignature != 0xFEEF04BD) return "unavailable";
            return fmt::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }

        void LogBuildIdentity(HMODULE module)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            if (!Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
                !Read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
                spdlog::warn("[Diagnostics] DDC build identity unavailable");
                return;
            }
            spdlog::info("[Environment] DDC PE timestamp=0x{:08X}, image bytes={}",
                nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage);
            const auto debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if (debug.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
                debug.Size > nt.OptionalHeader.SizeOfImage - debug.VirtualAddress) return;
            for (std::size_t i = 0; i < std::min<std::size_t>(debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY), 64); ++i) {
                IMAGE_DEBUG_DIRECTORY entry{};
                struct CodeView { std::uint32_t signature; GUID guid; std::uint32_t age; } cv{};
                if (!Read(base + debug.VirtualAddress + i * sizeof(entry), entry) ||
                    entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < sizeof(cv) ||
                    entry.AddressOfRawData >= nt.OptionalHeader.SizeOfImage ||
                    sizeof(cv) > nt.OptionalHeader.SizeOfImage - entry.AddressOfRawData ||
                    !Read(base + entry.AddressOfRawData, cv) || cv.signature != 0x53445352) continue;
                const auto& g = cv.guid;
                spdlog::info("[Environment] DDC PDB identity={:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}, age={}",
                    g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                    g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], cv.age);
                return;
            }
            spdlog::warn("[Diagnostics] DDC PDB identity unavailable");
        }

        void LogAddressLibrary(REL::Version runtime)
        {
            const std::filesystem::path path = std::filesystem::path("Data/SKSE/Plugins") /
                fmt::format("{}-{}.bin", runtime >= REL::Version{1, 6, 0, 0} ? "versionlib" : "version", runtime.string());
            // Match CommonLib's relative path exactly, including the mod manager's
            // virtual filesystem. This is a header observation, not a loader replacement.
            std::error_code error;
            const auto bytes = std::filesystem::file_size(path, error);
            if (error) {
                spdlog::warn("[Environment] Address Library candidate {}: unavailable ({})", Utf8(path), error.message());
                return;
            }
            std::array<std::uint32_t, 5> header{};
            std::ifstream file(path, std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header))) {
                spdlog::warn("[Environment] Address Library candidate {}: unreadable/truncated header, bytes={}", Utf8(path), bytes);
                return;
            }
            const bool matches = header[1] == runtime[0] && header[2] == runtime[1] &&
                header[3] == runtime[2] && header[4] == runtime[3];
            spdlog::info("[Environment] Address Library candidate {}: bytes={}, format={}, header runtime={}.{}.{}.{}, runtime matches={}",
                Utf8(path), bytes, header[0], header[1], header[2], header[3], header[4], matches);
            if (!matches || (header[0] != 1 && header[0] != 2 && header[0] != 5))
                spdlog::warn("[Environment] Address Library header mismatch or unsupported format; CommonLib will perform full validation");
        }
    }

    void StartSession(const std::filesystem::path& directory)
    {
        const auto path = directory / "DietDrCamera.log";
        // Rotate only at launch: preserve the startup header throughout even a
        // long verbose session. Ordinary per-frame output remains debug-only.
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path.string(), std::numeric_limits<std::size_t>::max(), 3, true);
        auto logger = std::make_shared<spdlog::logger>("DietDrCamera", std::move(sink));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logPath = path;
        spdlog::set_default_logger(std::move(logger));
        spdlog::info("[Session] Diet Dr Camera v" DDC_VERSION_STRING " | process {} | new session", GetCurrentProcessId());
        spdlog::info("[Session] Log: {} | previous sessions: DietDrCamera.1.log through DietDrCamera.3.log", LogLocation());
        spdlog::info("[Support] For startup problems send this complete log, skse64.log, and the full error message. Include a Crash Logger report for a crash. Verbose Logging is not needed for startup diagnostics.");
    }

    bool InitializeLog()
    {
        std::string reason;
        try {
            const auto directory = SKSE::log::log_directory();
            if (!directory) throw std::runtime_error("SKSE log directory is unavailable");
            StartSession(*directory);
            return true;
        } catch (const std::exception& error) { reason = error.what(); }
        try {
            StartSession(std::filesystem::temp_directory_path() / "DietDrCamera");
            spdlog::warn("[Session] Using a temporary log because the SKSE log directory failed: {}", reason);
            return true;
        } catch (const std::exception& error) {
            const auto message = fmt::format("Diet Dr Camera could not create its diagnostic log.\nSKSE log: {}\nTemporary log: {}\nThe plugin was not initialized.", reason, error.what());
            MessageBoxA(nullptr, message.c_str(), "Diet Dr Camera - logging unavailable", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    std::string LogLocation()
    {
        return logPath.empty() ? "DietDrCamera.log in the SKSE log directory" : Utf8(logPath);
    }

    void LogEnvironment(const SKSE::LoadInterface& skse)
    {
        const auto runtime = skse.RuntimeVersion();
        const auto packed = skse.SKSEVersion();
        const auto store = runtime == REL::Version{1,6,659,0} || runtime == REL::Version{1,6,1179,0} ?
            "GOG (runtime version)" : RuntimeVersion::IsKnown({runtime[0], runtime[1], runtime[2], runtime[3]}) ?
            "Steam (runtime version)" : "unknown distribution";
        spdlog::info("[Environment] Skyrim {}.{}.{}.{} | {} | SKSE {}.{}.{} (packed 0x{:08X}) | editor={}",
            runtime[0], runtime[1], runtime[2], runtime[3], store,
            (packed >> 24) & 0xFF, (packed >> 16) & 0xFF, (packed >> 4) & 0xFFF, packed, skse.IsEditor());
        spdlog::info("[Environment] Executable: {}", Utf8(ModulePath(nullptr)));
        std::error_code error;
        const auto cwd = std::filesystem::current_path(error);
        spdlog::info("[Environment] Working directory: {}", error ? error.message() : Utf8(cwd));
        HMODULE self{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&logPath), &self)) {
            spdlog::info("[Environment] DDC module: {} | file version={}", Utf8(ModulePath(self)), FileVersion(ModulePath(self)));
            LogBuildIdentity(self);
        }
        LogAddressLibrary(runtime);
    }

    void LogLoadedModules()
    {
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3; ++attempt) {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        }
        if (snapshot == INVALID_HANDLE_VALUE) {
            spdlog::warn("[Modules] Cannot enumerate loaded DLLs: Windows error {}", GetLastError());
            return;
        }
        struct SnapshotOwner { HANDLE handle; ~SnapshotOwner() { CloseHandle(handle); } } owner{snapshot};
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::vector<MODULEENTRY32W> entries;
        if (Module32FirstW(snapshot, &entry)) {
            do { entries.push_back(entry); } while (Module32NextW(snapshot, &entry));
        }
        const auto error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) spdlog::warn("[Modules] Enumeration incomplete: Windows error {}", error);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.szModule, b.szModule) < 0; });
        spdlog::info("[Modules] Loaded module snapshot before DDC hooks: {} entries; presence alone does not establish a conflict", entries.size());
        for (const auto& module : entries) {
            spdlog::info("[Modules] {} | file version={} | base=0x{:X} | image bytes={}",
                Utf8(module.szModule), FileVersion(module.szExePath),
                reinterpret_cast<std::uintptr_t>(module.modBaseAddr), module.modBaseSize);
        }
        spdlog::info("[Modules] End snapshot");
    }

    void Checkpoint(std::string_view step)
    {
        spdlog::info("[Startup] {}", step);
    }

    std::string DescribeAddress(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return fmt::format("0x{:X} (unmapped)", address);
        const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
        if (info.Type == MEM_IMAGE) {
            const auto path = ModulePath(static_cast<HMODULE>(info.AllocationBase));
            if (!path.empty()) return fmt::format("{}+0x{:X}", Utf8(path.filename()), address - base);
        }
        // Also handles privately mapped game images in the offline checker.
        if (base == REL::Module::get().base()) return fmt::format("Skyrim image+0x{:X}", address - base);
        return fmt::format("0x{:X} (allocation 0x{:X}, protection 0x{:X})", address, base, info.Protect);
    }

    void LogBytes(std::string_view label, std::uintptr_t address, std::size_t count)
    {
        std::array<std::uint8_t, 96> bytes{};
        count = std::min(count, bytes.size());
        SIZE_T read{};
        const auto success = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), bytes.data(), count, &read);
        const auto error = success ? ERROR_SUCCESS : GetLastError();
        std::string hex;
        for (std::size_t i = 0; i < read; ++i) hex += fmt::format("{:02X} ", bytes[i]);
        spdlog::critical("[Runtime] {} {}: {} (read {}/{} bytes, Windows error {})",
            label, DescribeAddress(address), hex, read, count, error);
    }

    std::string DescribeBranchTarget(std::uintptr_t address)
    {
        auto description = DescribeAddress(address);
        std::array<std::uintptr_t, 4> visited{address};
        // SKSE trampolines often live in anonymous executable allocations.
        // Follow only these recognizable jump stubs, with bounded safe reads;
        // no callback is executed and an unknown stub remains unidentified.
        for (std::size_t depth = 1; depth < visited.size(); ++depth) {
            std::array<std::uint8_t, 16> bytes{};
            if (!Read(address, bytes)) break;
            std::uintptr_t next{};
            if (bytes[0] == 0xE9 || (bytes[0] == 0xFF && bytes[1] == 0x25)) {
                std::int32_t displacement{};
                const bool indirect = bytes[0] == 0xFF;
                std::memcpy(&displacement, bytes.data() + (indirect ? 2 : 1), sizeof(displacement));
                next = address + (indirect ? 6 : 5) + displacement;
                if (indirect && !Read(next, next)) break;
            } else if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
                std::memcpy(&next, bytes.data() + 2, sizeof(next));
            } else break;
            if (!next || std::find(visited.begin(), visited.begin() + depth, next) != visited.begin() + depth) break;
            description += " -> " + DescribeAddress(next);
            visited[depth] = next;
            address = next;
        }
        return description;
    }
}
