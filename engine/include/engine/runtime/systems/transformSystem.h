#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/system.h"

#include <cstdint>
#include <vector>

namespace sfs
{

struct WorldTransformComponent;

/**
 * The transform pass: keeps each entity's world matrix cached in a
 * WorldTransformComponent, which it gives every entity with a
 * TransformComponent (and takes away with it).
 *
 * Every Scene adds it first, so its render() opens the render phase: after
 * every update, game object update and late update, before anything is drawn.
 * It draws nothing. Once a frame it walks the hierarchy's preorder index from
 * the roots down, in one linear pass, and recomposes
 * (hierarchy::composeWorld()) only the caches whose local transform, nearest
 * ancestor with a transform, or that ancestor's world matrix changed; each one
 * it changes gets this pass's number as its version.
 *
 * Memory: the caches (72 bytes an entity with a transform, in its component
 * pool slot) and a scratch of 16 bytes per living entity (a preorder entry's
 * nearest cache and its id), reused. Nothing is allocated a
 * frame once the scratch has grown with the entity count. Cost: a hash of
 * each transform and a pass over the preorder index, plus a composition for
 * each changed cache.
 *
 * An entity made since the last flush has no cache yet: the caches below it
 * are marked not made (version 0) until the next pass after the flush, and
 * read fresh until then.
 */
class TransformSystem : public System
{
protected:
  void create() override;
  void render() override;
  void onEntityAdded(const Entity& entity) override;
  void onEntityRemoved(const Entity& entity) override;

private:
  // The nearest entity at or above a preorder entry with a transform: its
  // cache (null when it has none yet) and its id (kNone when there is none).
  struct Above
  {
    WorldTransformComponent* cache = nullptr;
    Entity::EntityId id = Entity::InvalidId;
  };

  // The SFS_CHECK_TRANSFORMS check after the pass: every made cache against
  // one composed afresh.
  void checkCaches();

  // By preorder position, rewritten each pass.
  std::vector<Above> m_above;
  // The number of the next pass; never 0, which marks a cache not made.
  uint32_t m_pass = 1;
};

} // namespace sfs
