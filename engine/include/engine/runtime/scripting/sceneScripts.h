#pragma once

#include <string>
#include <vector>

namespace sfs
{

/**
 * The scripts a scene file lists by name: its game systems and services,
 * saved under "systems" and "services" and started by startScripts() as the
 * scene plays.
 */
struct SceneScripts
{
  /**
   * Its game systems, in the order they're added (which sets when each hears
   * events).
   */
  std::vector<std::string> systems;
  /** Its services; their order means nothing. */
  std::vector<std::string> services;
};

} // namespace sfs
