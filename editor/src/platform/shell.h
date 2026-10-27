#pragma once

#include <filesystem>

namespace shell
{

/** Open a file or folder in the system's default application (logged on failure). */
void openExternally(const std::filesystem::path& path);

} // namespace shell
