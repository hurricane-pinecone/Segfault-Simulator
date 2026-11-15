#pragma once

#include <string_view>

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

/**
 * Whether the transform check runs: after the transform pass, every cached
 * world matrix (WorldTransformComponent) is compared with one composed afresh,
 * as is each cached read in the render phase (hierarchy::cachedWorldMatrix());
 * a mismatch is logged once per entity. It catches a transform written after
 * the pass, and any bug in the cache. Off unless the environment sets
 * SFS_CHECK_TRANSFORMS=1 or setCheckTransforms() turns it on, and always off
 * when enabled() is false. Costs a full recompute a frame while on.
 */
bool checkTransforms();

/** Turn the transform check (checkTransforms()) on or off, as tests do. */
void setCheckTransforms(bool on);

/**
 * Whether the sends of an event type are logged (sfs::Events: each send's
 * type, number and frame, and a worker's with its thread). The environment's
 * SFS_LOG_EVENTS lists the types, comma separated, by name with or without
 * their namespaces (`DamageTaken,AgentEvent`), or `*` for every type;
 * setLogEvents() replaces the list. Asked as a type's queue is made, so a
 * change applies to queues made after it. Always false when enabled() is
 * false.
 * @param typeName the event type's name, for people ("game::DamageTaken")
 * @return whether its sends are logged
 */
bool logEvents(std::string_view typeName);

/**
 * Replace the list of event types logEvents() logs, as SFS_LOG_EVENTS would
 * (tests do).
 * @param types the types, comma separated, or `*`; empty: none
 */
void setLogEvents(std::string_view types);

} // namespace sfs::debugFeatures
