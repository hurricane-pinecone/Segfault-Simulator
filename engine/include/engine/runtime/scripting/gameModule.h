#pragma once

#include "engine/core/reflection/reflection.h"

// The entry point of a game module: the shared library sfs_add_game_module()
// builds from a project's scripts. sfs-codegen generates its definition, which
// registers the module's components (reflect::types()) and its systems and game
// objects (scripts()). The editor looks it up by name after loading the module.

#if defined(_WIN32)
  #define SFS_MODULE_EXPORT __declspec(dllexport)
#else
  #define SFS_MODULE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SFS_MODULE_EXPORT void sfsRegisterModule(sfs::reflect::TypeRegistry& registry);

namespace sfs
{
/** Exported name of the module entry point. */
inline constexpr const char* kModuleEntryPoint = "sfsRegisterModule";

/** Signature of the module entry point. */
using ModuleEntryPoint = void (*)(reflect::TypeRegistry&);
} // namespace sfs
