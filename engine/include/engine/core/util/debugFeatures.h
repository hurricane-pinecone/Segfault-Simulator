#pragma once

namespace sfs::debugFeatures
{

// The one switch for the engine's debug features (drawing colliders, and
// any later ones): what helps while a game is made, never in a game that
// ships. They work in the editor, in Debug builds and in development exports
// (`SfsEditor --export --development`, game.json "development": true); a
// release export has them off whatever its scenes or environment say.
//
// Two guards:
// - compiled: SFS_DEBUG_FEATURES (the CMake option of that name, on by
//   default). A build configured with it off compiles every debug feature
//   out; compiled() is then false and enabled() always is too.
// - allowed at run time: off in a release build until the app allows it.
//   The editor allows it; the player allows it only for a development
//   export. A Debug build allows it from the start.
//
// A debug feature checks enabled() (cheap: a load) before doing anything.

/** Whether debug features are compiled into this build. */
constexpr bool compiled()
{
#if SFS_DEBUG_FEATURES
  return true;
#else
  return false;
#endif
}

/** Allow (or forbid) debug features from now on. */
void allow(bool allowed);

/** Whether debug features may run: compiled in and allowed. */
bool enabled();

} // namespace sfs::debugFeatures
