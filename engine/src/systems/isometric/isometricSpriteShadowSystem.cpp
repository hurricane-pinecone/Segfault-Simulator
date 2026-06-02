#include "engine/systems/isometric/isometricSpriteShadowSystem.h"

#include "engine/assetStore/assetStore.h"
#include "engine/components/shadowCasterComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/rendering/util/isometric/geometry.h"
#include "engine/utils/algorithms/gridDDA.h"
#include "engine/utils/profiling.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace sfs
{

void IsometricSpriteShadowSystem::create()
{
  registerComponent<TransformComponent>();
  registerComponent<SpriteComponent>();
  registerComponent<ShadowCasterComponent>();
}

void IsometricSpriteShadowSystem::setSpriteShadowMaxLength(float length)
{
  m_shadowSettings.spriteShadowMaxLength = std::max(length, 0.0f);
}

void IsometricSpriteShadowSystem::setSpriteShadowAlpha(float alpha)
{
  m_shadowSettings.spriteShadowAlpha = std::clamp(alpha, 0.0f, 1.0f);
}

void IsometricSpriteShadowSystem::computeCommands(
    const IsometricRenderContext& context)
{
  ZoneScopedN("SpriteShadow: computeCommands");

  flush();

  for (const auto& e : getEntities())
  {
    constructSpriteShadows(context, e);
  }
}

void IsometricSpriteShadowSystem::constructSpriteShadows(
    const IsometricRenderContext& context,
    const Entity& entity)
{
  ZoneScopedN("SpriteShadow: constructSpriteShadows");

  const auto* ambientLighting = context.ambientLighting;
  const auto* pointLights = context.pointLights;

  if (!ambientLighting && (!pointLights || pointLights->empty()))
    return;

  const auto& transform = entity.getComponent<TransformComponent>();
  const auto& spriteComponent = entity.getComponent<SpriteComponent>();

  const auto sprite = m_assetStore.getSprite(spriteComponent.spriteId);
  if (!sprite)
    return;

  SDL_Surface* surface = m_assetStore.getSurface(sprite->textureId);
  if (!surface)
    return;

  const std::string* textureId = &sprite->textureId;
  const SDL_Rect srcRect = sprite->srcRect;
  const int textureWidth = surface->w;
  const int textureHeight = surface->h;
  const glm::vec2 worldSample = transform.position;

  int elevation = 0;
  context.terrainElevationGrid.tryGet(
      context.gridCellOf(glm::floor(worldSample)), elevation);

  const auto zoom = context.projection->zoom;

  const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x *
                                     context.projection->worldScale * zoom);
  const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y *
                                      context.projection->worldScale * zoom);
  const SDL_Rect dest{0, 0, width, height};

  // The caster's height in elevation levels (its on-screen pixel height divided
  // by the pixels-per-elevation-level). This is how far up a wall its shadow
  // can climb when standing right beside it.
  const float elevationStepPixels =
      static_cast<float>(context.projection->elevationStep) *
      context.projection->worldScale * zoom;
  const float casterHeightLevels =
      static_cast<float>(dest.h) / std::max(elevationStepPixels, 0.001f);

  const float sunStrength =
      ambientLighting ? std::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f)
                      : 0.0f;

  const float pointShadowStrength = 1.0f - sunStrength;

  std::vector<SpriteShadowCommand> items;

  auto constructTexturedSpriteShadow =
      [&](const glm::vec2& shadowDir,
          float shadowLength,
          float alpha,
          float climbScale,
          const glm::vec2* lightWorldPosition = nullptr)
  {
    if (alpha <= 0.01f || shadowLength <= 0.01f)
      return;

    if (!textureId || textureWidth <= 0 || textureHeight <= 0)
    {
      return;
    }

    auto toAtlasUv = [&](const glm::vec2& uv)
    {
      const float u0 =
          static_cast<float>(srcRect.x) / static_cast<float>(textureWidth);
      const float u1 = static_cast<float>(srcRect.x + srcRect.w) /
                       static_cast<float>(textureWidth);
      const float v0 =
          static_cast<float>(srcRect.y) / static_cast<float>(textureHeight);
      const float v1 = static_cast<float>(srcRect.y + srcRect.h) /
                       static_cast<float>(textureHeight);

      return glm::vec2{
          glm::mix(u0, u1, std::clamp(uv.x, 0.0f, 1.0f)),
          glm::mix(v0, v1, std::clamp(uv.y, 0.0f, 1.0f)),
      };
    };

    glm::vec2 dir = shadowDir;

    if (glm::length(dir) <= 0.001f)
      return;

    dir = glm::normalize(dir);

    const glm::vec2 side{-dir.y, dir.x};
    const bool pointShadow = lightWorldPosition != nullptr;

    constexpr float SpriteShadowBaseWidth = 1.0f;
    const float SpriteShadowTipWidth = 1.0f * 0.75f;

    const glm::vec2 base = worldSample;
    const glm::vec2 tip = base + dir * shadowLength;

    glm::vec2 groundPoints[4] = {
        base - side * SpriteShadowBaseWidth * 0.5f,
        base + side * SpriteShadowBaseWidth * 0.5f,
        tip + side * SpriteShadowTipWidth * 0.5f,
        tip - side * SpriteShadowTipWidth * 0.5f,
    };

    const float casterElevation = static_cast<float>(elevation);

    // Footprint tiles covered by the flat shadow, with their position resolved
    // along the shadow direction (along) and across its width (perp). Populated
    // by the ground march below and used to drive the per-column drape.
    struct FootprintTile
    {
      glm::ivec2 tile;
      float along; // distance along the shadow direction from the caster
      float perp;  // signed offset across the shadow width
      int elevation;
    };

    std::vector<FootprintTile> footprint;

    // Highest ground reached along a single column of the shadow — the line at
    // lateral offset `perp` running in the sun direction, from the caster out to
    // distance `along`. Per-column (not full-width) so a rise only shortens the
    // strip that actually climbs it: a shadow grazing a block by its edge keeps
    // its length everywhere else. Clamped to at least the caster's elevation so
    // flat ground contributes no rise. Continuity along the column is intrinsic
    // (the high-water mark only grows), so the drape never restarts.
    auto peakElevationUpTo = [&](float along, float perp)
    {
      float peak = casterElevation;

      const glm::vec2 columnStart = base + side * perp;

      walkGridDDA(columnStart,
                  dir,
                  std::max(along, 0.0f),
                  [&](const glm::ivec2& tile, float)
                  {
                    int e = 0;
                    if (context.terrainElevationGrid.tryGet(tile, e))
                      peak = std::max(peak, static_cast<float>(e));

                    return true;
                  });

      return peak;
    };

    // Silhouette length parameter is ARC LENGTH along the draped path:
    // horizontal travel plus vertical rise (climbScale * elevation above the
    // caster). This makes higher surfaces show later parts of the silhouette,
    // so the shadow climbs walls and a cliff at the sprite's feet maps the whole
    // silhouette up the face.
    auto computeShadowUv = [&](const glm::vec2& p, float surfaceElevation)
    {
      const float rise =
          climbScale * std::max(0.0f, surfaceElevation - casterElevation);

      if (!pointShadow)
      {
        const glm::vec2 local = p - base;

        float v = (glm::dot(local, dir) + rise) / shadowLength;
        v = std::clamp(v, 0.0f, 1.0f);

        const float width =
            glm::mix(SpriteShadowBaseWidth, SpriteShadowTipWidth, v);

        float u = glm::dot(local, side) / width + 0.5f;
        u = std::clamp(u, 0.0f, 1.0f);

        return glm::vec2{u, v};
      }

      const glm::vec2 light = *lightWorldPosition;
      const glm::vec2 baseA = groundPoints[0];
      const glm::vec2 baseB = groundPoints[1];
      const glm::vec2 ray = p - light;
      const glm::vec2 edge = baseB - baseA;

      auto cross = [](const glm::vec2& a, const glm::vec2& b)
      { return a.x * b.y - a.y * b.x; };

      const float denom = cross(ray, edge);

      if (std::abs(denom) < 0.0001f)
        return glm::vec2{0.5f, 0.0f};

      const glm::vec2 diff = baseA - light;
      const float edgeT = cross(diff, ray) / denom;

      float u = std::clamp(edgeT, 0.0f, 1.0f);
      float v =
          std::clamp((glm::dot(p - base, dir) + rise) / shadowLength, 0.0f, 1.0f);

      return glm::vec2{u, v};
    };

    // peakElevation is the highest ground reached so far along the ray (a
    // high-water mark), used only for the silhouette's arc-length parameter so
    // the drape stays continuous when terrain steps back down.
    auto constructSpriteShadowOnTile = [&](const glm::ivec2& tile,
                                           float peakElevation)
    {
      int receiverElevation = 0;

      if (!context.terrainElevationGrid.tryGet(tile, receiverElevation))
        return;

      // The shadow can't reach a surface at or above the caster's own top: a
      // tile at exactly the caster's height is where a clearing climb tops out,
      // so it must not also receive a ground shadow (that's the unwanted
      // continuation when the sprite is the same height as the block).
      const float casterTopElevation = casterElevation + casterHeightLevels;

      if (static_cast<float>(receiverElevation) >= casterTopElevation)
        return;

      const auto clipped = clipPolygonToTile(groundPoints, tile);

      if (clipped.count < 3)
        return;

      constexpr float ElevationSortWeight = 0.5f;

      const float sortKey =
          static_cast<float>(tile.x) + static_cast<float>(tile.y) +
          static_cast<float>(receiverElevation) * ElevationSortWeight + 0.0004f;

      for (int i = 1; i + 1 < clipped.count; i++)
      {
        const glm::vec2 p0 = clipped.points[0];
        const glm::vec2 p1 = clipped.points[i];
        const glm::vec2 p2 = clipped.points[i + 1];

        SpriteShadowCommand shadow;

        shadow.textureId = textureId;
        shadow.order.depth = sortKey;
        shadow.quad.srcRect = srcRect;
        shadow.quad.textureWidth = textureWidth;
        shadow.quad.textureHeight = textureHeight;

        shadow.quad.tint = SDL_Color{
            0,
            0,
            0,
            static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f),
        };

        shadow.quad.points[0] =
            context.worldToScreen(p0, static_cast<float>(receiverElevation));
        shadow.quad.points[1] =
            context.worldToScreen(p1, static_cast<float>(receiverElevation));
        shadow.quad.points[2] =
            context.worldToScreen(p2, static_cast<float>(receiverElevation));
        shadow.quad.points[3] = shadow.quad.points[2];

        // Render the quad at the receiver's real elevation, but sample the
        // silhouette using the high-water-mark elevation so the parameter never
        // rewinds when the ray descends past a rise.
        shadow.quad.uvs[0] = toAtlasUv(computeShadowUv(p0, peakElevation));
        shadow.quad.uvs[1] = toAtlasUv(computeShadowUv(p1, peakElevation));
        shadow.quad.uvs[2] = toAtlasUv(computeShadowUv(p2, peakElevation));
        shadow.quad.uvs[3] = shadow.quad.uvs[2];

        m_commands.push_back(shadow);
      }
    };

    // Emit a shadow quad on a vertical wall face the shadow climbs (the high
    // tile's face toward the low tile). The silhouette drapes up the face via
    // arc-length v; the climb is capped by the remaining shadow budget so a
    // tall cliff holds the whole shadow on the wall.
    auto emitWallShadowQuad =
        [&](const glm::ivec2& highTile, int wallSide, int eLow, int eHigh)
    {
      glm::vec2 wallA;
      glm::vec2 wallB;
      getTileWallEdge(highTile, wallSide, wallA, wallB);

      float tMin = 0.0f;
      float tMax = 0.0f;

      if (!projectShadowOntoWallEdge(groundPoints, wallA, wallB, tMin, tMax))
        return;

      const glm::vec2 edgeA = glm::mix(wallA, wallB, tMin);
      const glm::vec2 edgeB = glm::mix(wallA, wallB, tMax);
      const glm::vec2 mid = (edgeA + edgeB) * 0.5f;

      const float eLowF = static_cast<float>(eLow);
      const float eHighF = static_cast<float>(eHigh);

      // Budget consumed reaching this wall: horizontal travel plus the climb to
      // the wall's own base relative to the caster. Kept to the wall's base (not
      // a per-column terrain high-water mark) so it varies continuously as the
      // sun sweeps; a grid-marched high-water mark would jump tile-to-tile and
      // flicker the climbing shadows at low sun.
      const float wallAlong = glm::dot(mid - base, dir);
      const float arcBase = wallAlong +
                            climbScale * std::max(0.0f, eLowF - casterElevation);

      if (arcBase >= shadowLength)
        return; // shadow ran out before reaching this wall

      // A sprite can only cast its shadow up to its own height: cap the climb
      // at casterElevation + casterHeight. If the wall is taller than the
      // sprite, the shadow stays on the wall and never reaches the ledge above
      // (the cliff case); otherwise it climbs the whole wall and continues.
      const float heightCap = casterElevation + casterHeightLevels;
      const float budgetCap = eLowF + (shadowLength - arcBase) / climbScale;
      const float eTopClamped = std::min({eHighF, heightCap, budgetCap});

      if (eTopClamped <= eLowF + 0.001f)
        return;

      // Does the sprite clear the wall? If it's taller than the wall it climbs
      // the wall cheaply and the rest of the shadow continues on the ledge
      // above. If not (sprite <= block), the whole silhouette lands on the wall
      // (force the top to v=1) and nothing continues above.
      const bool clearsWall = eHighF < heightCap - 0.001f;

      SpriteShadowCommand shadow;
      shadow.textureId = textureId;

      constexpr float ElevationSortWeight = 0.5f;
      // Sit just in front of the high block's face (which is drawn at its own
      // elevation) so the climbing shadow paints over the wall.
      shadow.order.depth = static_cast<float>(highTile.x) +
                           static_cast<float>(highTile.y) +
                           eHighF * ElevationSortWeight + 0.0006f;

      shadow.quad.srcRect = srcRect;
      shadow.quad.textureWidth = textureWidth;
      shadow.quad.textureHeight = textureHeight;

      shadow.quad.tint = SDL_Color{
          0,
          0,
          0,
          static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f),
      };

      shadow.quad.points[0] = context.worldToScreen(edgeA, eLowF);
      shadow.quad.points[1] = context.worldToScreen(edgeB, eLowF);
      shadow.quad.points[2] = context.worldToScreen(edgeB, eTopClamped);
      shadow.quad.points[3] = context.worldToScreen(edgeA, eTopClamped);

      glm::vec2 uvTopB = computeShadowUv(edgeB, eTopClamped);
      glm::vec2 uvTopA = computeShadowUv(edgeA, eTopClamped);

      if (!clearsWall)
      {
        // Whole silhouette on the wall: stretch it to the top of the climb.
        uvTopB.y = 1.0f;
        uvTopA.y = 1.0f;
      }

      shadow.quad.uvs[0] = toAtlasUv(computeShadowUv(edgeA, eLowF));
      shadow.quad.uvs[1] = toAtlasUv(computeShadowUv(edgeB, eLowF));
      shadow.quad.uvs[2] = toAtlasUv(uvTopB);
      shadow.quad.uvs[3] = toAtlasUv(uvTopA);

      m_commands.push_back(shadow);
    };

    std::unordered_set<glm::ivec2, IVec2Hash> visited;

    // Collect every footprint tile (both side rails of the quad), then drape.
    auto collectTile = [&](const glm::ivec2& tile, float)
    {
      if (!visited.insert(tile).second)
        return true;

      int e = 0;
      if (context.terrainElevationGrid.tryGet(tile, e))
      {
        const glm::vec2 center = glm::vec2(tile) + 0.5f;
        const glm::vec2 offset = center - base;
        footprint.push_back(
            {tile, glm::dot(offset, dir), glm::dot(offset, side), e});
      }

      return true;
    };

    walkGridDDA(groundPoints[0], dir, shadowLength, collectTile);
    walkGridDDA(groundPoints[1], dir, shadowLength, collectTile);

    // The silhouette's arc-length parameter must stay continuous as the shadow
    // drapes over terrain. Each tile samples the silhouette using the high-water
    // mark of the highest ground reached along its own column, so a partial/side
    // climb shortens only the strip that climbs while the rest keeps its length.
    // The shadow can never climb above the caster's own top, so terrain that
    // reaches that height blocks it entirely (the same cap the wall climb uses).
    const float casterTop = casterElevation + casterHeightLevels;

    for (const FootprintTile& ft : footprint)
    {
      const float peak = std::max(peakElevationUpTo(ft.along, ft.perp),
                                  static_cast<float>(ft.elevation));

      // Terrain at or above the caster's top blocks the shadow: it can't climb
      // higher, so this tile and everything farther along the column (the far
      // side of a mountain, a ledge above a too-tall wall) lie in the terrain's
      // own shadow rather than the caster's. The high-water mark carries the
      // block forward, and it applies toward the camera too, where no wall climb
      // shows the step.
      if (peak >= casterTop)
        continue;

      // Drop tiles the shadow's arc length doesn't reach. Climbing to an
      // elevated tile spends extra arc (climbScale * rise), so a ledge past a
      // wall the shadow only partly climbs lies beyond the shadow's end even
      // though it still falls inside the flat horizontal march. The tile's
      // nearest corner is used so the tile holding the shadow's tip survives.
      const float rise = climbScale * std::max(0.0f, peak - casterElevation);

      float nearestAlong = glm::dot(glm::vec2(ft.tile) - base, dir);
      for (int corner = 1; corner < 4; corner++)
      {
        const glm::vec2 c = glm::vec2(ft.tile) +
                            glm::vec2(static_cast<float>(corner & 1),
                                      static_cast<float>((corner >> 1) & 1));
        nearestAlong = std::min(nearestAlong, glm::dot(c - base, dir));
      }

      if (nearestAlong + rise >= shadowLength)
        continue;

      constructSpriteShadowOnTile(ft.tile, peak);
    }

    // After the ground march, climb walls: for each covered tile, a higher
    // neighbour the shadow heads into (dot with dir > 0) is an up-step wall.
    //
    // Only the high tile's camera-facing faces are visible in this iso view
    // (+x East and +y South go toward the camera; West/North are hidden behind
    // the block), so we only climb those. The high tile sits to the West of the
    // low tile (its East face, side 2) or to the North (its South face, side 3).
    if (climbScale > 0.0f)
    {
      const glm::ivec2 offsets[2] = {{-1, 0}, {0, -1}};
      const int sideForOffset[2] = {2, 3}; // East, South — the visible faces

      for (const glm::ivec2& low : visited)
      {
        int eLow = 0;
        if (!context.terrainElevationGrid.tryGet(low, eLow))
          continue;

        for (int oi = 0; oi < 2; oi++)
        {
          if (glm::dot(glm::vec2(offsets[oi]), dir) <= 0.0f)
            continue;

          const glm::ivec2 high = low + offsets[oi];

          int eHigh = 0;
          if (!context.terrainElevationGrid.tryGet(high, eHigh))
            continue;

          if (eHigh <= eLow)
            continue;

          emitWallShadowQuad(high, sideForOffset[oi], eLow, eHigh);
        }
      }
    }
  };

  if (ambientLighting)
  {
    const glm::vec3 sunDir3D = ambientLighting->direction;
    const float diffuse =
        std::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f);

    if (sunDir3D.z > 0.02f && diffuse > 0.01f)
    {
      glm::vec2 shadowDir{-sunDir3D.x, -sunDir3D.y};
      const float horizontalAmount = glm::length(shadowDir);

      if (horizontalAmount > 0.001f)
      {
        shadowDir /= horizontalAmount;

        const float sunHeight = std::max(sunDir3D.z, 0.08f);

        // Shadow length is the caster's actual height projected by the sun. A
        // caster taller than a wall therefore climbs the whole wall and spills
        // onto the ground above; max climb (right beside the wall) equals the
        // caster height in levels.
        float shadowLength = casterHeightLevels * horizontalAmount / sunHeight;

        // Cap to avoid runaway shadows at grazing sun, but scale the cap with
        // caster height so tall casters keep their reach.
        shadowLength =
            std::min(shadowLength,
                     m_shadowSettings.spriteShadowMaxLength *
                         std::max(1.0f, casterHeightLevels));

        const float alpha = m_shadowSettings.spriteShadowAlpha * diffuse;

        // Climbing a wall costs the same shadow budget per elevation level as
        // travelling one tile on the ground (1:1), so a shadow drapes cheaply
        // up a wall and keeps going on the ledge above. The caster-height cap
        // in the wall projection still stops a short sprite from clearing a
        // tall block.
        const float climbScale = 1.0f;

        constructTexturedSpriteShadow(shadowDir, shadowLength, alpha, climbScale);
      }
    }
  }

  if (!pointLights)
    return;

  for (const auto& light : *pointLights)
  {
    const glm::vec2 toCaster = worldSample - light.worldPosition;
    const float distance = glm::length(toCaster);

    if (distance <= 0.001f || distance >= light.radius)
      continue;

    float attenuation = 1.0f - distance / light.radius;
    attenuation = std::clamp(attenuation, 0.0f, 1.0f);
    attenuation *= attenuation;

    const float lightFactor =
        std::clamp(light.intensity * attenuation, 0.0f, 1.0f);

    const float alpha = std::clamp(
        m_shadowSettings.spriteShadowAlpha * lightFactor * pointShadowStrength,
        0.0f,
        1.0f);

    if (alpha <= 0.01f)
      continue;

    constexpr float SpriteCasterHeightWorld = 1.0f;

    const float elevationStepPixels =
        static_cast<float>(context.projection->elevationStep) *
        context.projection->worldScale;
    const float lightHeightWorld =
        std::max(light.height / elevationStepPixels, 0.08f);

    float shadowLength = SpriteCasterHeightWorld * distance / lightHeightWorld;
    shadowLength =
        std::clamp(shadowLength, 0.15f, m_shadowSettings.spriteShadowMaxLength);

    // Point-light sprite shadows stay flat for now (climb is phase 3).
    constructTexturedSpriteShadow(
        toCaster, shadowLength, alpha, 0.0f, &light.worldPosition);
  }
}

} // namespace sfs
