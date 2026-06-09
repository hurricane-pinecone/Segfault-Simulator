#pragma once

#include "engine/core/ecs/system.h"
#include "engine/runtime/voxel/tinyVoxelWorld.h" // TinyIVec3Hash + the world

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace sfs
{

// A material reaction CA: fire. Tracks the set of voxels currently on fire (the
// "fire voxels"), and on a fixed tick spreads to adjacent flammable voxels,
// burns down fuel, and consumes spent voxels (leaves -> air, wood -> char).
// Operates purely on TinyVoxelWorld voxel data; the visible flames are
// particles the renderer emits from burningVoxels(). NOT a renderer.
class VoxelFireSystem : public System
{
public:
  explicit VoxelFireSystem(TinyVoxelWorld* world) : m_world(world) {}

  // Start a fire at a voxel (no-op if it isn't flammable / already burning).
  void ignite(const glm::ivec3& voxel);

  // World voxel coords currently on fire -- the renderer reads these to spawn
  // flame particles. Refreshed each fire tick.
  const std::vector<glm::ivec3>& burningVoxels() const { return m_burningList; }

  // A flying spark thrown off by a fire: arcs with gravity and, where it lands
  // on something flammable, may start a new fire. The renderer draws these
  // glowing.
  struct Ember
  {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float life = 0.0f;
  };
  const std::vector<Ember>& embers() const { return m_embers; }

protected:
  void update(double deltaTime) override; // fixed-tick fire CA + ember flight

private:
  void step();                           // one fire tick
  void consume(const glm::ivec3& voxel); // burn a voxel away (char or air)
  float fuelFor(const glm::ivec3& voxel) const;
  void spawnEmber(const glm::ivec3& from); // throw a spark from a burning voxel
  void simEmbers(float dt);                // per-frame ember flight + landing
  void emberLand(const glm::ivec3& cell,
                 std::uint32_t h); // maybe ignite on land

  TinyVoxelWorld* m_world = nullptr;

  struct Cell
  {
    glm::ivec3 coord{0};
    float fuel = 0.0f;
    int age = 0; // ticks burning (a warm-up before it can spread)
  };
  std::vector<Cell> m_burning;
  std::unordered_set<glm::ivec3, TinyIVec3Hash> m_burningSet;
  std::vector<glm::ivec3> m_burningList; // positions, rebuilt each tick
  std::vector<Ember> m_embers;

  double m_accum = 0.0;
  std::uint32_t m_tick = 0;
};

} // namespace sfs
