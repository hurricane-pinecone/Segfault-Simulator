#include "engine/runtime/scripting/moduleHost.h"

#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/scripting/gameModule.h"
#include "engine/runtime/scripting/scriptRegistry.h"

#include <SDL_loadso.h>
#include <SDL_timer.h>

#include <string>
#include <system_error>

namespace sfs
{

namespace
{
std::filesystem::path copyFolder()
{
  return std::filesystem::temp_directory_path() / "sfs-editor" / "modules";
}
} // namespace

bool ModuleHost::load(const std::filesystem::path& module,
                      const std::string& moduleName,
                      Load mode)
{
  unload();

  std::error_code error;
  std::filesystem::path copy;
  if (mode == Load::FromCopy)
  {
    std::filesystem::create_directories(copyFolder(), error);
    // A unique name per load: the previous copy may still be mapped briefly,
    // and the OS caches libraries by path.
    copy = copyFolder() /
           (moduleName + "-" + std::to_string(SDL_GetPerformanceCounter()) +
            module.extension().string());
    std::filesystem::copy_file(
        module, copy, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
      LOG_ERROR("Can't copy " + module.string() + ": " + error.message());
      return false;
    }
  }
  const std::filesystem::path& loadPath = copy.empty() ? module : copy;

  void* handle = SDL_LoadObject(loadPath.string().c_str());
  if (!handle)
  {
    LOG_ERROR(std::string("Can't load the game module: ") + SDL_GetError());
    if (!copy.empty())
      std::filesystem::remove(copy, error);
    return false;
  }

  // Built against this engine? Its types' layouts and the engine's must
  // agree, or the first call into it can crash.
  const auto abi = reinterpret_cast<sfs::ModuleAbiEntryPoint>(
      SDL_LoadFunction(handle, sfs::kModuleAbiEntryPoint));
  const std::string moduleAbi = abi && abi() ? abi() : "";
  if (moduleAbi != sfs::kEngineAbi)
  {
    LOG_ERROR("The game module " + module.filename().string() +
              " was built for a different engine (" +
              (moduleAbi.empty() ? std::string("an older one") : moduleAbi) +
              "; this is " + sfs::kEngineAbi +
              "): rebuild it against this engine");
    SDL_UnloadObject(handle);
    if (!copy.empty())
      std::filesystem::remove(copy, error);
    return false;
  }

  const auto entry = reinterpret_cast<sfs::ModuleEntryPoint>(
      SDL_LoadFunction(handle, sfs::kModuleEntryPoint));
  if (!entry)
  {
    LOG_ERROR("The game module has no entry point (" +
              std::string(sfs::kModuleEntryPoint) + ")");
    SDL_UnloadObject(handle);
    if (!copy.empty())
      std::filesystem::remove(copy, error);
    return false;
  }

  entry(sfs::reflect::types());

  m_handle = handle;
  m_moduleName = moduleName;
  m_copy = copy;
  if (!m_quiet)
    LOG_INFO("Loaded game module " + moduleName);
  return true;
}

void ModuleHost::unload()
{
  if (!m_handle)
    return;

  // Loaded scriptable objects may be of the module's types (their
  // destructors are its code).
  sfs::scriptableObjects().clearModule(m_moduleName);
  sfs::reflect::types().removeModule(m_moduleName);
  sfs::scripts().removeModule(m_moduleName);
  SDL_UnloadObject(m_handle);
  m_handle = nullptr;

  if (!m_copy.empty())
  {
    std::error_code error;
    std::filesystem::remove(
        m_copy, error); // best effort; temp is cleaned anyway
    m_copy.clear();
  }
}

} // namespace sfs
