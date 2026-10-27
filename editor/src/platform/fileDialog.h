#pragma once

#include <filesystem>
#include <optional>

struct SDL_Window;

namespace fileDialog
{

/**
 * Ask the user for a folder with the system's own folder picker (Explorer's on
 * Windows; zenity or kdialog on Linux). Blocks until it closes.
 *
 * @param owner the window the dialog belongs to (may be null)
 * @param start the folder it opens in (may be empty)
 * @return the chosen folder, or nothing if cancelled or unavailable (logged)
 */
std::optional<std::filesystem::path> pickFolder(SDL_Window* owner,
                                                const char* title,
                                                const std::filesystem::path& start);

} // namespace fileDialog
