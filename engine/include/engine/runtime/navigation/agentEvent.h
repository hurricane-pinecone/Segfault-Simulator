#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/navigation/fleet.h"

namespace sfs::nav
{

/**
 * News from NavigationSystem about one of its agents, sent with the agent's
 * entity as it happens during the system's update: it arrived, it's proven
 * blocked, it began giving way or went on, or its game's path can't go on
 * and needs planning again. Consumers read it with `read()` (every one, in
 * order); a newer one about the same agent makes the older stale for
 * `readLatest()`.
 */
struct [[sfs::event_entity_news]] AgentEvent
{
  /**
   * What happened: its kind, the request it's about and what blocks it
   * (Fleet::Event). Its `other` is the fleet's id: read `other` below.
   */
  Fleet::Event what;
  /**
   * The other agent it's about (whose way it gives way to, whose stop it
   * waits for), to ask the navigation about (navigation().pose(other));
   * invalid when it names none.
   */
  Entity other;
};

} // namespace sfs::nav
