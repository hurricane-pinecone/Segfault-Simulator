#include "platform/shell.h"

#include "engine/core/logger/logger.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <SDL_error.h>
  #include <SDL_misc.h>
#endif

#include <filesystem>
#include <string>

namespace shell
{

#if defined(_WIN32)

namespace
{
// ShellExecute succeeds with a value over 32.
bool succeeded(HINSTANCE result) { return reinterpret_cast<INT_PTR>(result) > 32; }
} // namespace

// Through the shell itself, with a Windows path: a file:// URL for a network
// share (\\wsl.localhost\...) isn't reliably opened.
void openExternally(const std::filesystem::path& path)
{
  const std::wstring target = std::filesystem::path(path).make_preferred().wstring();
  // A folder in Explorer, by name: "open" on a path with no extension runs a
  // program beside it of that name first (builds\windows\CozyFarm opened an
  // old builds\windows\CozyFarm.exe, not the folder).
  std::error_code error;
  if (std::filesystem::is_directory(path, error))
  {
    const std::wstring arguments = L"\"" + target + L"\"";
    if (!succeeded(ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL)))
      LOG_ERROR("Can't open " + path.string() + " in Explorer");
    return;
  }
  if (!succeeded(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)))
    LOG_ERROR("Can't open " + path.string() + " (no app opens it?)");
}

void showInFolder(const std::filesystem::path& file)
{
  const std::wstring arguments = L"/select,\"" + std::filesystem::path(file).make_preferred().wstring() + L"\"";
  if (!succeeded(ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL)))
    LOG_ERROR("Can't show " + file.string() + " in Explorer");
}

#else

namespace
{
// A path as a file:// URL for SDL_OpenURL.
std::string fileUrl(const std::filesystem::path& path)
{
  std::string generic = path.generic_string();
  if (!generic.starts_with("/"))
    generic.insert(0, "/");
  return "file://" + generic;
}
} // namespace

void openExternally(const std::filesystem::path& path)
{
  if (SDL_OpenURL(fileUrl(path).c_str()) != 0)
    LOG_ERROR("Can't open " + path.string() + ": " + SDL_GetError());
}

// File browsers differ in how (or whether) they select a file: its folder.
void showInFolder(const std::filesystem::path& file) { openExternally(file.parent_path()); }

#endif

} // namespace shell
