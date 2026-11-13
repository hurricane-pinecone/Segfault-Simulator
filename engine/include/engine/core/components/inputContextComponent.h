#pragma once

#include <string>
#include <vector>

namespace sfs
{

/**
 * Claims keys for the game object on its entity while it is active, as
 * Unreal's input mapping contexts do: driving claims the drive keys, a build
 * mode its own. Each frame, before the objects see input, a key claimed by an
 * active context is withheld from every other object whose own context ranks
 * lower (it reads as untouched there), and from every object with no active
 * context. Its own object still gets it, and contexts of equal priority share
 * their keys. The scene reads the contexts as they stand after the last
 * frame, so turning one on or off takes effect from the next frame. The mouse
 * is not affected (the UI's hold on it is separate).
 *
 * @param std::vector<std::string> keys - the keys it claims, by name as the
 *        sfs::Key enumerators are spelled ("Space", "Up", "F8", "Num1");
 *        unknown names are logged once and skipped
 * @param int priority - higher wins a shared key; contexts of equal priority
 *        share it
 * @param bool active - whether it claims its keys now
 */
struct InputContextComponent
{
  std::vector<std::string> keys;
  int priority = 0;
  bool active = true;

  /** An unknown key name has been logged (the scene's). */
  [[sfs::hide]] bool loggedUnknown = false;
};

} // namespace sfs
