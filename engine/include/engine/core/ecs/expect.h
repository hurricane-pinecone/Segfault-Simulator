#pragma once

#include "engine/core/ecs/component.h"
#include "engine/core/ecs/entity.h"

#include <source_location>
#include <string_view>

namespace sfs
{

namespace detail
{
/**
 * What a failing sfs::expect() does, once per call site per run (later
 * failures there return at once): logs an error naming the call site, the
 * entity and the component type. In development (debugFeatures::enabled())
 * it then aborts if the environment sets SFS_EXPECT_ABORT=1; otherwise it
 * breaks into an attached debugger and shows the host's error dialog
 * (showErrorDialog(): never in a run that sets SFS_NO_ERROR_DIALOG), and the
 * code carries on with its fallback once it's dismissed.
 * @param where the call site
 * @param entity the entity it was looked up on
 * @param type the component's type key (componentTypeKey())
 */
void expectFailed(const std::source_location& where,
                  const Entity& entity,
                  std::string_view type);

/** Forgets which call sites have failed, so each reports again (for tests). */
void forgetExpectFailures();
} // namespace detail

/**
 * Whether a component the code needs is there, as Unreal's ensure: loud in
 * development, a log line and a fallback in release, never a crash (unless
 * asked for). When `component` is null it returns false, so the code takes
 * its fallback: `if (!sfs::expect(hitch, vehicle)) return;`. For lookups the
 * scene's data should always satisfy: a prefab's own components, a
 * relationship's other side.
 *
 * The first failure at each call site in a run logs an error naming it, the
 * entity and the type ("HitchSystem.cpp:122: entity 812 has no Hitch"); later
 * ones there say nothing more. In development and editor builds (the debug
 * guard, debugFeatures::enabled()) that first failure also breaks into an
 * attached debugger and shows the engine's error dialog, which the game
 * carries on after (none in a run that sets SFS_NO_ERROR_DIALOG, where it
 * stays a log line); with SFS_EXPECT_ABORT=1 it aborts instead, for anyone
 * who wants development to crash on it. A passing check is one branch, and
 * the message is built only on failure.
 * @param component a getComponent<TComponent>() result
 * @param entity the entity it was looked up on
 * @param where the call site (leave it to the default)
 * @return whether `component` is non-null
 */
template <typename TComponent>
bool expect(const TComponent* component,
            const Entity& entity,
            std::source_location where = std::source_location::current())
{
  if (component) [[likely]]
    return true;
  detail::expectFailed(where, entity, detail::componentTypeKey<TComponent>());
  return false;
}

} // namespace sfs
