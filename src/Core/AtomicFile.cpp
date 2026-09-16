#include "Core/AtomicFile.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <system_error>

namespace DietDrCamera
{
    bool WriteFileAtomically(const std::filesystem::path& a_path, std::string_view a_contents,
                             bool a_replaceExisting, std::string& a_error)
    {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        std::filesystem::create_directories(a_path.parent_path(), error);
        if (error) {
            a_error = error.message();
            return false;
        }
        std::filesystem::path temporary;
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 32; ++attempt) {
            temporary = a_path;
            temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            const DWORD failure = GetLastError();
            if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS) {
                a_error = std::system_category().message(failure);
                return false;
            }
        }
        if (file == INVALID_HANDLE_VALUE) {
            a_error = "Could not create a unique temporary file";
            return false;
        }
        DWORD failure = ERROR_SUCCESS;
        std::size_t offset = 0;
        while (offset < a_contents.size()) {
            const DWORD length = static_cast<DWORD>((std::min)(a_contents.size() - offset,
                                                               std::size_t{1024 * 1024}));
            DWORD written = 0;
            if (!WriteFile(file, a_contents.data() + offset, length, &written, nullptr)) {
                failure = GetLastError();
                break;
            }
            if (written == 0) {
                failure = ERROR_WRITE_FAULT;
                break;
            }
            offset += written;
        }
        if (failure == ERROR_SUCCESS && !FlushFileBuffers(file)) failure = GetLastError();
        if (!CloseHandle(file) && failure == ERROR_SUCCESS) failure = GetLastError();
        if (failure == ERROR_SUCCESS) {
            const DWORD flags = MOVEFILE_WRITE_THROUGH |
                                (a_replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
            if (MoveFileExW(temporary.c_str(), a_path.c_str(), flags)) return true;
            failure = GetLastError();
        }
        DeleteFileW(temporary.c_str());
        a_error = std::system_category().message(failure);
        return false;
    }
}
