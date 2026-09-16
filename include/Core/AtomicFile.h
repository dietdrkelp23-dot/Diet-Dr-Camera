#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace DietDrCamera
{
    bool WriteFileAtomically(const std::filesystem::path& a_path, std::string_view a_contents,
                             bool a_replaceExisting, std::string& a_error);
}
