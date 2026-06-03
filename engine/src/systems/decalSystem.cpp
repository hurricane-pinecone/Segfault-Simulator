#include "engine/systems/decalSystem.h"

#include "engine/utils/profiling.h"

#include <cmath>

namespace sfs
{

namespace
{

// Decals sit a hair in front of the surface they're on so they draw over it
// (depth test is LEQUAL, no depth write). Walls need a touch more to clear the
// block's own billboard face.
constexpr float kGroundBias = 0.01f;
// Small: the wall decal shares its block's sort key, so it only needs to clear
// that block's own billboard, not beat other blocks.
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

} // namespace

glm::ivec2 DecalSystem::chunkOf(const glm::vec2& worldPos) const
{
  const glm::ivec2 tile = floorTile(worldPos);
  return {floorDiv(tile.x, kChunkTiles), floorDiv(tile.y, kChunkTiles)};
}

const std::string* DecalSystem::internTexture(const std::string& id)
{
  // unordered_set is node-based, so element addresses are stable across inserts.
  return &*m_textureIds.insert(id).first;
}

void DecalSystem::addDecal(const DecalSpawn& spawn)
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
  d.textureId =
      internTexture(spawn.textureId ? *spawn.textureId : std::string("white_pixel"));
  d.fadeRate = spawn.fadeRate;
  d.dripSpeed = spawn.dripSpeed;
  d.sortKey = spawn.sortKey;
  d.age = 0.0f;
  d.settled = false;

  if (d.fadeRate > 0.0f)
    ++m_fadingCount;
  if (d.dripSpeed > 0.0f)
    ++m_animatingCount; // animates while running down, then stays permanent

  m_chunks[chunkOf(spawn.worldPos)].push_back(d);
}

void DecalSystem::clearAll()
{
  m_chunks.clear();
  m_fadingCount = 0;
  m_animatingCount = 0;
}

void DecalSystem::clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile)
{
  for (auto chunkIt = m_chunks.begin(); chunkIt != m_chunks.end();)
  {
    auto& vec = chunkIt->second;
    for (std::size_t i = 0; i < vec.size();)
    {
      const glm::ivec2 tile = floorTile(vec[i].worldPos);
      const bool inside = tile.x >= minTile.x && tile.x < maxTile.x &&
                          tile.y >= minTile.y && tile.y < maxTile.y;
      if (inside)
      {
        if (vec[i].fadeRate > 0.0f)
          --m_fadingCount;
        if (vec[i].dripSpeed > 0.0f && !vec[i].settled)
          --m_animatingCount;
        vec[i] = vec.back();
        vec.pop_back();
      }
      else
      {
        ++i;
      }
    }

    if (vec.empty())
      chunkIt = m_chunks.erase(chunkIt);
    else
      ++chunkIt;
  }
}

std::size_t DecalSystem::decalCount() const
{
  std::size_t total = 0;
  for (const auto& [chunk, vec] : m_chunks)
    total += vec.size();
  return total;
}

void DecalSystem::update(double deltaTime)
{
  ZoneScopedN("DecalSystem::update");

  // Settled/permanent decals (the vast majority) never change, so only do work
  // while something is fading (water) or still running down (wall drips).
  if (m_fadingCount == 0 && m_animatingCount == 0)
    return;

  const float dt = static_cast<float>(deltaTime);
  if (dt <= 0.0f)
    return;

  for (auto chunkIt = m_chunks.begin(); chunkIt != m_chunks.end();)
  {
    auto& vec = chunkIt->second;
    for (std::size_t i = 0; i < vec.size();)
    {
      Decal& d = vec[i];

      // Water (and any fading decal): age and remove when invisible.
      if (d.fadeRate > 0.0f)
      {
        d.age += dt;
        if (d.age * d.fadeRate >= 1.0f)
        {
          --m_fadingCount;
          vec[i] = vec.back();
          vec.pop_back();
          continue;
        }
        ++i;
        continue;
      }

      // Wall drip: age until the head reaches the base, then settle (it becomes
      // a permanent stain and stops needing updates).
      if (d.dripSpeed > 0.0f && !d.settled)
      {
        d.age += dt;
        if (d.elevation - d.dripSpeed * d.age <= d.wallBottom)
        {
          d.settled = true;
          --m_animatingCount;
        }
      }

      ++i;
    }

    if (vec.empty())
      chunkIt = m_chunks.erase(chunkIt);
    else
      ++chunkIt;
  }
}

void DecalSystem::appendDecalQuad(const Decal& decal,
                                  const IsometricRenderContext& context,
                                  std::vector<ParticleQuad>& out) const
{
  const IsometricProjection& proj = *context.projection;

  float alpha = decal.color.a;
  if (decal.fadeRate > 0.0f)
    alpha *= std::max(0.0f, 1.0f - decal.age * decal.fadeRate);

  if (alpha <= 0.0f)
    return;

  const glm::vec4 color{decal.color.r, decal.color.g, decal.color.b, alpha};
  const float pixelPerTile =
      static_cast<float>(proj.tileWidth) * proj.worldScale * proj.zoom;

  ParticleQuad q;
  q.color = color;

  if (decal.surface == DecalSurface::Wall)
  {
    // A thin drip: a streak from its start elevation down to a head that runs
    // toward the wall base over time (decal.age), so it animates as running
    // blood. Only the camera-facing east (2) / south (3) faces get wall drips.
    const float headElev = std::max(
        decal.wallBottom, decal.elevation - decal.dripSpeed * decal.age);

    const glm::vec2 top = proj.worldToScreen(decal.worldPos, decal.elevation);
    const glm::vec2 head = proj.worldToScreen(decal.worldPos, headElev);

    // Horizontal axis runs along the visible edge: east face along +Y, south
    // face along +X.
    const glm::vec2 edgeDirWorld =
        decal.wallSide == 2 ? glm::vec2{0.0f, 1.0f} : glm::vec2{1.0f, 0.0f};
    const glm::vec2 edgeScreen =
        proj.worldToScreen(decal.worldPos + edgeDirWorld, decal.elevation) - top;
    const float len =
        std::sqrt(edgeScreen.x * edgeScreen.x + edgeScreen.y * edgeScreen.y);
    const glm::vec2 unitH =
        len > 1e-4f ? edgeScreen / len : glm::vec2{1.0f, 0.0f};

    const float halfW = decal.size * pixelPerTile * 0.5f;
    const glm::vec2 axisH = unitH * halfW;
    // Co-sort with the host block's billboard (not the face-edge position) so the
    // streak is occluded by exactly what occludes the block -- otherwise a tall
    // streak's inflated key draws over nearer, shorter blocks.
    const float z = decal.sortKey + kWallBias;

    // The streak. Sample the dot's horizontal centreline (v = 0.5) along the
    // whole length: soft on the left/right edges (u 0->1), opaque top-to-bottom
    // so it paints a continuous path instead of fading out at the ends.
    q.points[0] = top - axisH; // streak top (fixed at the start elevation)
    q.points[1] = top + axisH;
    q.points[2] = head + axisH; // running head, descending toward the base
    q.points[3] = head - axisH;
    q.uvs[0] = {0.0f, 0.5f};
    q.uvs[1] = {1.0f, 0.5f};
    q.uvs[2] = {1.0f, 0.5f};
    q.uvs[3] = {0.0f, 0.5f};
    q.z = z;
    out.push_back(q);

    // A soft round blob at the leading edge so the running tip reads as a
    // droplet rather than a flat cut (full radial UVs).
    const float r = halfW * 1.3f;
    ParticleQuad cap;
    cap.color = color;
    cap.points[0] = head + glm::vec2{-r, -r};
    cap.points[1] = head + glm::vec2{r, -r};
    cap.points[2] = head + glm::vec2{r, r};
    cap.points[3] = head + glm::vec2{-r, r};
    cap.z = z;
    out.push_back(cap);
    return;
  }
  else
  {
    // Ground/water: a rotated square lying flat on the surface (rotated in world
    // space, then projected so it sits in the isometric plane).
    const float h = decal.size * 0.5f;
    const float c = std::cos(decal.rotation);
    const float s = std::sin(decal.rotation);

    const auto rot = [&](float ox, float oy)
    { return glm::vec2{ox * c - oy * s, ox * s + oy * c}; };

    const float elev = decal.elevation;
    q.points[0] = proj.worldToScreen(decal.worldPos + rot(-h, -h), elev);
    q.points[1] = proj.worldToScreen(decal.worldPos + rot(h, -h), elev);
    q.points[2] = proj.worldToScreen(decal.worldPos + rot(h, h), elev);
    q.points[3] = proj.worldToScreen(decal.worldPos + rot(-h, h), elev);

    q.z = decal.worldPos.x + decal.worldPos.y + elev * 0.5f + kGroundBias;
  }

  out.push_back(q);
}

void DecalSystem::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("DecalSystem::computeCommands");

  flush(); // clears m_commands

  if (!context.projection)
    return;

  // Only build quads for chunks inside the camera's tile window, so the cost
  // tracks what's on screen, not the total number of stored decals.
  const TerrainElevationGridView& grid = context.terrainElevationGrid;
  if (!grid.valid())
    return;

  const glm::ivec2 minTile = grid.origin;
  const glm::ivec2 maxTile =
      grid.origin + glm::ivec2{grid.width, grid.height};

  const glm::ivec2 minChunk{floorDiv(minTile.x, kChunkTiles),
                            floorDiv(minTile.y, kChunkTiles)};
  const glm::ivec2 maxChunk{floorDiv(maxTile.x, kChunkTiles),
                            floorDiv(maxTile.y, kChunkTiles)};

  // Bucket visible decals by texture (usually just one).
  std::unordered_map<const std::string*, ParticleBatch> buckets;

  for (int cy = minChunk.y; cy <= maxChunk.y; ++cy)
  {
    for (int cx = minChunk.x; cx <= maxChunk.x; ++cx)
    {
      const auto it = m_chunks.find(glm::ivec2{cx, cy});
      if (it == m_chunks.end())
        continue;

      for (const Decal& decal : it->second)
        appendDecalQuad(decal, context, buckets[decal.textureId].quads);
    }
  }

  for (auto& [texture, batch] : buckets)
  {
    if (batch.quads.empty())
      continue;

    ParticleBatchCommand cmd;
    cmd.quad = std::move(batch);
    cmd.textureId = texture;
    cmd.blend = BlendMode::Alpha;
    cmd.order.pass = RenderPass::Decals;
    cmd.order.depth = 0.0f;
    cmd.order.subpass = 0;

    m_commands.push_back(std::move(cmd));
  }
}

} // namespace sfs
