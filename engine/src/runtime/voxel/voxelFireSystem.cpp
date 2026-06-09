#include "engine/runtime/voxel/voxelFireSystem.h"

#include "engine/core/util/profiling.h"
#include "engine/core/voxel/tinyVoxelMaterial.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>

namespace sfs
{
namespace
{
constexpr double kTickInterval = 0.1; // 10 Hz fire ticks
constexpr int kSpreadWarmup = 3; // ticks a voxel burns before it can spread
constexpr std::size_t kMaxEmbers = 400;

// Deterministic-ish [0,1) from a voxel coord + tick, for spread chance.
float rng(const glm::ivec3& v, std::uint32_t tick)
{
  std::uint32_t h = static_cast<std::uint32_t>(v.x) * 73856093u ^
                    static_cast<std::uint32_t>(v.y) * 19349663u ^
                    static_cast<std::uint32_t>(v.z) * 83492791u ^
                    tick * 2654435761u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>(h & 0xFFFFu) / 65536.0f;
}

// Per-tick chance that a burning voxel ignites this flammable neighbour.
float spreadChance(TinyMaterial m)
{
  if (m == TinyMaterial::Leaves)
    return 0.55f; // leaves catch fast
  if (m == TinyMaterial::Wood)
    return 0.12f; // wood is slow to catch
  return 0.0f;
}
} // namespace

float VoxelFireSystem::fuelFor(const glm::ivec3& voxel) const
{
  const TinyMaterial m =
      tinyMaterialOf(m_world->voxelAt(voxel.x, voxel.y, voxel.z));
  return m == TinyMaterial::Leaves ? 4.0f : 14.0f; // ticks of burn
}

void VoxelFireSystem::ignite(const glm::ivec3& voxel)
{
  if (!m_world || m_burningSet.count(voxel))
    return;
  const TinyMaterial m =
      tinyMaterialOf(m_world->voxelAt(voxel.x, voxel.y, voxel.z));
  if (!tinyFlammable(m))
    return;
  m_burning.push_back({voxel, fuelFor(voxel)});
  m_burningSet.insert(voxel);
}

void VoxelFireSystem::consume(const glm::ivec3& v)
{
  const TinyMaterial m = tinyMaterialOf(m_world->voxelAt(v.x, v.y, v.z));
  if (m == TinyMaterial::Leaves)
    m_world->setVoxel(v.x, v.y, v.z, 0u); // leaves burn away to nothing
  else
    m_world->setVoxel(
        v.x, v.y, v.z, tinyVoxel(38, 30, 26, TinyMaterial::Generic)); // char
}

void VoxelFireSystem::step()
{
  ++m_tick;
  static const glm::ivec3 kN[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

  std::vector<Cell> survivors;
  survivors.reserve(m_burning.size());
  std::vector<glm::ivec3> ignitions;

  for (Cell& c : m_burning)
  {
    c.fuel -= 1.0f;
    ++c.age;
    if (c.fuel <= 0.0f)
    {
      consume(c.coord);
      m_burningSet.erase(c.coord);
      continue;
    }
    // A freshly-lit voxel warms up before it can spread or spark, so the fire
    // grows from one voxel as a front instead of flashing a whole
    // cross-section.
    if (c.age >= kSpreadWarmup)
    {
      // Established fires occasionally throw an ember.
      if (m_embers.size() < kMaxEmbers &&
          rng(c.coord, m_tick * 7u + 3u) < 0.05f)
        spawnEmber(c.coord);

      for (const glm::ivec3& d : kN)
      {
        const glm::ivec3 n = c.coord + d;
        if (m_burningSet.count(n))
          continue;
        const TinyMaterial nm = tinyMaterialOf(m_world->voxelAt(n.x, n.y, n.z));
        if (tinyFlammable(nm) && rng(n, m_tick) < spreadChance(nm))
          ignitions.push_back(n);
      }
    }
    survivors.push_back(c);
  }

  m_burning = std::move(survivors);
  for (const glm::ivec3& n : ignitions)
    if (!m_burningSet.count(n))
    {
      m_burning.push_back({n, fuelFor(n)});
      m_burningSet.insert(n);
    }

  m_burningList.clear();
  m_burningList.reserve(m_burning.size());
  for (const Cell& c : m_burning)
    m_burningList.push_back(c.coord);
}

void VoxelFireSystem::spawnEmber(const glm::ivec3& from)
{
  std::uint32_t h = static_cast<std::uint32_t>(from.x) * 73856093u ^
                    static_cast<std::uint32_t>(from.y) * 19349663u ^
                    static_cast<std::uint32_t>(from.z) * 83492791u ^
                    m_tick * 374761393u;
  h = (h ^ (h >> 13)) * 1274126177u;
  glm::vec3 dir{static_cast<float>(h & 0xFFu) / 127.5f - 1.0f,
                0.0f,
                static_cast<float>((h >> 8) & 0xFFu) / 127.5f - 1.0f};
  const float len = glm::length(dir);
  dir = len > 0.001f ? dir / len : glm::vec3{1.0f, 0.0f, 0.0f};
  const float hspeed = 25.0f + static_cast<float>((h >> 16) & 0x1Fu);
  const float upspeed = 55.0f + static_cast<float>((h >> 21) & 0x1Fu);

  Ember e;
  e.pos = glm::vec3{static_cast<float>(from.x) + 0.5f,
                    static_cast<float>(from.y) + 1.0f,
                    static_cast<float>(from.z) + 0.5f};
  e.vel = dir * hspeed + glm::vec3{0.0f, upspeed, 0.0f};
  e.life = 1.6f + static_cast<float>((h >> 26) & 0xFu) / 8.0f;
  m_embers.push_back(e);
}

void VoxelFireSystem::emberLand(const glm::ivec3& cell, std::uint32_t h)
{
  if ((h & 1u) == 0u)
    return; // ~half of landings spark nothing
  static const glm::ivec3 kAround[7] = {{0, 0, 0},
                                        {1, 0, 0},
                                        {-1, 0, 0},
                                        {0, 1, 0},
                                        {0, -1, 0},
                                        {0, 0, 1},
                                        {0, 0, -1}};
  for (const glm::ivec3& d : kAround)
  {
    const glm::ivec3 n = cell + d;
    if (tinyFlammable(tinyMaterialOf(m_world->voxelAt(n.x, n.y, n.z))))
    {
      ignite(n);
      return;
    }
  }
}

void VoxelFireSystem::simEmbers(float dt)
{
  if (m_embers.empty())
    return;
  const float gravity = 95.0f;
  const auto vox = [](const glm::vec3& p)
  {
    return glm::ivec3{static_cast<int>(glm::floor(p.x)),
                      static_cast<int>(glm::floor(p.y)),
                      static_cast<int>(glm::floor(p.z))};
  };
  for (Ember& e : m_embers)
  {
    e.vel.y -= gravity * dt;
    const glm::ivec3 prev = vox(e.pos);
    e.pos += e.vel * dt;
    e.life -= dt;
    if (e.life <= 0.0f || e.pos.y < -80.0f)
    {
      e.life = -1.0f; // burnt out in the air
      continue;
    }
    const glm::ivec3 cell = vox(e.pos);
    if (cell != prev && m_world->solidAt(cell.x, cell.y, cell.z))
    {
      std::uint32_t h = static_cast<std::uint32_t>(cell.x) * 2654435761u ^
                        static_cast<std::uint32_t>(cell.y) * 40503u ^
                        static_cast<std::uint32_t>(cell.z) * 73856093u ^ m_tick;
      emberLand(cell, h);
      e.life = -1.0f; // consumed on landing
    }
  }
  m_embers.erase(std::remove_if(m_embers.begin(),
                                m_embers.end(),
                                [](const Ember& e) { return e.life < 0.0f; }),
                 m_embers.end());
}

void VoxelFireSystem::update(double deltaTime)
{
  ZoneScopedN("VoxelFireSystem::update");
  if (!m_world ||
      (m_burning.empty() && m_burningList.empty() && m_embers.empty()))
    return;
  m_accum += deltaTime;
  int guard = 0;
  while (m_accum >= kTickInterval && guard++ < 8) // cap catch-up ticks
  {
    m_accum -= kTickInterval;
    step();
  }
  simEmbers(static_cast<float>(deltaTime));
}

} // namespace sfs
