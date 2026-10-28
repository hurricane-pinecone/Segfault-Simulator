#pragma once

#include <filesystem>

namespace shell
{

/** Open a file or folder in the system's default application (logged on failure). */
void openExternally(const std::filesystem::path& path);

/**
 * Show a file in the system's file browser: its folder, with it selected
 * where the browser can (Windows Explorer). Logged on failure.
 */
void showInFolder(const std::filesystem::path& file);

} // namespace shell
