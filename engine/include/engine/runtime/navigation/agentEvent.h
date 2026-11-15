#pragma once

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
   * What happened: its kind, the request it's about, the other agent and
   * what blocks it (Fleet::Event).
   */
  Fleet::Event what;
};

} // namespace sfs::nav
