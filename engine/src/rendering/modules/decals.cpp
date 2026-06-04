#include "engine/rendering/modules/decals.h"

#include "engine/utils/profiling.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{

// Decals sit a hair in front of the surface they're on so they draw over it.
constexpr float kGroundBias = 0.01f;
constexpr float kWallBias = 0.02f;

int floorDiv(int a, int b)
{
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}

inline glm::ivec2 floorTile(const glm::vec2& p)
{
  return {static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))};
}

// Pack a colour into RGBA8 (the DecalVertex format; uploaded as a normalised
// ubyte4 attribute, so a vertex is 28B instead of 40B).
std::uint32_t packRGBA8(const glm::vec4& c)
{
  const auto q = [](float v)
  {
    return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f +
                                      0.5f);
  };
  return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}

} // namespace

glm::ivec2 Decals::chunkOf(const glm::vec2& worldPos) const
{
  const glm::ivec2 t = floorTile(worldPos);
  return {floorDiv(t.x, kChunkTiles), floorDiv(t.y, kChunkTiles)};
}

std::int64_t Decals::chunkKey(glm::ivec2 chunk)
{
  // Pack both halves as unsigned 32-bit so the key is a clean bijection.
  return (static_cast<std::int64_t>(static_cast<std::uint32_t>(chunk.x))
          << 32) |
         static_cast<std::uint32_t>(chunk.y);
}

const std::string* Decals::internTexture(const std::string& id)
{
  return &*m_textureIds.insert(id).first;
}

bool Decals::isStatic(const Decal& d)
{
  // Static = never changes per frame: not fading, and not a wall drip still
  // running down. (Permanent water with fadeRate 0 counts as static too.)
  return d.fadeRate <= 0.0f && !(d.surface == DecalSurface::Wall && !d.settled);
}

bool Decals::sameColor(const glm::vec3& a, const glm::vec3& b)
{
  // Compare colour DIRECTION (cosine), so an effect's light->dark gradient
  // reads as one paint but a different hue (blue over red) reads as a repaint.
  const float la = glm::length(a);
  const float lb = glm::length(b);
  if (la < 1e-4f || lb < 1e-4f)
    return true; // near-black: don't churn on it
  return glm::dot(a, b) / (la * lb) >= kColorSimilarity;
}

void Decals::addDecal(const DecalSpawn& spawn)
{
  Decal d;
  d.worldPos = spawn.worldPos;
  d.elevation = spawn.elevation;
  d.surface = spawn.surface;
  d.wallSide = spawn.wallSide;
  d.wallBottom = spawn.wallBottom;
  d.size = spawn.size;
  d.rotation = spawn.rotation;
  d.color = spawn.color;
  d.textureId = internTexture(spawn.textureId ? *spawn.textureId
                                              : std::string("white_pixel"));
  d.fadeRate = spawn.fadeRate;
  d.dripSpeed = spawn.dripSpeed;
  d.age = 0.0f;
  d.settled = false;

  const glm::ivec2 chunkCoord = chunkOf(d.worldPos);
  ChunkData& chunk = m_chunks[chunkCoord];

  // Spatial saturation: a permanent decal (anything that doesn't fade --
  // ground, walls, permanent water) fills its small world cell up to a quota of
  // one colour, then stops -- spraying the SAME colour on a painted spot is
  // dropped (memory tracks painted AREA, not spray count). A DIFFERENT colour
  // replaces the cell's paint, so you can paint over. The stain is otherwise
  // permanent (never time-fades). Fading decals (water) don't saturate a cell.
  const bool permanent = d.fadeRate <= 0.0f;
  if (permanent)
  {
    const std::int64_t key = coverageKey(chunkCoord, d);
    CoverageCell& cell = chunk.coverage[key];
    const glm::vec3 c{d.color};

    // Wall streaks cover more surface each, so they saturate at a lower quota.
    const int cap = d.surface == DecalSurface::Wall ? m_maxWallDecalsPerCell
                                                    : m_maxDecalsPerCell;

    if (cell.count == 0)
    {
      cell.color = c;
    }
    else if (sameColor(c, cell.color))
    {
      if (cell.count >= cap)
        return; // already painted this colour here
    }
    else
    {
      // Different colour: drop the old paint in this cell and start the new
      // one.
      clearCell(chunkCoord, chunk, key);
      cell.count = 0;
      cell.color = c;
    }
    ++cell.count;
  }

  if (d.fadeRate > 0.0f)
    ++m_fadingCount;
  if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f)
    ++m_animatingCount;

  if (isStatic(d))
    buildDecalVerts(
        d, chunk.pendingStatic); // append-only: O(new), not O(chunk)
  else
    ++chunk.animatingCount;
  chunk.decals.push_back(d);
}

std::int64_t Decals::coverageKey(glm::ivec2 chunk, const Decal& d) const
{
  // Position local to the chunk so cell indices stay small (the chunk is
  // kChunkTiles wide). Ground decals collapse by x,y; wall decals also key on
  // an elevation band + side so a streak still stacks down a face. Each field
  // has its own byte/word lane, so distinct cells never collide.
  const glm::vec2 local =
      d.worldPos - glm::vec2{chunk} * static_cast<float>(kChunkTiles);
  const std::int64_t ix =
      static_cast<std::int64_t>(std::floor(local.x / m_coverageCell)) & 0xFF;
  const std::int64_t iy =
      static_cast<std::int64_t>(std::floor(local.y / m_coverageCell)) & 0xFF;
  const std::int64_t iz =
      static_cast<std::int64_t>(std::floor(d.elevation / m_coverageElevCell)) &
      0xFFFF;
  const std::int64_t disc =
      (static_cast<std::int64_t>(d.surface) * 4 + d.wallSide) & 0xFF;

  return ix | (iy << 8) | (iz << 16) | (disc << 32);
}

void Decals::clearCell(glm::ivec2 chunk, ChunkData& data, std::int64_t key)
{
  auto& vec = data.decals;
  bool removed = false;

  for (std::size_t i = 0; i < vec.size();)
  {
    const Decal& d = vec[i];
    if (d.fadeRate <= 0.0f && coverageKey(chunk, d) == key)
    {
      // A running drip is permanent-to-be but still animating; keep the
      // animating counters in step when it's replaced.
      if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled)
      {
        --m_animatingCount;
        --data.animatingCount;
      }
      vec[i] = vec.back();
      vec.pop_back();
      removed = true;
    }
    else
    {
      ++i;
    }
  }

  if (removed)
  {
    // Can't trim the middle of the append-only GPU buffer; rebuild from what
    // remains (done once this frame in computeCommands).
    data.needsFullRebuild = true;
    data.pendingStatic.clear();
  }
}

void Decals::rebuildCoverage(glm::ivec2 chunk, ChunkData& data) const
{
  data.coverage.clear();
  for (const Decal& d : data.decals)
    if (d.fadeRate <= 0.0f)
    {
      CoverageCell& cell = data.coverage[coverageKey(chunk, d)];
      if (cell.count == 0)
        cell.color = glm::vec3{d.color};
      ++cell.count;
    }
}

void Decals::rebuildAllCoverage()
{
  for (auto& [chunk, data] : m_chunks)
    rebuildCoverage(chunk, data);
}

void Decals::clearAll()
{
  for (const auto& [chunk, data] : m_chunks)
    m_pendingFree.push_back(chunkKey(chunk));

  m_chunks.clear();
  m_fadingCount = 0;
  m_animatingCount = 0;
}

void Decals::clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile)
{
  for (auto it = m_chunks.begin(); it != m_chunks.end();)
  {
    auto& vec = it->second.decals;
    bool removedStatic = false;
    bool removedAny = false;

    for (std::size_t i = 0; i < vec.size();)
    {
      const glm::ivec2 t = floorTile(vec[i].worldPos);
      const bool inside = t.x >= minTile.x && t.x < maxTile.x &&
                          t.y >= minTile.y && t.y < maxTile.y;
      if (inside)
      {
        const Decal& d = vec[i];
        if (d.fadeRate > 0.0f)
          --m_fadingCount;
        if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled)
          --m_animatingCount;
        if (isStatic(d))
          removedStatic = true;
        else
          --it->second.animatingCount;

        vec[i] = vec.back();
        vec.pop_back();
        removedAny = true;
      }
      else
      {
        ++i;
      }
    }

    if (removedStatic)
    {
      // Can't remove from the middle of the GPU buffer; rebuild it from what
      // remains (rare path). Pending appends are part of that rebuild.
      it->second.needsFullRebuild = true;
      it->second.pendingStatic.clear();
    }

    // Coverage is only ever incremented on add, so recount it from the
    // survivors after a removal -- else cleared cells stay marked "painted" and
    // reject new paint.
    if (removedAny && !vec.empty())
      rebuildCoverage(it->first, it->second);

    if (vec.empty())
    {
      m_pendingFree.push_back(chunkKey(it->first));
      it = m_chunks.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

std::size_t Decals::decalCount() const
{
  std::size_t total = 0;
  for (const auto& [chunk, data] : m_chunks)
    total += data.decals.size();
  return total;
}

void Decals::update(double deltaTime)
{
  ZoneScopedN("Decals::update");

  // Settled/static decals never change, so only do work while something fades
  // (water) or is still running down (wall drips).
  if (m_fadingCount == 0 && m_animatingCount == 0)
    return;

  const float dt = static_cast<float>(deltaTime);
  if (dt <= 0.0f)
    return;

  for (auto it = m_chunks.begin(); it != m_chunks.end();)
  {
    ChunkData& chunk = it->second;
    auto& vec = chunk.decals;

    for (std::size_t i = 0; i < vec.size();)
    {
      Decal& d = vec[i];

      // Fading water: age and remove when invisible.
      if (d.fadeRate > 0.0f)
      {
        d.age += dt;
        if (d.age * d.fadeRate >= 1.0f)
        {
          --m_fadingCount;
          --chunk.animatingCount;
          vec[i] = vec.back();
          vec.pop_back();
          continue;
        }
        ++i;
        continue;
      }

      // Running wall drip: age until the head reaches the base, then settle
      // (it becomes a permanent static decal, so the chunk must re-upload).
      if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled)
      {
        d.age += dt;
        if (d.elevation - d.dripSpeed * d.age <= d.wallBottom)
        {
          d.settled = true;
          --m_animatingCount;
          --chunk.animatingCount;
          // Now a permanent static decal -> append its verts to the GPU buffer.
          buildDecalVerts(d, chunk.pendingStatic);
        }
      }

      ++i;
    }

    if (vec.empty())
    {
      m_pendingFree.push_back(chunkKey(it->first));
      it = m_chunks.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void Decals::buildDecalVerts(const Decal& decal,
                             std::vector<DecalVertex>& out) const
{
  glm::vec4 color = decal.color;
  if (decal.fadeRate > 0.0f)
    color.a *= std::max(0.0f, 1.0f - decal.age * decal.fadeRate);
  if (color.a <= 0.0f)
    return;

  const std::uint32_t packed = packRGBA8(color);
  const auto push = [&](glm::vec2 wp, float elev, glm::vec2 uv, float key)
  { out.push_back(DecalVertex{wp, elev, uv, packed, key}); };

  if (decal.surface != DecalSurface::Wall)
  {
    // Ground/water: a rotated world square lying flat on the surface.
    const float h = decal.size * 0.5f;
    const float c = std::cos(decal.rotation);
    const float s = std::sin(decal.rotation);
    const auto rot = [&](float ox, float oy)
    { return glm::vec2{ox * c - oy * s, ox * s + oy * c}; };

    const float e = decal.elevation;
    const float key =
        decal.worldPos.x + decal.worldPos.y + e * 0.5f + kGroundBias;

    const glm::vec2 w0 = decal.worldPos + rot(-h, -h);
    const glm::vec2 w1 = decal.worldPos + rot(h, -h);
    const glm::vec2 w2 = decal.worldPos + rot(h, h);
    const glm::vec2 w3 = decal.worldPos + rot(-h, h);

    push(w0, e, {0.0f, 0.0f}, key);
    push(w1, e, {1.0f, 0.0f}, key);
    push(w2, e, {1.0f, 1.0f}, key);
    push(w0, e, {0.0f, 0.0f}, key);
    push(w2, e, {1.0f, 1.0f}, key);
    push(w3, e, {0.0f, 1.0f}, key);
    return;
  }

  // Wall drip: a streak running down the face from its start elevation to the
  // head, capped with a soft round blob at the head. UVs: streak samples the
  // dot's centreline (v=0.5) so it's solid top-to-bottom; the cap uses full
  // radial UVs to read round.
  const glm::vec2 edgeDir =
      decal.wallSide == 2 ? glm::vec2{0.0f, 1.0f} : glm::vec2{1.0f, 0.0f};
  const float headElev =
      std::max(decal.wallBottom, decal.elevation - decal.dripSpeed * decal.age);
  const float halfW = decal.size * 0.5f;

  // Key each vertex off its own world position + elevation, matching the block
  // face's depth (world.x + world.y + ground * 0.5); the bias lifts the drip
  // just in front of the coplanar face so the depth test keeps it visible.
  const auto wallKey = [](glm::vec2 wp, float elev)
  { return wp.x + wp.y + elev * 0.5f + kWallBias; };

  const glm::vec2 a = decal.worldPos - edgeDir * halfW;
  const glm::vec2 b = decal.worldPos + edgeDir * halfW;

  push(a, decal.elevation, {0.0f, 0.5f}, wallKey(a, decal.elevation));
  push(b, decal.elevation, {1.0f, 0.5f}, wallKey(b, decal.elevation));
  push(b, headElev, {1.0f, 0.5f}, wallKey(b, headElev));
  push(a, decal.elevation, {0.0f, 0.5f}, wallKey(a, decal.elevation));
  push(b, headElev, {1.0f, 0.5f}, wallKey(b, headElev));
  push(a, headElev, {0.0f, 0.5f}, wallKey(a, headElev));

  // Round cap at the head. capH (elevation levels) is sized from capW (tiles)
  // by the projection's tile/elevation ratio so it reads roughly round.
  const float capW = halfW;
  const float capH = m_elevationStep > 0.0001f
                         ? capW * (m_tileWidth * 0.5f / m_elevationStep)
                         : capW;
  const glm::vec2 ca = decal.worldPos - edgeDir * capW;
  const glm::vec2 cb = decal.worldPos + edgeDir * capW;
  const float top = headElev + capH;
  const float bot = headElev - capH;

  push(ca, top, {0.0f, 0.0f}, wallKey(ca, top));
  push(cb, top, {1.0f, 0.0f}, wallKey(cb, top));
  push(cb, bot, {1.0f, 1.0f}, wallKey(cb, bot));
  push(ca, top, {0.0f, 0.0f}, wallKey(ca, top));
  push(cb, bot, {1.0f, 1.0f}, wallKey(cb, bot));
  push(ca, bot, {0.0f, 1.0f}, wallKey(ca, bot));
}

void Decals::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("Decals::computeCommands");

  flush(); // clears m_commands

  DecalDrawCommand cmd;
  cmd.order.pass = RenderPass::Decals;
  cmd.freeKeys = std::move(m_pendingFree);
  m_pendingFree.clear();

  const TerrainElevationGridView& grid = context.terrainElevationGrid;

  if (context.projection && grid.valid())
  {
    const IsometricProjection& proj = *context.projection;
    // Cache for geometry built at add/settle time (constant in practice).
    m_tileWidth = static_cast<float>(proj.tileWidth);
    m_elevationStep = static_cast<float>(proj.elevationStep);

    const glm::ivec2 minTile = grid.origin;
    const glm::ivec2 maxTile =
        grid.origin + glm::ivec2{grid.width, grid.height};
    const glm::ivec2 minChunk{
        floorDiv(minTile.x, kChunkTiles), floorDiv(minTile.y, kChunkTiles)};
    const glm::ivec2 maxChunk{
        floorDiv(maxTile.x, kChunkTiles), floorDiv(maxTile.y, kChunkTiles)};

    for (int cy = minChunk.y; cy <= maxChunk.y; ++cy)
    {
      for (int cx = minChunk.x; cx <= maxChunk.x; ++cx)
      {
        const auto it = m_chunks.find(glm::ivec2{cx, cy});
        if (it == m_chunks.end())
          continue;

        ChunkData& chunk = it->second;
        const std::int64_t key = chunkKey(glm::ivec2{cx, cy});

        if (!cmd.textureId && !chunk.decals.empty())
          cmd.textureId = chunk.decals.front().textureId;

        if (chunk.needsFullRebuild)
        {
          // Rare: a static decal was removed -> rebuild the whole buffer.
          DecalChunkUpload upload;
          upload.key = key;
          for (const Decal& d : chunk.decals)
            if (isStatic(d))
              buildDecalVerts(d, upload.vertices);

          cmd.uploads.push_back(std::move(upload));
          chunk.needsFullRebuild = false;
          chunk.pendingStatic.clear();
        }
        else if (!chunk.pendingStatic.empty())
        {
          // Common: append only the new static verts (O(new)).
          DecalChunkUpload append;
          append.key = key;
          append.vertices = std::move(chunk.pendingStatic);
          chunk.pendingStatic.clear();
          cmd.appends.push_back(std::move(append));
        }

        cmd.drawKeys.push_back(key);

        // Animating decals (running drips, fading water) rebuilt every frame.
        // Settled chunks have none, so they do no per-decal work at all.
        if (chunk.animatingCount > 0)
          for (const Decal& d : chunk.decals)
            if (!isStatic(d))
              buildDecalVerts(d, cmd.dynamic);
      }
    }
  }

  m_commands.push_back(std::move(cmd));
}

} // namespace sfs
