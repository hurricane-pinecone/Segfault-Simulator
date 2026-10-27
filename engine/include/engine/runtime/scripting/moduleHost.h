#pragma once

#include <filesystem>
#include <string>

namespace sfs
{

/**
 * Loads a game module (the library built from a project's scripts) and unloads
 * it again. Loading calls the module's entry point, registering its types and
 * scripts; unloading unregisters them first.
 *
 * Everything the module created (component storage, game objects, systems)
 * must be destroyed before unload(): that code lives in the module.
 */
class ModuleHost
{
public:
  ModuleHost() = default;
  ~ModuleHost() { unload(); }

  ModuleHost(const ModuleHost&) = delete;
  ModuleHost& operator=(const ModuleHost&) = delete;

  enum class Load
  {
    InPlace,
    // From a private copy in the temp folder: the build output stays
    // replaceable (a loaded library is locked on Windows) and a local copy
    // loads faster than one on a network or WSL share. For editors that
    // rebuild and reload.
    FromCopy,
  };

  /**
   * Load `module` and register its types under `moduleName`.
   *
   * @return false (logged) if it can't be loaded or has no entry point.
   */
  bool load(const std::filesystem::path& module,
            const std::string& moduleName,
            Load mode = Load::InPlace);

  /** Unregister the module's types and unload it. No-op when none is loaded. */
  void unload();

  bool loaded() const { return m_handle != nullptr; }

private:
  void* m_handle = nullptr;
  std::string m_moduleName;
  std::filesystem::path m_copy;
};

} // namespace sfs
