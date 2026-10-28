#include "platform/fileDialog.h"

#include "engine/core/logger/logger.h"

#include <SDL_syswm.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shobjidl.h>
#else
  #include <cstdio>
  #include <cstdlib>
#endif

#include <string>

namespace fileDialog
{

#if defined(_WIN32)

namespace
{
std::wstring widen(const std::string& text)
{
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
  if (size > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
  return wide;
}

std::string narrow(const wchar_t* text)
{
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size - 1 : 0, '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

HWND nativeWindow(SDL_Window* window)
{
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  return window && SDL_GetWindowWMInfo(window, &info) ? info.info.win.window : nullptr;
}
} // namespace

std::optional<std::filesystem::path> pickFolder(SDL_Window* owner,
                                                const char* title,
                                                const std::filesystem::path& start)
{
  // The dialog is a COM object; this thread may not have COM yet.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  std::optional<std::filesystem::path> chosen;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog))))
  {
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(widen(title).c_str());

    std::error_code error;
    if (!start.empty() && std::filesystem::is_directory(start, error))
    {
      IShellItem* folder = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(widen(start.string()).c_str(), nullptr,
                                                IID_PPV_ARGS(&folder))))
      {
        dialog->SetFolder(folder);
        folder->Release();
      }
    }

    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(nativeWindow(owner))) && SUCCEEDED(dialog->GetResult(&result)))
    {
      PWSTR path = nullptr;
      if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
      {
        chosen = std::filesystem::path(narrow(path));
        CoTaskMemFree(path);
      }
      result->Release();
    }
    dialog->Release();
  }
  else
    LOG_ERROR("Can't open the folder picker");

  if (SUCCEEDED(com))
    CoUninitialize();
  return chosen;
}

#else // Linux: whichever desktop dialog tool is installed

namespace
{
std::string quote(const std::string& text)
{
  std::string out = "'";
  for (const char c : text)
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

std::optional<std::string> run(const std::string& command)
{
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe)
    return std::nullopt;
  std::string output;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe))
    output += buffer;
  if (pclose(pipe) != 0)
    return std::nullopt; // cancelled
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  return output;
}
} // namespace

std::optional<std::filesystem::path> pickFolder(SDL_Window*,
                                                const char* title,
                                                const std::filesystem::path& start)
{
  const std::string folder = start.empty() ? std::string(".") : start.string();
  if (std::system("command -v zenity > /dev/null 2>&1") == 0)
  {
    if (auto out = run("zenity --file-selection --directory --title=" + quote(title) +
                       " --filename=" + quote(folder + "/") + " 2>/dev/null"))
      return std::filesystem::path(*out);
    return std::nullopt;
  }
  if (std::system("command -v kdialog > /dev/null 2>&1") == 0)
  {
    if (auto out = run("kdialog --getexistingdirectory " + quote(folder) + " --title " +
                       quote(title) + " 2>/dev/null"))
      return std::filesystem::path(*out);
    return std::nullopt;
  }
  LOG_ERROR("No folder picker available: install zenity or kdialog, or type the folder");
  return std::nullopt;
}

#endif

} // namespace fileDialog
