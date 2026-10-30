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

extern "C" SFS_MODULE_EXPORT void
sfsRegisterModule(sfs::reflect::TypeRegistry& registry);

// The engine version a module was built against, as a string that differs
// whenever the two can't work together. Bump the number when a change to the
// engine's headers changes what a module compiled against them expects (a
// class's layout or virtual functions, an inline function's behaviour).
#define SFS_ENGINE_ABI_NUMBER 2

#define SFS_ABI_STRINGIFY2(x) #x
#define SFS_ABI_STRINGIFY(x) SFS_ABI_STRINGIFY2(x)
#if defined(_MSC_VER) && !defined(__clang__)
  // MSVC's debug and release standard libraries differ in layout.
  #define SFS_ABI_COMPILER                                                     \
    "msvc-" SFS_ABI_STRINGIFY(_MSC_VER) "-" SFS_ABI_STRINGIFY(                 \
        _ITERATOR_DEBUG_LEVEL)
#elif defined(__clang__)
  #define SFS_ABI_COMPILER "clang-" SFS_ABI_STRINGIFY(__clang_major__)
#elif defined(__GNUC__)
  #define SFS_ABI_COMPILER "gcc-" SFS_ABI_STRINGIFY(__GNUC__)
#else
  #define SFS_ABI_COMPILER "unknown"
#endif

namespace sfs
{
/** Exported name of the module entry point. */
inline constexpr const char* kModuleEntryPoint = "sfsRegisterModule";

/** Signature of the module entry point. */
using ModuleEntryPoint = void (*)(reflect::TypeRegistry&);

/**
 * The engine ABI this engine (or a module built against its headers) has.
 * A module reports its own through kModuleAbiEntryPoint; one whose ABI isn't
 * the loading engine's is refused (rebuild it against this engine).
 */
inline constexpr const char* kEngineAbi =
    "sfs-" SFS_ABI_STRINGIFY(SFS_ENGINE_ABI_NUMBER) "-" SFS_ABI_COMPILER;

/** Exported name of the function a module reports its engine ABI with. */
inline constexpr const char* kModuleAbiEntryPoint = "sfsModuleEngineAbi";

/** Signature of that function. */
using ModuleAbiEntryPoint = const char* (*)();
} // namespace sfs

// A game module's report of the engine ABI it was built against: defined once
// in every module by SFS_DEFINE_MODULE_ENGINE_ABI() (sfs_add_game_module
// generates the file that does). Not inline: an exported inline function
// isn't emitted where nothing calls it (MinGW's -fno-keep-inline-dllexport).
extern "C" SFS_MODULE_EXPORT const char* sfsModuleEngineAbi();
#define SFS_DEFINE_MODULE_ENGINE_ABI()                                         \
  extern "C" SFS_MODULE_EXPORT const char* sfsModuleEngineAbi()                \
  {                                                                            \
    return sfs::kEngineAbi;                                                    \
  }
