#pragma once

#include <cstdint>

namespace sfs
{

/**
 * A scene-owned entity's id in its scene file, given only to entities that an
 * entity reference elsewhere in the scene names: saved as the entry's
 * "$sceneId", it is what a scene-absolute reference ("/7") points at (see
 * SceneSerializer). Handed out when the scene (or a copy of a branch) is
 * saved, a random 31-bit number not in use in the scene, so entries added in
 * two branches of a scene don't clash when merged; kept from then on. Not
 * reflected: the serializer reads and writes it.
 */
struct SceneIdComponent
{
  /** The id, unique in the scene; 0 is never given. */
  uint32_t id = 0;
};

} // namespace sfs
