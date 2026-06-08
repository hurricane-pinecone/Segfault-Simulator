#include "engine/runtime/rendering/modules/isometricDecalSink.h"

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

// Pack a colour into RGBA8 (the vertex format; uploaded as a normalised ubyte4
// attribute).
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

glm::ivec2 IsometricDecalSink::chunkOf(const glm::vec2& worldPos) const
{
  const glm::ivec2 t = floorTile(worldPos);
  return {floorDiv(t.x, kChunkTiles), floorDiv(t.y, kChunkTiles)};
}

const std::string* IsometricDecalSink::internTexture(const std::string& id)
{
  return &*m_textureIds.insert(id).first;
}

bool IsometricDecalSink::isStatic(const Decal& d)
{
  // Static = bakeable now: not fading, and not a wall drip still running down.
  // A wall mark with no drip bakes at once; a settled drip bakes too.
  return d.fadeRate <= 0.0f &&
         !(d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled);
}

void IsometricDecalSink::addDecal(const DecalSpawn& spawn)
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

  m_spriteId = d.textureId; // one sprite drives baking + dynamic decals

  const glm::ivec2 chunkCoord = chunkOf(d.worldPos);
  ChunkData& chunk = m_chunks[chunkCoord];

  if (isStatic(d))
  {
    // Permanent: stamp it into the paint texture and discard the geometry.
    bakePermanent(chunkCoord, chunk, d);
    return;
  }

  // Animating (fading water / running drip): keep it; rebuilt each frame into
  // the dynamic buffer until it expires or settles.
  if (d.fadeRate > 0.0f)
    ++m_fadingCount;
  if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f)
    ++m_animatingCount;
  ++chunk.animatingCount;
  chunk.decals.push_back(d);
}

std::int64_t IsometricDecalSink::faceKey(const Decal& d)
{
  // A wall face is one tile edge: the along-edge tile, the fixed
  // (perpendicular) tile, and the side. Only side 2 (east) runs along Y; the
  // others along X.
  const bool alongY = d.wallSide == 2;
  const float along = alongY ? d.worldPos.y : d.worldPos.x;
  const float fixed = alongY ? d.worldPos.x : d.worldPos.y;
  const std::int64_t lo =
      static_cast<std::int64_t>(glm::floor(along)) & 0xFFFFF;
  const std::int64_t fx =
      static_cast<std::int64_t>(glm::floor(fixed)) & 0xFFFFF;
  const std::int64_t side = d.wallSide & 0x3;
  return lo | (fx << 20) | (side << 40);
}

void IsometricDecalSink::bakePermanent(glm::ivec2 chunkCoord,
                                       ChunkData& chunk,
                                       const Decal& d)
{
  if (d.surface == DecalSurface::Wall)
  {
    WallFace& face = chunk.wallFaces[faceKey(d)];
    if (face.targetId == 0)
    {
      face.targetId = m_nextTargetId++;
      face.texW = kTexelsPerTile;
      const int levels =
          static_cast<int>(glm::max(1.0f, glm::ceil(d.wallTop - d.wallBottom)));
      face.texH = glm::min(levels * kTexelsPerTile, kMaxFaceElevTexels);
      buildWallFaceDraw(d, face.drawQuad);
      face.drawDirty = true;
    }
    buildWallBake(d, face.pendingBake);
    return;
  }

  // Ground.
  const glm::vec2 origin =
      glm::vec2{chunkCoord} * static_cast<float>(kChunkTiles);
  if (chunk.groundTargetId == 0)
  {
    chunk.groundTargetId = m_nextTargetId++;
    chunk.groundTexW = kChunkTiles * kTexelsPerTile;
    chunk.groundTexH = kChunkTiles * kTexelsPerTile;
  }
  const int lx = floorTile(d.worldPos).x - chunkCoord.x * kChunkTiles;
  const int ly = floorTile(d.worldPos).y - chunkCoord.y * kChunkTiles;
  if (chunk.paintedTiles.insert(ly * kChunkTiles + lx).second)
  {
    buildGroundTileDraw(lx, ly, origin, d.elevation, chunk.groundDrawQuads);
    chunk.groundDrawDirty = true;
  }
  buildGroundBake(d, origin, chunk.pendingGroundBake);
}

void IsometricDecalSink::queueChunkFree(const ChunkData& chunk)
{
  if (chunk.groundTargetId != 0)
    m_pendingFree.push_back(chunk.groundTargetId);
  for (const auto& [fk, face] : chunk.wallFaces)
    if (face.targetId != 0)
      m_pendingFree.push_back(face.targetId);
}

void IsometricDecalSink::buildGroundBake(
    const Decal& d,
    glm::vec2 chunkOrigin,
    std::vector<DecalBakeVertex>& out) const
{
  const std::uint32_t packed = packRGBA8(d.color);
  const float hx = d.size.x * 0.5f;
  const float hy = d.size.y * 0.5f;
  const float c = glm::cos(d.rotation);
  const float s = glm::sin(d.rotation);
  const auto rot = [&](float ox, float oy) {
    return glm::vec2{ox * c - oy * s, ox * s + oy * c};
  };

  const float uLo = d.crisp ? 0.42f : 0.0f;
  const float uHi = d.crisp ? 0.58f : 1.0f;

  const glm::vec2 pts[4] = {d.worldPos + rot(-hx, -hy),
                            d.worldPos + rot(hx, -hy),
                            d.worldPos + rot(hx, hy),
                            d.worldPos + rot(-hx, hy)};
  const glm::vec2 uvs[4] = {{uLo, uLo}, {uHi, uLo}, {uHi, uHi}, {uLo, uHi}};

  // Keep the mark on the tile it landed on (the paint texture is keyed by
  // tile).
  const glm::vec2 tileMin{glm::floor(d.worldPos.x), glm::floor(d.worldPos.y)};
  ClipVertex poly[12];
  const int cnt = clipQuadToRect(
      pts, uvs, tileMin.x, tileMin.y, tileMin.x + 1.0f, tileMin.y + 1.0f, poly);

  const float inv = 1.0f / static_cast<float>(kChunkTiles);
  const auto local = [&](glm::vec2 wp) { return (wp - chunkOrigin) * inv; };
  for (int i = 1; i + 1 < cnt; ++i)
  {
    out.push_back({local(poly[0].p), poly[0].uv, packed});
    out.push_back({local(poly[i].p), poly[i].uv, packed});
    out.push_back({local(poly[i + 1].p), poly[i + 1].uv, packed});
  }
}

void IsometricDecalSink::buildGroundTileDraw(
    int lx,
    int ly,
    glm::vec2 chunkOrigin,
    float elevation,
    std::vector<DecalVertex>& out) const
{
  const float wx0 = chunkOrigin.x + static_cast<float>(lx);
  const float wy0 = chunkOrigin.y + static_cast<float>(ly);
  const float inv = 1.0f / static_cast<float>(kChunkTiles);
  const float u0 = static_cast<float>(lx) * inv;
  const float v0 = static_cast<float>(ly) * inv;
  const float u1 = static_cast<float>(lx + 1) * inv;
  const float v1 = static_cast<float>(ly + 1) * inv;
  const std::uint32_t white = 0xFFFFFFFF;

  // Per-vertex painter key (matches the terrain tile top, which varies across
  // the diamond): a single key would only depth-match along one line, clipping
  // the quad to a triangle. The key is linear in worldPos, so it interpolates
  // exactly across the two triangles.
  const auto key = [&](glm::vec2 w)
  { return w.x + w.y + elevation * 0.5f + kGroundBias; };

  const glm::vec2 P[4] = {{wx0, wy0},
                          {wx0 + 1.0f, wy0},
                          {wx0 + 1.0f, wy0 + 1.0f},
                          {wx0, wy0 + 1.0f}};
  const glm::vec2 UV[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i : idx)
    out.push_back(DecalVertex{P[i], elevation, UV[i], white, key(P[i])});
}

void IsometricDecalSink::buildWallBake(const Decal& d,
                                       std::vector<DecalBakeVertex>& out) const
{
  const std::uint32_t packed = packRGBA8(d.color);
  const bool alongY = d.wallSide == 2;
  const float along0 = alongY ? d.worldPos.y : d.worldPos.x;
  const float lo = glm::floor(along0);
  const float wb = d.wallBottom;
  const float wt = d.wallTop;
  const float h = (wt - wb) > 1e-4f ? (wt - wb) : 1.0f;

  // Face-local [0,1]: along the edge, up the face.
  const auto faceLocal = [&](float along, float elev) {
    return glm::vec2{along - lo, (elev - wb) / h};
  };

  const float uLo = d.crisp ? 0.42f : 0.0f;
  const float uHi = d.crisp ? 0.58f : 1.0f;
  const float aspect =
      m_elevationStep > 0.0001f ? (m_tileWidth * 0.5f / m_elevationStep) : 1.0f;

  if (d.dripSpeed <= 0.0f)
  {
    // Impact mark: a rotated rect in (along, elevation) space, clipped to the
    // face, mapped to face-local.
    const float hx = d.size.x * 0.5f;
    const float hy = d.size.y * 0.5f;
    const float c = glm::cos(d.rotation);
    const float s = glm::sin(d.rotation);
    const auto faceCorner = [&](float ox, float oy)
    {
      const float ru = ox * c - oy * s;
      const float rv = ox * s + oy * c;
      return glm::vec2{along0 + ru, d.elevation + rv * aspect};
    };
    const glm::vec2 pts[4] = {faceCorner(-hx, -hy),
                              faceCorner(hx, -hy),
                              faceCorner(hx, hy),
                              faceCorner(-hx, hy)};
    const glm::vec2 uvs[4] = {{uLo, uLo}, {uHi, uLo}, {uHi, uHi}, {uLo, uHi}};
    ClipVertex poly[12];
    const int cnt = clipQuadToRect(pts, uvs, lo, wb, lo + 1.0f, wt, poly);
    for (int i = 1; i + 1 < cnt; ++i)
    {
      out.push_back({faceLocal(poly[0].p.x, poly[0].p.y), poly[0].uv, packed});
      out.push_back({faceLocal(poly[i].p.x, poly[i].p.y), poly[i].uv, packed});
      out.push_back({faceLocal(poly[i + 1].p.x, poly[i + 1].p.y),
                     poly[i + 1].uv,
                     packed});
    }
    return;
  }

  // Settled drip: a streak from its start elevation down to the head, capped
  // with a round blob. UVs match buildDecalVerts' drip (centreline streak, full
  // radial cap).
  const float headElev = glm::max(wb, d.elevation - d.dripSpeed * d.age);
  const float halfW = d.size.y * 0.5f;
  const auto push = [&](float along, float elev, glm::vec2 uv) {
    out.push_back({faceLocal(along, elev), uv, packed});
  };

  const float aL = along0 - halfW;
  const float aR = along0 + halfW;
  push(aL, d.elevation, {0.0f, 0.5f});
  push(aR, d.elevation, {1.0f, 0.5f});
  push(aR, headElev, {1.0f, 0.5f});
  push(aL, d.elevation, {0.0f, 0.5f});
  push(aR, headElev, {1.0f, 0.5f});
  push(aL, headElev, {0.0f, 0.5f});

  const float capW = halfW;
  const float capH = m_elevationStep > 0.0001f
                         ? capW * (m_tileWidth * 0.5f / m_elevationStep)
                         : capW;
  const float cL = along0 - capW;
  const float cR = along0 + capW;
  const float top = headElev + capH;
  const float bot = headElev - capH;
  push(cL, top, {0.0f, 0.0f});
  push(cR, top, {1.0f, 0.0f});
  push(cR, bot, {1.0f, 1.0f});
  push(cL, top, {0.0f, 0.0f});
  push(cR, bot, {1.0f, 1.0f});
  push(cL, bot, {0.0f, 1.0f});
}

void IsometricDecalSink::buildWallFaceDraw(const Decal& d,
                                           std::vector<DecalVertex>& out) const
{
  const bool alongY = d.wallSide == 2;
  const float along0 = alongY ? d.worldPos.y : d.worldPos.x;
  const float fixed = alongY ? d.worldPos.x : d.worldPos.y;
  const float lo = glm::floor(along0);
  const float wb = d.wallBottom;
  const float wt = d.wallTop;
  const std::uint32_t white = 0xFFFFFFFF;

  const auto wp = [&](float along) {
    return alongY ? glm::vec2{fixed, along} : glm::vec2{along, fixed};
  };

  // Face rect corners (along, elevation) -> world, uv spanning the full face.
  struct Corner
  {
    float along;
    float elev;
    float u;
    float v;
  };
  const Corner corners[4] = {{lo, wb, 0.0f, 0.0f},
                             {lo + 1.0f, wb, 1.0f, 0.0f},
                             {lo + 1.0f, wt, 1.0f, 1.0f},
                             {lo, wt, 0.0f, 1.0f}};
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i : idx)
  {
    const Corner& cc = corners[i];
    const glm::vec2 w = wp(cc.along);
    const float key = w.x + w.y + cc.elev * 0.5f + kWallBias;
    out.push_back(DecalVertex{w, cc.elev, {cc.u, cc.v}, white, key});
  }
}

void IsometricDecalSink::clearAll()
{
  for (const auto& [chunk, data] : m_chunks)
    queueChunkFree(data);

  m_chunks.clear();
  m_fadingCount = 0;
  m_animatingCount = 0;
}

void IsometricDecalSink::clearRegion(glm::ivec2 minTile, glm::ivec2 maxTile)
{
  // Baked paint can't be partially erased (the geometry is gone), so clear at
  // chunk granularity: free every chunk whose footprint overlaps the region.
  for (auto it = m_chunks.begin(); it != m_chunks.end();)
  {
    const glm::ivec2 chunkMin = it->first * kChunkTiles;
    const glm::ivec2 chunkMax = chunkMin + glm::ivec2{kChunkTiles, kChunkTiles};
    const bool overlaps = chunkMin.x < maxTile.x && chunkMax.x > minTile.x &&
                          chunkMin.y < maxTile.y && chunkMax.y > minTile.y;
    if (!overlaps)
    {
      ++it;
      continue;
    }

    for (const Decal& d : it->second.decals)
    {
      if (d.fadeRate > 0.0f)
        --m_fadingCount;
      if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled)
        --m_animatingCount;
    }

    queueChunkFree(it->second);
    it = m_chunks.erase(it);
  }
}

std::size_t IsometricDecalSink::decalCount() const
{
  std::size_t total = 0;
  for (const auto& [chunk, data] : m_chunks)
  {
    total += data.decals.size();       // animating
    total += data.paintedTiles.size(); // baked ground tiles
    total += data.wallFaces.size();    // baked wall faces
  }
  return total;
}

void IsometricDecalSink::update(double deltaTime)
{
  ZoneScopedN("IsometricDecalSink::update");

  // Settled/baked decals never change, so only do work while something fades
  // (water) or is still running down (wall drips).
  if (m_fadingCount == 0 && m_animatingCount == 0)
    return;

  const float dt = static_cast<float>(deltaTime);
  if (dt <= 0.0f)
    return;

  for (auto it = m_chunks.begin(); it != m_chunks.end();)
  {
    const glm::ivec2 coord = it->first;
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

      // Running wall drip: age until the head reaches the base, then settle --
      // bake the finished streak into its face and drop the dynamic copy.
      if (d.surface == DecalSurface::Wall && d.dripSpeed > 0.0f && !d.settled)
      {
        d.age += dt;
        if (d.elevation - d.dripSpeed * d.age <= d.wallBottom)
        {
          d.settled = true;
          --m_animatingCount;
          --chunk.animatingCount;
          bakePermanent(coord, chunk, d);
          vec[i] = vec.back();
          vec.pop_back();
          continue;
        }
      }

      ++i;
    }

    // Drop a chunk only when nothing is left in it (no animating decals and no
    // baked paint); baked stains are permanent, so painted chunks persist.
    if (vec.empty() && chunk.paintedTiles.empty() && chunk.wallFaces.empty())
      it = m_chunks.erase(it);
    else
      ++it;
  }
}

void IsometricDecalSink::buildDecalVerts(const Decal& decal,
                                         std::vector<DecalVertex>& out) const
{
  glm::vec4 color = decal.color;
  if (decal.fadeRate > 0.0f)
    color.a *= glm::max(0.0f, 1.0f - decal.age * decal.fadeRate);
  if (color.a <= 0.0f)
    return;

  const std::uint32_t packed = packRGBA8(color);
  const auto push = [&](glm::vec2 wp, float elev, glm::vec2 uv, float key) {
    out.push_back(DecalVertex{wp, elev, uv, packed, key});
  };

  const float uLo = decal.crisp ? 0.42f : 0.0f;
  const float uHi = decal.crisp ? 0.58f : 1.0f;

  if (decal.surface != DecalSurface::Wall)
  {
    // Fading water: a rotated world rectangle lying flat on the surface.
    const float hx = decal.size.x * 0.5f;
    const float hy = decal.size.y * 0.5f;
    const float c = glm::cos(decal.rotation);
    const float s = glm::sin(decal.rotation);
    const auto rot = [&](float ox, float oy) {
      return glm::vec2{ox * c - oy * s, ox * s + oy * c};
    };

    const float e = decal.elevation;
    const float key =
        decal.worldPos.x + decal.worldPos.y + e * 0.5f + kGroundBias;

    const glm::vec2 pts[4] = {decal.worldPos + rot(-hx, -hy),
                              decal.worldPos + rot(hx, -hy),
                              decal.worldPos + rot(hx, hy),
                              decal.worldPos + rot(-hx, hy)};
    const glm::vec2 uvs[4] = {{uLo, uLo}, {uHi, uLo}, {uHi, uHi}, {uLo, uHi}};

    const glm::vec2 tileMin{
        glm::floor(decal.worldPos.x), glm::floor(decal.worldPos.y)};
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

  // Running wall drip: a streak from its start elevation to the head, capped
  // with a soft round blob at the head.
  const glm::vec2 edgeDir =
      decal.wallSide == 2 ? glm::vec2{0.0f, 1.0f} : glm::vec2{1.0f, 0.0f};

  const auto wallKey = [](glm::vec2 wp, float elev)
  { return wp.x + wp.y + elev * 0.5f + kWallBias; };

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

void IsometricDecalSink::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("IsometricDecalSink::computeCommands");

  flush(); // clears m_commands

  // Hidden: emit nothing so no decals draw. Stains keep accumulating (pending
  // bakes/frees queue up) and reappear when re-shown.
  if (!m_visible)
    return;

  DecalDrawCommand cmd;
  cmd.order.pass = RenderPass::Decals;
  cmd.textureId = m_spriteId;
  cmd.freeKeys = std::move(m_pendingFree);
  m_pendingFree.clear();

  if (context.projection)
  {
    m_tileWidth = static_cast<float>(context.projection->tileWidth);
    m_elevationStep = static_cast<float>(context.projection->elevationStep);
  }

  // Bake this frame's new splats and refresh any target whose draw set grew --
  // for every chunk, so off-screen painting flushes too (memory stays bounded).
  for (auto& [coord, chunk] : m_chunks)
  {
    if (chunk.groundTargetId != 0 && !chunk.pendingGroundBake.empty())
    {
      DecalBakeBatch bake;
      bake.key = chunk.groundTargetId;
      bake.texW = chunk.groundTexW;
      bake.texH = chunk.groundTexH;
      bake.verts = std::move(chunk.pendingGroundBake);
      chunk.pendingGroundBake.clear();
      cmd.bakes.push_back(std::move(bake));
    }
    if (chunk.groundDrawDirty)
    {
      cmd.drawUploads.push_back({chunk.groundTargetId, chunk.groundDrawQuads});
      chunk.groundDrawDirty = false;
    }

    for (auto& [fk, face] : chunk.wallFaces)
    {
      if (!face.pendingBake.empty())
      {
        DecalBakeBatch bake;
        bake.key = face.targetId;
        bake.texW = face.texW;
        bake.texH = face.texH;
        bake.verts = std::move(face.pendingBake);
        face.pendingBake.clear();
        cmd.bakes.push_back(std::move(bake));
      }
      if (face.drawDirty)
      {
        cmd.drawUploads.push_back({face.targetId, face.drawQuad});
        face.drawDirty = false;
      }
    }
  }

  // Draw + rebuild dynamics only for visible chunks.
  const TerrainElevationGridView& grid = context.terrainElevationGrid;
  if (context.projection && grid.valid())
  {
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

        if (chunk.groundTargetId != 0 && !chunk.paintedTiles.empty())
          cmd.drawKeys.push_back(chunk.groundTargetId);
        for (const auto& [fk, face] : chunk.wallFaces)
          if (face.targetId != 0)
            cmd.drawKeys.push_back(face.targetId);

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
