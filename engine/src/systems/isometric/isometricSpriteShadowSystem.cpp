#include "engine/systems/isometric/isometricSpriteShadowSystem.h"

#include "engine/assetStore/assetStore.h"
#include "engine/components/shadowCasterComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/rendering/util/isometric/geometry.h"
#include "engine/utils/algorithms/gridDDA.h"
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

  const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x *
                                     context.worldScale * context.zoom);
  const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y *
                                      context.worldScale * context.zoom);
  const SDL_Rect dest{0, 0, width, height};

  const float sunStrength =
      ambientLighting ? std::clamp(ambientLighting->diffuseStrength, 0.0f, 1.0f)
                      : 0.0f;

  const float pointShadowStrength = 1.0f - sunStrength;

  std::vector<SpriteShadowCommand> items;

  auto constructTexturedSpriteShadow =
      [&](const glm::vec2& shadowDir,
          float shadowLength,
          float alpha,
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

    auto computeShadowUv = [&](const glm::vec2& p)
    {
      if (!pointShadow)
      {
        const glm::vec2 local = p - base;

        float v = glm::dot(local, dir) / shadowLength;
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
      float v = std::clamp(glm::dot(p - base, dir) / shadowLength, 0.0f, 1.0f);

      return glm::vec2{u, v};
    };

    auto constructSpriteShadowOnTile = [&](const glm::ivec2& tile)
    {
      int receiverElevation = 0;

      if (!context.terrainElevationGrid.tryGet(tile, receiverElevation))
        return;

      const float elevationStepPixels =
          static_cast<float>(context.elevationStep) * context.worldScale *
          context.zoom;
      const float spriteHeightElevation =
          static_cast<float>(dest.h) / std::max(elevationStepPixels, 0.001f);
      const float maxShadowReceiverElevation =
          static_cast<float>(elevation) + spriteHeightElevation;

      if (static_cast<float>(receiverElevation) > maxShadowReceiverElevation)
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

        shadow.quad.uvs[0] = toAtlasUv(computeShadowUv(p0));
        shadow.quad.uvs[1] = toAtlasUv(computeShadowUv(p1));
        shadow.quad.uvs[2] = toAtlasUv(computeShadowUv(p2));
        shadow.quad.uvs[3] = shadow.quad.uvs[2];

        m_commands.push_back(shadow);
      }
    };

    std::unordered_set<glm::ivec2, IVec2Hash> visited;

    auto visitSpriteShadowTile = [&](const glm::ivec2& tile, float)
    {
      if (!visited.insert(tile).second)
        return true;

      constructSpriteShadowOnTile(tile);
      return true;
    };

    walkGridDDA(groundPoints[0], dir, shadowLength, visitSpriteShadowTile);
    walkGridDDA(groundPoints[1], dir, shadowLength, visitSpriteShadowTile);
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

        constexpr float SpriteCasterHeight = 0.75f;

        const float shadowLength =
            std::min(m_shadowSettings.spriteShadowMaxLength,
                     SpriteCasterHeight * horizontalAmount / sunHeight);
        const float alpha = m_shadowSettings.spriteShadowAlpha * diffuse;

        constructTexturedSpriteShadow(shadowDir, shadowLength, alpha);
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
        static_cast<float>(context.elevationStep) * context.worldScale;
    const float lightHeightWorld =
        std::max(light.height / elevationStepPixels, 0.08f);

    float shadowLength = SpriteCasterHeightWorld * distance / lightHeightWorld;
    shadowLength =
        std::clamp(shadowLength, 0.15f, m_shadowSettings.spriteShadowMaxLength);

    constructTexturedSpriteShadow(
        toCaster, shadowLength, alpha, &light.worldPosition);
  }
}

} // namespace sfs
