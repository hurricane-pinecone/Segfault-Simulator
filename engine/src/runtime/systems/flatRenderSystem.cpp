#include "engine/runtime/systems/flatRenderSystem.h"

#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/spriteComponent.h" // SpriteComponent + NormalMapComponent
#include "engine/core/components/spriteTint.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h" // IWYU pragma: keep -- Entity::getComponent<T> defs
#include "engine/core/particles/particleBatch.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "engine/runtime/rendering/quads.h"
#include "glm/glm/geometric.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>
#include <SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>
#include <vector>

namespace sfs
{

FlatRenderSystem::FlatRenderSystem(AssetStore& assetStore,
                                   IQuadRenderer& quadRenderer)
    : m_assetStore(assetStore), m_quadRenderer(quadRenderer)
{
}

void FlatRenderSystem::create()
{
  registerComponent<TransformComponent>();
  registerComponent<SpriteComponent>();
}

namespace
{
// A sprite gathered for this frame, with its painter sort-key (layer, then world
// Y) resolved to a clip-space depth in a second pass once the frame's key range
// is known -- so the depth buffer orders sprites without the render system
// caring about submission order.
struct GatheredSprite
{
  LitQuad quad;
  float sortKey = 0.0f;
};

unsigned int
resolveSpriteTexture(AssetStore& store, std::uint32_t spriteId, SDL_Rect& srcOut,
                     int& texWOut, int& texHOut, IQuadRenderer& renderer)
{
  const Sprite* sprite = store.getSprite(spriteId);
  if (!sprite)
    return 0;

  SDL_Surface* surface = store.getSurface(sprite->textureId);
  if (!surface)
    return 0;

  srcOut = sprite->srcRect;
  texWOut = surface->w;
  texHOut = surface->h;
  return renderer.getOrCreateTexture(sprite->textureId, surface);
}
} // namespace

void FlatRenderSystem::render()
{
  if (!m_projection)
    return;

  m_context.projection = m_projection;

  // Gather point-light candidates (world space), then keep at most
  // MaxShaderLights -- the nearest to the camera focus when there are more, so
  // the cap degrades gracefully instead of dropping arbitrary lights.
  struct LightCand
  {
    glm::vec2 worldPos;
    glm::vec3 color;
    float intensity;
    float radius;
  };
  std::vector<LightCand> cands;
  if (registry)
  {
    for (const auto& entity :
         registry->view<TransformComponent, LightEmitterComponent>())
    {
      const auto& transform = entity.getComponent<TransformComponent>();
      const auto& emitter = entity.getComponent<LightEmitterComponent>();
      cands.push_back({transform.position, emitter.color, emitter.intensity,
                       emitter.radius});
    }
  }

  if (static_cast<int>(cands.size()) > MaxShaderLights)
  {
    const glm::vec2 focus = m_focus;
    std::nth_element(
        cands.begin(), cands.begin() + MaxShaderLights, cands.end(),
        [&focus](const LightCand& a, const LightCand& b)
        {
          const glm::vec2 da = a.worldPos - focus;
          const glm::vec2 db = b.worldPos - focus;
          return glm::dot(da, da) < glm::dot(db, db);
        });
    cands.resize(MaxShaderLights);
  }

  // Point lights are submitted in screen space (worldPoints below are
  // screen-space too, so distances and radii are consistent without a
  // heightfield).
  PointLightSet lights{};
  for (const LightCand& c : cands)
  {
    const int i = lights.count++;
    lights.positions[i] = m_projection->worldToScreen(c.worldPos, 0.0f);
    lights.colors[i] = c.color;
    lights.intensities[i] = c.intensity;
    lights.radii[i] = c.radius;
    lights.heights[i] = 0.0f;       // flat: no elevation
    lights.groundLevels[i] = 0.0f;
  }
  m_quadRenderer.setPointLights(lights);

  m_quadRenderer.begin();
  m_quadRenderer.setSurfaceTime(static_cast<float>(SDL_GetTicks()) / 1000.0f);

  // Gather sprites, then resolve depth across the frame's sort-key range.
  std::vector<GatheredSprite> sprites;
  sprites.reserve(getEntities().size());

  float minKey = std::numeric_limits<float>::max();
  float maxKey = std::numeric_limits<float>::lowest();

  for (const auto& entity : getEntities())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& spriteComponent = entity.getComponent<SpriteComponent>();

    SDL_Rect srcRect{};
    int texW = 0;
    int texH = 0;
    const unsigned int texture = resolveSpriteTexture(
        m_assetStore, spriteComponent.spriteId, srcRect, texW, texH,
        m_quadRenderer);
    if (texture == 0)
      continue;

    // Sprite size scales with the camera zoom (worldUnitToPixels), matching the
    // zoom worldToScreen applies to position.
    const float zoom = m_projection->worldUnitToPixels();
    const float w = srcRect.w * transform.scale.x * zoom;
    const float h = srcRect.h * transform.scale.y * zoom;

    const glm::vec2 screen =
        m_projection->worldToScreen(transform.position, 0.0f) +
        spriteComponent.renderOffset;

    const float anchorX = spriteComponent.anchor.x * w;
    const float anchorY = spriteComponent.anchor.y * h;
    const float left = screen.x - anchorX;
    const float top = screen.y - anchorY;

    LitQuad quad;
    quad.texture = texture;
    quad.srcRect = srcRect;
    quad.destRect = SDL_Rect{static_cast<int>(std::round(left)),
                             static_cast<int>(std::round(top)),
                             static_cast<int>(std::round(w)),
                             static_cast<int>(std::round(h))};
    quad.textureWidth = texW;
    quad.textureHeight = texH;
    quad.rotation = static_cast<float>(transform.rotation);

    // Lighting coords in screen space, matching the point-light positions.
    quad.worldPoints[0] = {left, top};
    quad.worldPoints[1] = {left + w, top};
    quad.worldPoints[2] = {left + w, top + h};
    quad.worldPoints[3] = {left, top + h};

    quad.ambient = m_lighting.ambient;
    quad.lightColor = m_lighting.ambientColor;
    quad.lightDirection = m_lighting.sunDirection;
    quad.diffuseStrength = m_lighting.diffuseStrength;

    if (entity.hasComponent<SpriteTint>())
    {
      const auto& tint = entity.getComponent<SpriteTint>();
      const auto to8 = [](float v)
      { return static_cast<Uint8>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
      quad.tint = SDL_Color{to8(tint.color.r), to8(tint.color.g),
                            to8(tint.color.b), to8(tint.alpha)};
    }

    if (entity.hasComponent<NormalMapComponent>())
    {
      const auto& nm = entity.getComponent<NormalMapComponent>();
      SDL_Rect nSrc{};
      int nW = 0;
      int nH = 0;
      const unsigned int normalTex = resolveSpriteTexture(
          m_assetStore, nm.spriteId, nSrc, nW, nH, m_quadRenderer);
      if (normalTex != 0)
      {
        quad.normalTexture = normalTex;
        quad.hasNormalMap = true;
      }
    }

    const int layer = entity.hasComponent<RenderLayerComponent>()
                          ? entity.getComponent<RenderLayerComponent>().layer
                          : 0;
    const float sortKey =
        static_cast<float>(layer) * 1.0e6f + transform.position.y;

    minKey = std::min(minKey, sortKey);
    maxKey = std::max(maxKey, sortKey);
    sprites.push_back({quad, sortKey});
  }

  // Map sort-key -> clip depth: higher key (foreground / larger Y) gets the
  // nearer depth so it draws in front under the GL_LEQUAL depth test.
  const float range = maxKey - minKey;
  for (auto& gathered : sprites)
  {
    const float t = range > 1e-6f ? (gathered.sortKey - minKey) / range : 0.0f;
    gathered.quad.z = 0.9f - 1.8f * t;
    m_quadRenderer.submit(gathered.quad);
  }

  // Drive render modules (e.g. Particles). They emit AnyRenderCommands; the flat
  // path consumes particle batches and draws them on top (screen-space), so the
  // 2D scene needs no depth coupling between sprites and effects.
  std::vector<AnyRenderCommand> scratch;
  for (auto& [type, module] : m_modules)
  {
    scratch.clear();
    module->emit(m_context, scratch);

    for (const auto& command : scratch)
    {
      std::visit(
          [&](const auto& concrete)
          {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, ParticleBatchCommand>)
            {
              if (!concrete.textureId)
                return;
              SDL_Surface* surface = m_assetStore.getSurface(*concrete.textureId);
              if (!surface)
                return;
              const unsigned int texture =
                  m_quadRenderer.getOrCreateTexture(*concrete.textureId, surface);
              if (texture != 0)
              {
                // buildBatches sets each quad's z to a raw world sort-key for
                // the iso depth pass (assignClipDepth) to normalise. The flat
                // path draws particles as an on-top overlay (depthTested=false),
                // so it needs no depth ordering -- but the raw z would fall
                // outside the clip volume and cull the quad. Flatten it to 0.
                ParticleBatch batch = concrete.quad;
                for (ParticleQuad& q : batch.quads)
                  q.z = 0.0f;
                m_quadRenderer.submitParticleBatch(
                    batch, texture, concrete.blend, false);
              }
            }
          },
          command);
    }
  }

  m_quadRenderer.flush();
}

} // namespace sfs
