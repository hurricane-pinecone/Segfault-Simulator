#include "platform/shell.h"

#include "engine/core/logger/logger.h"

#include <SDL_error.h>
#include <SDL_misc.h>

#include <string>

namespace shell
{

namespace
{
// A path as a file:// URL for SDL_OpenURL (handles drive letters and UNC
// shares such as \\wsl.localhost\...).
std::string fileUrl(const std::filesystem::path& path)
{
  std::string generic = path.generic_string();
  if (generic.starts_with("//"))
    return "file:" + generic; // UNC: file://host/share/...
  if (!generic.starts_with("/"))
    generic.insert(0, "/"); // C:/... -> /C:/...
  return "file://" + generic;
}
} // namespace

void openExternally(const std::filesystem::path& path)
{
  if (SDL_OpenURL(fileUrl(path).c_str()) != 0)
    LOG_ERROR("Can't open " + path.string() + ": " + SDL_GetError());
}

} // namespace shell
