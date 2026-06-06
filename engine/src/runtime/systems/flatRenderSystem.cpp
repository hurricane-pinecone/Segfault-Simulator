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
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <SDL_pixels.h>
#include <SDL_rect.h>
#include <SDL_timer.h>
#include <algorithm>
#include <limits>
#include <unordered_map>
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

// A decal corner carried through clipping: screen position + its texture UV, so a
// slice gets the right UV at the cut.
struct ClipVert
{
  glm::vec2 p{0.0f, 0.0f};
  glm::vec2 uv{0.0f, 0.0f};
};

// Clip a polygon against one axis-aligned half-plane (Sutherland-Hodgman): keep
// the side where coord >= limit (keepGreater) or coord <= limit, interpolating
// position + UV at each crossing. Returns the new vertex count.
int clipHalfPlane(const ClipVert* in, int n, int axis, float limit,
                  bool keepGreater, ClipVert* out)
{
  int m = 0;
  for (int i = 0; i < n; ++i)
  {
    const ClipVert& a = in[i];
    const ClipVert& b = in[(i + 1) % n];
    const float ca = axis == 0 ? a.p.x : a.p.y;
    const float cb = axis == 0 ? b.p.x : b.p.y;
    const bool aIn = keepGreater ? ca >= limit : ca <= limit;
    const bool bIn = keepGreater ? cb >= limit : cb <= limit;
    if (aIn)
      out[m++] = a;
    if (aIn != bIn)
    {
      const float denom = cb - ca;
      const float s = denom != 0.0f ? (limit - ca) / denom : 0.0f;
      out[m++] = ClipVert{a.p + (b.p - a.p) * s, a.uv + (b.uv - a.uv) * s};
    }
  }
  return m;
}

// Clip a (possibly rotated) quad to an axis-aligned screen rect. Returns the
// clipped convex polygon in `out` (up to 8 verts); count < 3 means fully clipped.
int clipQuadToRect(const glm::vec2* pts, const glm::vec2* uvs, float left,
                   float top, float right, float bottom, ClipVert* out)
{
  ClipVert a[12];
  ClipVert b[12];
  for (int i = 0; i < 4; ++i)
    a[i] = ClipVert{pts[i], uvs[i]};
  int n = clipHalfPlane(a, 4, 0, left, true, b);
  n = clipHalfPlane(b, n, 0, right, false, a);
  n = clipHalfPlane(a, n, 1, top, true, b);
  n = clipHalfPlane(b, n, 1, bottom, false, out);
  return n;
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
    quad.destRect = SDL_Rect{static_cast<int>(glm::round(left)),
                             static_cast<int>(glm::round(top)),
                             static_cast<int>(glm::round(w)),
                             static_cast<int>(glm::round(h))};
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
      { return static_cast<Uint8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f); };
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

    minKey = glm::min(minKey, sortKey);
    maxKey = glm::max(maxKey, sortKey);
    sprites.push_back({quad, sortKey});
  }

  // Decals (blood, scorch...) draw as a blended, depth-TESTED overlay via the
  // particle path (depth test on, depth WRITE off) rather than the lit pipeline.
  // That's what makes accumulating blood look right: the lit pipeline writes
  // depth even for a soft sprite's transparent rim, so each new blob CLEARED the
  // one behind it (hard artifacts, no blending). With write off they layer and
  // blend in stamp order; with test on they're still occluded by nearer
  // characters. They contribute to the shared depth range first.
  for (const FlatDecal& decal : m_decals)
  {
    const float sortKey =
        static_cast<float>(decal.layer) * 1.0e6f + decal.worldPos.y;
    minKey = glm::min(minKey, sortKey);
    maxKey = glm::max(maxKey, sortKey);
  }

  // Map sort-key -> clip depth: higher key (foreground / larger Y) gets the
  // nearer depth so it draws in front under the GL_LEQUAL depth test.
  const float range = maxKey - minKey;
  for (auto& gathered : sprites)
  {
    const float t = range > 1e-6f ? (gathered.sortKey - minKey) / range : 0.0f;
    gathered.quad.z = 0.9f - 1.8f * t;
    // Flat quads have no depth lean (that's an isometric standing-billboard
    // feature the iso path fills via assignClipDepth). Without this the top edge
    // keeps the default zTop = 0 while the bottom uses z, so every quad gets a
    // spurious top-to-bottom depth gradient -- overlapping quads (e.g. a blood
    // pool's many ribbons) then fight and cull each other. Flat = top depth ==
    // bottom depth.
    gathered.quad.zTop = gathered.quad.z;
    m_quadRenderer.submit(gathered.quad);
  }

  // Build decal quads and submit them through the particle path (blended, depth
  // test on / write off) AFTER the lit sprites have written the depth buffer, so
  // blood is occluded by nearer characters yet blends over older blood instead
  // of clearing it. Bucketed by texture (one draw per texture).
  const float zoom = m_projection->worldUnitToPixels();
  std::unordered_map<unsigned int, ParticleBatch> decalBatches;
  for (const FlatDecal& decal : m_decals)
  {
    SDL_Rect srcRect{};
    int texW = 0;
    int texH = 0;
    const unsigned int texture = resolveSpriteTexture(
        m_assetStore, decal.sprite, srcRect, texW, texH, m_quadRenderer);
    if (texture == 0 || texW == 0 || texH == 0)
      continue;

    const float w = decal.size.x * zoom;
    const float h = decal.size.y * zoom;
    const glm::vec2 screen = m_projection->worldToScreen(decal.worldPos, 0.0f);
    // Quad centre (honour the anchor; blood is centre-anchored).
    const glm::vec2 c{screen.x - (decal.anchor.x - 0.5f) * w,
                      screen.y - (decal.anchor.y - 0.5f) * h};

    float u0 = static_cast<float>(srcRect.x) / texW;
    float v0 = static_cast<float>(srcRect.y) / texH;
    float u1 = static_cast<float>(srcRect.x + srcRect.w) / texW;
    float v1 = static_cast<float>(srcRect.y + srcRect.h) / texH;

    ParticleQuad q;
    const float sortKey =
        static_cast<float>(decal.layer) * 1.0e6f + decal.worldPos.y;
    const float t = range > 1e-6f ? (sortKey - minKey) / range : 0.0f;
    q.z = 0.9f - 1.8f * t;

    // The particle path is unlit, so bake the scene lighting at the decal's
    // position into its colour: a dim ambient base plus each point light's
    // smootherstep falloff. Blood is dark in shadow and lit near torches and
    // muzzle/death flashes.
    glm::vec3 lightAcc = m_lighting.ambient * m_lighting.ambientColor;
    for (int li = 0; li < lights.count; ++li)
    {
      const float radius = lights.radii[li];
      if (radius <= 0.0f)
        continue;
      const float dist = glm::length(lights.positions[li] - screen);
      if (dist >= radius)
        continue;
      const float reach = 1.0f - dist / radius;
      const float atten =
          reach * reach * reach * (reach * (reach * 6.0f - 15.0f) + 10.0f);
      glm::vec3 lc = lights.colors[li];
      const float mx = glm::max(glm::max(lc.r, lc.g), lc.b);
      lc = mx > 0.001f ? lc / mx : glm::vec3(1.0f);
      lightAcc += lc * (lights.intensities[li] * atten);
    }
    q.color = {(decal.tint.r / 255.0f) * glm::min(lightAcc.r, 1.0f),
               (decal.tint.g / 255.0f) * glm::min(lightAcc.g, 1.0f),
               (decal.tint.b / 255.0f) * glm::min(lightAcc.b, 1.0f),
               decal.tint.a / 255.0f};

    // The (possibly rotated) quad corners in screen space, with their UVs.
    glm::vec2 p0{-w * 0.5f, -h * 0.5f};
    glm::vec2 p1{w * 0.5f, -h * 0.5f};
    glm::vec2 p2{w * 0.5f, h * 0.5f};
    glm::vec2 p3{-w * 0.5f, h * 0.5f};
    if (decal.rotation != 0.0f)
    {
      const float s = glm::sin(decal.rotation);
      const float co = glm::cos(decal.rotation);
      const auto rot = [&](glm::vec2 p)
      { return glm::vec2{p.x * co - p.y * s, p.x * s + p.y * co}; };
      p0 = rot(p0);
      p1 = rot(p1);
      p2 = rot(p2);
      p3 = rot(p3);
    }
    const glm::vec2 pts[4] = {c + p0, c + p1, c + p2, c + p3};
    const glm::vec2 uvArr[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};

    // Clip to the surface the decal stuck to (default), so a mark never spills
    // past the platform edge -- even a rotated streak. Clipping::None opts out
    // (e.g. a free-floating mark). A zero rect means "no surface" -> no clip.
    const bool doClip = decal.clipping == Clipping::Surface &&
                        decal.clipMax.x > decal.clipMin.x &&
                        decal.clipMax.y > decal.clipMin.y;
    if (!doClip)
    {
      for (int i = 0; i < 4; ++i)
      {
        q.points[i] = pts[i];
        q.uvs[i] = uvArr[i];
      }
      decalBatches[texture].quads.push_back(q);
      continue;
    }

    const glm::vec2 cc0 = m_projection->worldToScreen(decal.clipMin, 0.0f);
    const glm::vec2 cc1 = m_projection->worldToScreen(decal.clipMax, 0.0f);
    ClipVert poly[12];
    const int count = clipQuadToRect(pts,
                                     uvArr,
                                     glm::min(cc0.x, cc1.x),
                                     glm::min(cc0.y, cc1.y),
                                     glm::max(cc0.x, cc1.x),
                                     glm::max(cc0.y, cc1.y),
                                     poly);
    if (count < 3)
      continue;

    // Emit the clipped convex polygon as a triangle fan, packed one triangle per
    // quad (4th corner duplicated -> a degenerate second triangle).
    for (int i = 1; i + 1 < count; ++i)
    {
      ParticleQuad fan = q; // keeps colour + depth
      fan.points[0] = poly[0].p;
      fan.points[1] = poly[i].p;
      fan.points[2] = poly[i + 1].p;
      fan.points[3] = poly[i + 1].p;
      fan.uvs[0] = poly[0].uv;
      fan.uvs[1] = poly[i].uv;
      fan.uvs[2] = poly[i + 1].uv;
      fan.uvs[3] = poly[i + 1].uv;
      decalBatches[texture].quads.push_back(fan);
    }
  }
  for (auto& [texture, batch] : decalBatches)
    m_quadRenderer.submitParticleBatch(batch, texture, BlendMode::Alpha, true);

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

long long FlatRenderSystem::decalCell(const glm::vec2& worldPos) const
{
  const int cx = static_cast<int>(glm::floor(worldPos.x / m_decalCellSize));
  const int cy = static_cast<int>(glm::floor(worldPos.y / m_decalCellSize));
  // Pack two int cell coords into one key (clean bijection via unsigned halves).
  return (static_cast<long long>(static_cast<unsigned int>(cx)) << 32) |
         static_cast<unsigned int>(cy);
}

void FlatRenderSystem::stampDecal(const FlatDecal& decal)
{
  // Permanent decals (blood etc.) saturate a world cell: once it holds enough,
  // further stamps there are dropped, so a hammered spot stays put instead of
  // churning the ring buffer (which would erase older marks). Fading decals
  // (lifetime >= 0) clear themselves, so they don't participate.
  const bool permanent = decal.lifetime < 0.0f && m_decalsPerCell > 0;
  if (permanent)
  {
    const auto it = m_decalCoverage.find(decalCell(decal.worldPos));
    if (it != m_decalCoverage.end() && it->second >= m_decalsPerCell)
      return;
  }

  if (static_cast<int>(m_decals.size()) >= m_maxDecals)
  {
    // Global backstop: drop the oldest, keeping its cell's coverage in step.
    const FlatDecal& oldest = m_decals.front();
    if (oldest.lifetime < 0.0f && m_decalsPerCell > 0)
    {
      const auto oit = m_decalCoverage.find(decalCell(oldest.worldPos));
      if (oit != m_decalCoverage.end() && --oit->second <= 0)
        m_decalCoverage.erase(oit);
    }
    m_decals.erase(m_decals.begin());
  }

  m_decals.push_back(decal);
  if (permanent)
    ++m_decalCoverage[decalCell(decal.worldPos)];
}

void FlatRenderSystem::ageDecals(float deltaTime)
{
  const auto approach = [](float current, float target, float step)
  {
    return current < target ? glm::min(target, current + step)
                            : glm::max(target, current - step);
  };

  for (auto it = m_decals.begin(); it != m_decals.end();)
  {
    FlatDecal& decal = *it;

    if (decal.follow.isValid() &&
        decal.follow.hasComponent<TransformComponent>())
      decal.worldPos =
          decal.follow.getComponent<TransformComponent>().position +
          decal.followOffset;

    if (decal.growRate > 0.0f)
    {
      const float step = decal.growRate * deltaTime;
      if (decal.growTo.x > 0.0f)
        decal.size.x = approach(decal.size.x, decal.growTo.x, step);
      if (decal.growTo.y > 0.0f)
        decal.size.y = approach(decal.size.y, decal.growTo.y, step);
    }

    if (decal.lifetime >= 0.0f)
    {
      decal.lifetime -= deltaTime;
      if (decal.lifetime <= 0.0f)
      {
        it = m_decals.erase(it);
        continue;
      }
    }
    ++it;
  }
}

} // namespace sfs
