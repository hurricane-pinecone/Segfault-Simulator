#include "engine/runtime/rendering/modules/decals.h"

#include "engine/core/util/algorithms/polygonClip.h"
#include "engine/core/util/profiling.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <utility>

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
  return {static_cast<int>(glm::floor(p.x)), static_cast<int>(glm::floor(p.y))};
}

// Pack a colour into RGBA8 (the DecalVertex format; uploaded as a normalised
// ubyte4 attribute, so a vertex is 28B instead of 40B).
std::uint32_t packRGBA8(const glm::vec4& c)
{
  const auto q = [](float v)
  {
    return static_cast<std::uint32_t>(glm::clamp(v, 0.0f, 1.0f) * 255.0f +
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
  // running down. A wall mark with no drip (dripSpeed 0) is static at once;
  // permanent water (fadeRate 0) counts as static too.
  return d.fadeRate <= 0.0f &&
         !(d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled);
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
  d.wallTop = spawn.wallTop;
  d.size = spawn.size;
  d.rotation = spawn.rotation;
  d.crisp = spawn.crisp;
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
      static_cast<std::int64_t>(glm::floor(local.x / m_coverageCell)) & 0xFF;
  const std::int64_t iy =
      static_cast<std::int64_t>(glm::floor(local.y / m_coverageCell)) & 0xFF;
  const std::int64_t iz =
      static_cast<std::int64_t>(glm::floor(d.elevation / m_coverageElevCell)) &
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
    color.a *= glm::max(0.0f, 1.0f - decal.age * decal.fadeRate);
  if (color.a <= 0.0f)
    return;

  const std::uint32_t packed = packRGBA8(color);
  const auto push = [&](glm::vec2 wp, float elev, glm::vec2 uv, float key)
  { out.push_back(DecalVertex{wp, elev, uv, packed, key}); };

  // Crisp marks sample the dot's solid centre (a sharp filled streak); soft marks
  // sample the whole sprite (the round radial falloff). One texture, two looks.
  const float uLo = decal.crisp ? 0.42f : 0.0f;
  const float uHi = decal.crisp ? 0.58f : 1.0f;

  if (decal.surface != DecalSurface::Wall)
  {
    // Ground/water: a rotated world rectangle lying flat on the surface. Local
    // +X is the length (along the impact direction), +Y the width, so an
    // elongated splat streaks along travel.
    const float hx = decal.size.x * 0.5f;
    const float hy = decal.size.y * 0.5f;
    const float c = glm::cos(decal.rotation);
    const float s = glm::sin(decal.rotation);
    const auto rot = [&](float ox, float oy)
    { return glm::vec2{ox * c - oy * s, ox * s + oy * c}; };

    const float e = decal.elevation;
    const float key =
        decal.worldPos.x + decal.worldPos.y + e * 0.5f + kGroundBias;

    const glm::vec2 pts[4] = {decal.worldPos + rot(-hx, -hy),
                              decal.worldPos + rot(hx, -hy),
                              decal.worldPos + rot(hx, hy),
                              decal.worldPos + rot(-hx, hy)};
    const glm::vec2 uvs[4] = {
        {uLo, uLo}, {uHi, uLo}, {uHi, uHi}, {uLo, uHi}};

    // Keep the mark on the tile it landed on (clip to that tile's world rect), so
    // a streak doesn't bleed across a tile/elevation boundary. Same key for all
    // verts (the rect is one tile, one depth band).
    const glm::vec2 tileMin{glm::floor(decal.worldPos.x),
                            glm::floor(decal.worldPos.y)};
    ClipVertex poly[12];
    const int cnt = clipQuadToRect(pts,
                                   uvs,
                                   tileMin.x,
                                   tileMin.y,
                                   tileMin.x + 1.0f,
                                   tileMin.y + 1.0f,
                                   poly);
    for (int i = 1; i + 1 < cnt; ++i)
    {
      push(poly[0].p, e, poly[0].uv, key);
      push(poly[i].p, e, poly[i].uv, key);
      push(poly[i + 1].p, e, poly[i + 1].uv, key);
    }
    return;
  }

  // Wall faces run along one tile axis (east face along Y, south along X). Decals
  // on the face live in (u along the edge, v in elevation).
  const glm::vec2 edgeDir =
      decal.wallSide == 2 ? glm::vec2{0.0f, 1.0f} : glm::vec2{1.0f, 0.0f};

  // Key each vertex off its own world position + elevation, matching the block
  // face's depth (world.x + world.y + ground * 0.5); the bias lifts the decal
  // just in front of the coplanar face so the depth test keeps it visible.
  const auto wallKey = [](glm::vec2 wp, float elev)
  { return wp.x + wp.y + elev * 0.5f + kWallBias; };

  // Elevation levels per tile-unit of screen height, so a face decal rotates by
  // a true on-screen angle and isn't squashed by the projection.
  const float aspect = m_elevationStep > 0.0001f
                           ? (m_tileWidth * 0.5f / m_elevationStep)
                           : 1.0f;

  // No drip: a directional impact mark, a rotated rectangle on the face. Rotated
  // in screen-proportional space (elevation scaled by `aspect`), then mapped to
  // world (u -> edge) and elevation.
  if (decal.dripSpeed <= 0.0f)
  {
    const float hx = decal.size.x * 0.5f; // length (local +X)
    const float hy = decal.size.y * 0.5f; // width  (local +Y)
    const float c = glm::cos(decal.rotation);
    const float s = glm::sin(decal.rotation);
    const bool east = decal.wallSide == 2;
    const float alongCenter = east ? decal.worldPos.y : decal.worldPos.x;
    const float fixedCoord = east ? decal.worldPos.x : decal.worldPos.y;

    // Corners in (along-edge, elevation) face space, then clip to the face rect
    // [tile edge] x [wallBottom, wallTop] so the mark can't spill above the wall
    // top, off its sides, or onto the ground below -- even when rotated.
    const auto faceCorner = [&](float ox, float oy)
    {
      const float ru = ox * c - oy * s; // along the edge (tiles)
      const float rv = ox * s + oy * c; // vertical (tile-equiv)
      return glm::vec2{alongCenter + ru, decal.elevation + rv * aspect};
    };
    const glm::vec2 pts[4] = {faceCorner(-hx, -hy),
                              faceCorner(hx, -hy),
                              faceCorner(hx, hy),
                              faceCorner(-hx, hy)};
    const glm::vec2 uvs[4] = {
        {uLo, uLo}, {uHi, uLo}, {uHi, uHi}, {uLo, uHi}};

    const float lo = glm::floor(alongCenter);
    ClipVertex poly[12];
    const int cnt = clipQuadToRect(
        pts, uvs, lo, decal.wallBottom, lo + 1.0f, decal.wallTop, poly);

    const auto emit = [&](const ClipVertex& v)
    {
      const glm::vec2 wp =
          east ? glm::vec2{fixedCoord, v.p.x} : glm::vec2{v.p.x, fixedCoord};
      push(wp, v.p.y, v.uv, wallKey(wp, v.p.y));
    };
    for (int i = 1; i + 1 < cnt; ++i)
    {
      emit(poly[0]);
      emit(poly[i]);
      emit(poly[i + 1]);
    }
    return;
  }

  // Wall drip: a streak running down the face from its start elevation to the
  // head, capped with a soft round blob at the head. UVs: streak samples the
  // dot's centreline (v=0.5) so it's solid top-to-bottom; the cap uses full
  // radial UVs to read round.
  const float headElev =
      glm::max(decal.wallBottom, decal.elevation - decal.dripSpeed * decal.age);
  const float halfW = decal.size.y * 0.5f;

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

  // Hidden: emit nothing so no decals draw. Stains keep accumulating in the
  // chunks (pending uploads/frees just queue up) and reappear when re-shown.
  if (!m_visible)
    return;

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
