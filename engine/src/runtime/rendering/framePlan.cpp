#include "engine/runtime/rendering/framePlan.h"

#include "engine/core/rendering/frustum.h"
#include "engine/core/rendering/materialGraphCompiler.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/constants.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>

namespace sfs
{

namespace
{
// How a grass field's grown, by distance (fractions of its draw distance):
// to where blades have 4 segments, then 2 (then 1); and, thinned the old way
// (grassLegacyThinning()), each lattice's reach.
constexpr std::array<float, 4> kGrassReach = {0.2f, 0.4f, 0.65f, 1.0f};
constexpr float kGrassFineDetail = 0.12f;
constexpr float kGrassMidDetail = 0.35f;
// Past here (m), where a blade is a few pixels across (and past the nearest
// list's blades, and the reach of their shadows), the sun's shadow on it is
// found at its vertices, a blade at a time over the band past it (see
// meshGrass.wgsl).
constexpr float kGrassVertexSunFrom = 30.0f;
constexpr float kGrassVertexSunBand = 10.0f;
// How many blades are kept by distance (see grassKeptShare()): all of them
// out to this many times the spacing of the finest lattice's cells (so
// denser grass thins out sooner, about where its blades crowd as much on
// screen), but no further than this share of the draw distance; past it,
// fewer by this power of the distance; and from this share of the draw
// distance, fewer still, to none at it.
constexpr float kGrassFullCells = 770.0f;
constexpr float kGrassMostFull = 0.3f;
constexpr float kGrassThinning = 2.3f;
constexpr float kGrassFadeFrom = 0.6f;
// Over how much of its rank a blade grows in (meshGrassGrow.wgsl's
// kGrowIn): the most of a lattice's blades that can be part grown.
constexpr float kGrassGrowIn = 0.2f;
// Most blades a field grows a frame (its lists all together), and the most
// cells a side of a lattice.
constexpr double kGrassMostBlades = 4.0e6;
constexpr int kGrassMostCells = 3072;
// How much grass's colour covers the ground it grows on, where it's full.
constexpr float kGrassGroundTint = 0.9f;
// How much wider a blade's shadow is than the blade: a little, so a field's
// shadows hold together at the shadow map's resolution rather than flicker.
constexpr float kGrassShadowWidth = 1.6f;
// How much brighter a physical sky's moon's disk is than its light on a
// surface facing it.
constexpr float kMoonDiskGlow = 120.0f;

// The bearings (radians, in a ground's own XY, `toOwn` from the world) the
// view spans seen from above, from its corners' (the first no more than the
// second, and less than a half turn apart); nothing if it looks too steeply
// down or up for them to say, or spans too much.
std::optional<glm::vec2> viewBearings(const FrameParams& frame,
                                      const glm::mat4& toOwn)
{
  const glm::mat4 toWorld = glm::inverse(frame.viewProjection);
  const glm::mat3 turn(toOwn);
  const glm::vec3 ahead = turn * frame.cameraForward;
  if (glm::length(glm::vec2(ahead)) < 0.2f * glm::length(ahead))
    return std::nullopt;
  const float middle = std::atan2(ahead.y, ahead.x);
  glm::vec2 span(0.0f);
  for (int corner = 0; corner < 4; ++corner)
  {
    const glm::vec4 clip(
        corner & 1 ? 1.0f : -1.0f, corner & 2 ? 1.0f : -1.0f, 0.5f, 1.0f);
    const glm::vec4 point = toWorld * clip;
    const glm::vec3 ray =
        turn * (glm::vec3(point) / point.w - frame.cameraPosition);
    if (glm::length(glm::vec2(ray)) < 0.2f * glm::length(ray))
      return std::nullopt;
    float off = std::atan2(ray.y, ray.x) - middle;
    off = std::remainder(off, glm::two_pi<float>());
    span = glm::vec2(std::min(span.x, off), std::max(span.y, off));
  }
  // (A little more, for blades leaning in from either side.)
  span += glm::vec2(-0.1f, 0.1f);
  if (span.y - span.x >= glm::pi<float>())
    return std::nullopt;
  return span + middle;
}
} // namespace

MaterialUniforms standardMaterialUniforms(const MeshMaterial& material)
{
  return {material.baseColor,
          glm::vec4(material.normalScale,
                    material.metallic,
                    material.roughness,
                    material.alphaCutoff),
          glm::vec4(material.tiling, material.offset),
          glm::uvec4(static_cast<uint32_t>(material.alphaMode),
                     material.unlit ? 1u : 0u,
                     0,
                     0)};
}

TerrainUniforms terrainMaterialUniforms(const TerrainMaterial& terrain,
                                        const TerrainPuddles* puddles,
                                        const TerrainDeformation* deformation,
                                        bool patch)
{
  TerrainUniforms out{};
  const float sharpness = std::clamp(terrain.triplanarSharpness, 0.0f, 1.0f);
  const std::size_t count =
      std::min(terrain.layers.size(), std::size_t{kMaxTerrainLayers});
  for (std::size_t i = 0; i < kMaxTerrainLayers; ++i)
  {
    // A layer it doesn't have: as the first (or plain white).
    const TerrainLayer layer =
        i < count ? terrain.layers[i]
                  : (count > 0 ? terrain.layers[0] : TerrainLayer{});
    out.layers[i] = glm::vec4(glm::max(layer.tint, glm::vec3(0.0f)),
                              1.0f / std::max(layer.tileSize, 0.01f));
    out.detail[i] = glm::vec4(std::clamp(layer.roughness, 0.0f, 1.0f),
                              layer.normalScale,
                              std::clamp(layer.meadow, 0.0f, 1.0f),
                              // Its side projections' blend: the normal's
                              // weights raised to this (0: from above only).
                              layer.triplanar ? 1.0f + 3.0f * sharpness : 0.0f);
  }
  out.params = glm::vec4(static_cast<float>(std::max<std::size_t>(count, 1)),
                         std::clamp(terrain.blendSharpness, 0.0f, 1.0f),
                         terrain.antiTiling ? 1.0f : 0.0f,
                         std::clamp(terrain.variation, 0.0f, 1.0f));
  for (std::size_t i = 0; i < kMaxTerrainLayers; ++i)
  {
    const TerrainLayer layer =
        i < count ? terrain.layers[i]
                  : (count > 0 ? terrain.layers[0] : TerrainLayer{});
    out.puddleLayers[i / 4][static_cast<int>(i % 4)] =
        std::clamp(layer.puddles, 0.0f, 1.0f);
  }
  const bool mapped =
      terrain.puddles && puddles && !puddles->empty() && puddles->texel > 0.0f;
  if (mapped)
    out.puddleMap = glm::vec4(puddles->origin, 1.0f / puddles->extent());
  out.puddleParams = glm::vec4(mapped ? 1.0f : 0.0f, kPuddleDepthRange, 0, 0);
  if (terrain.deformable && deformation && !deformation->empty())
  {
    const float window =
        deformation->texel * static_cast<float>(deformation->size);
    out.deformMap = glm::vec4(patch ? 2.0f : 1.0f,
                              deformation->texel,
                              1.0f / window,
                              std::min(deformation->deepest, 0.0f));
    out.deformWindow = glm::vec4(
        glm::vec2(deformation->from) * deformation->texel, window, window);
    out.deformPatch = glm::vec4(deformation->patchCenter,
                                deformation->patchHalf,
                                std::max(deformation->patchBand, 1e-3f));
    out.deformGround = glm::vec4(deformation->groundOrigin,
                                 std::max(deformation->groundTexel, 1e-3f),
                                 0.0f);
  }
  return out;
}

std::vector<glm::vec4> graphMaterialParameters(const MeshMaterial& material)
{
  const std::size_t count =
      material.shader ? material.shader->parameterCount() : 0;
  std::vector<glm::vec4> parameters(
      std::max<std::size_t>(count, 1), glm::vec4(0.0f));
  std::copy_n(material.shaderParameters.begin(),
              std::min(material.shaderParameters.size(), parameters.size()),
              parameters.begin());
  return parameters;
}

MeshMaterial brokenMaterial()
{
  MeshMaterial broken;
  broken.baseColor = {1.0f, 0.0f, 1.0f, 1.0f};
  return broken;
}

glm::mat4 cubeFaceView(int face, const glm::vec3& light)
{
  // Rows: view x, y and z (looking down -z) from the offset from the light.
  // A face's texel (s, t) looks along its axis with s across it and t down it
  // as the spec's face table has them, so drawing it with this view puts each
  // direction's surface where sampling in that direction reads.
  static const glm::vec3 kRows[6][3] = {
      {{0, 0, -1}, {0, 1, 0}, {-1, 0, 0}}, // +X: s = -z, t = -y
      {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}},   // -X: s = +z, t = -y
      {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}}, // +Y: s = +x, t = +z
      {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -Y: s = +x, t = -z
      {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},  // +Z: s = +x, t = -y
      {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  // -Z: s = -x, t = -y
  };
  glm::mat4 view(1.0f);
  for (int row = 0; row < 3; ++row)
  {
    for (int column = 0; column < 3; ++column)
      view[column][row] = kRows[face][row][column];
    view[3][row] = -glm::dot(kRows[face][row], light);
  }
  return view;
}

void FramePlan::begin(const FrameParams& frame)
{
  m_frame = frame;
  m_pointLights.assign(frame.pointLights.begin(), frame.pointLights.end());
  m_frame.pointLights = m_pointLights;

  m_instances.clear();
  m_drawMaterials.clear();
  m_litDraws.clear();
  m_blendDraws.clear();
  m_blendOrder.clear();
  m_shadowDraws.clear();
  m_unshadowedDraws.clear();
  for (std::vector<Draw>& draws : m_cascadeDraws)
    draws.clear();
  m_casterBatches.clear();
  m_casters.clear();
  m_shadowedLights.clear();
  m_grassDraws.clear();
  m_uiQuads.clear();
  m_open = true;
}

void FramePlan::addMesh(const MeshBatch& batch,
                        uint32_t meshSlot,
                        const PlannedMesh& mesh,
                        bool hasBlend)
{
  if (!m_open)
    return;

  // The batch's materials, shared by its draws.
  const auto firstMaterial = static_cast<uint32_t>(m_drawMaterials.size());
  const std::size_t materialCount =
      std::min(batch.materials.size(), mesh.sections);
  for (std::size_t i = 0; i < materialCount; ++i)
    m_drawMaterials.push_back(batch.materials[i].id);
  const Draw base{meshSlot,
                  0,
                  0,
                  firstMaterial,
                  static_cast<uint32_t>(materialCount),
                  false};

  const auto append = [&](const MeshInstance& instance)
  {
    m_instances.push_back(instance);
    return static_cast<uint32_t>(m_instances.size() - 1);
  };
  const auto seeThrough = [&](uint32_t index, bool allSections)
  {
    Draw draw = base;
    draw.firstInstance = index;
    draw.instanceCount = 1;
    draw.allSections = allSections;
    const glm::vec3 position(m_instances[index].model[3]);
    m_blendDraws.push_back(
        {draw, glm::distance(position, m_frame.cameraPosition)});
  };

  // Drawn together: every instance, back to front, in one see-through draw
  // (its sections all), placed among the scene's see-through things by its
  // middle.
  if (batch.together && !batch.instances.empty())
  {
    std::vector<std::pair<float, const MeshInstance*>> order;
    order.reserve(batch.instances.size());
    float total = 0.0f;
    for (const MeshInstance& instance : batch.instances)
    {
      const float distance =
          glm::distance(glm::vec3(instance.model[3]), m_frame.cameraPosition);
      total += distance;
      order.emplace_back(distance, &instance);
    }
    std::stable_sort(order.begin(),
                     order.end(),
                     [](const auto& a, const auto& b)
                     { return a.first > b.first; });
    Draw together = base;
    together.firstInstance = static_cast<uint32_t>(m_instances.size());
    together.instanceCount = static_cast<uint32_t>(order.size());
    together.allSections = true;
    for (const auto& [distance, instance] : order)
      m_instances.push_back(*instance);
    m_blendDraws.push_back(
        {together, total / static_cast<float>(order.size())});
  }

  // Faded instances (tint alpha below 1) draw see-through, after everything
  // opaque; the rest draw together, but their Blend sections join the
  // see-through ones.
  Draw opaque = base;
  opaque.firstInstance = static_cast<uint32_t>(m_instances.size());
  if (!batch.together)
  {
    std::vector<uint32_t> faded;
    for (const MeshInstance& instance : batch.instances)
      if (instance.tint.a < 1.0f)
        faded.push_back(append(instance));
    for (const MeshInstance& instance : batch.instances)
      if (instance.tint.a >= 1.0f)
      {
        const uint32_t index = append(instance);
        if (opaque.instanceCount == 0)
          opaque.firstInstance = index;
        ++opaque.instanceCount;
        if (hasBlend)
          seeThrough(index, false);
      }
    if (opaque.instanceCount > 0)
      m_litDraws.push_back(opaque);
    if (opaque.instanceCount > 0 && batch.shadowInstances.empty())
      m_unshadowedDraws.push_back(opaque);
    for (const uint32_t index : faded)
      seeThrough(index, true);
  }

  if (!batch.lightShadowInstances.empty())
  {
    m_casterBatches.push_back(
        {base, m_casters.size(), batch.lightShadowInstances.size()});
    m_casters.insert(m_casters.end(),
                     batch.lightShadowInstances.begin(),
                     batch.lightShadowInstances.end());
  }

  if (m_frame.shadows.enabled && !batch.shadowInstances.empty())
  {
    Draw shadow = base;
    shadow.firstInstance = static_cast<uint32_t>(m_instances.size());
    shadow.instanceCount = static_cast<uint32_t>(batch.shadowInstances.size());
    m_shadowDraws.push_back(shadow);
    m_instances.insert(m_instances.end(),
                       batch.shadowInstances.begin(),
                       batch.shadowInstances.end());
  }
}

void FramePlan::addGrass(const GrassBatch& batch)
{
  if (m_open && batch.grass.valid())
    m_grassDraws.push_back(batch);
}

void FramePlan::addUi(const UiDrawList& list)
{
  if (m_open)
    m_uiQuads.insert(m_uiQuads.end(), list.quads.begin(), list.quads.end());
}

void FramePlan::finish(
    glm::ivec2 viewport,
    uint32_t shadowMapResolution,
    uint32_t pointShadowResolution,
    const std::function<const PlannedMesh*(uint32_t)>& meshes)
{
  m_open = false;

  // Farthest first, so nearer see-through things blend over farther ones.
  std::stable_sort(m_blendDraws.begin(),
                   m_blendDraws.end(),
                   [](const BlendDraw& a, const BlendDraw& b)
                   { return a.distance > b.distance; });
  m_blendOrder.clear();
  for (const BlendDraw& blend : m_blendDraws)
    m_blendOrder.push_back(blend.draw);

  // The lights casting shadows, up to the budget, each with the casters
  // within its reach (a caster's bounding sphere, placed, touching the
  // light's), copied into the frame's instances.
  const std::size_t count = std::min(m_pointLights.size(), kMaxPointLights);
  const std::size_t budget = std::min<std::size_t>(
      m_frame.shadows.pointLights, kMaxShadowedPointLights);
  // (A lightning strike's first: it's what the scene's lit by while it
  // flashes.)
  const int strikeShadow = m_frame.lightning.shadowLight;
  const bool strikeShadowed =
      strikeShadow >= 0 && static_cast<std::size_t>(strikeShadow) < count &&
      budget > 0 &&
      m_pointLights[static_cast<std::size_t>(strikeShadow)].castsShadow;
  if (strikeShadowed)
    m_shadowedLights.push_back({static_cast<std::size_t>(strikeShadow), {}});
  for (std::size_t i = 0; i < count && m_shadowedLights.size() < budget; ++i)
    if (m_pointLights[i].castsShadow &&
        (!strikeShadowed || i != static_cast<std::size_t>(strikeShadow)))
      m_shadowedLights.push_back({i, {}});
  for (ShadowedLight& shadowed : m_shadowedLights)
  {
    const PointLight& light = m_pointLights[shadowed.light];
    for (const CasterBatch& batch : m_casterBatches)
    {
      const PlannedMesh* mesh = meshes(batch.base.meshSlot);
      if (!mesh)
        continue;
      Draw draw = batch.base;
      draw.firstInstance = static_cast<uint32_t>(m_instances.size());
      for (std::size_t k = 0; k < batch.count; ++k)
      {
        const MeshInstance& instance = m_casters[batch.first + k];
        const glm::vec3 center(instance.model *
                               glm::vec4(mesh->boundsCenter, 1.0f));
        const float scale =
            std::max({glm::length(glm::vec3(instance.model[0])),
                      glm::length(glm::vec3(instance.model[1])),
                      glm::length(glm::vec3(instance.model[2]))});
        if (glm::distance(center, light.position) >
            light.radius + mesh->boundsRadius * scale)
          continue;
        m_instances.push_back(instance);
        ++draw.instanceCount;
      }
      if (draw.instanceCount > 0)
        shadowed.draws.push_back(draw);
    }
  }

  // Each sun shadow cascade's casters: those whose bounding sphere, placed,
  // reaches into its box (or beyond it toward the sun, within its depth).
  const SunCascades& cascades = m_frame.cascades;
  if (m_frame.shadows.enabled && cascades.count > 1)
    for (uint32_t c = 0; c < cascades.count && c < kMaxShadowCascades; ++c)
    {
      const SunCascade& cascade = cascades.cascades[c];
      const float toClip = 1.0f / std::max(cascade.radius, 1e-4f);
      const float toDepth = 1.0f / std::max(cascade.depthRange, 1e-4f);
      for (const Draw& shadow : m_shadowDraws)
      {
        const PlannedMesh* mesh = meshes(shadow.meshSlot);
        if (!mesh)
          continue;
        Draw draw = shadow;
        draw.firstInstance = static_cast<uint32_t>(m_instances.size());
        draw.instanceCount = 0;
        for (uint32_t k = 0; k < shadow.instanceCount; ++k)
        {
          const MeshInstance instance = m_instances[shadow.firstInstance + k];
          const glm::vec4 center = cascade.viewProjection * instance.model *
                                   glm::vec4(mesh->boundsCenter, 1.0f);
          const float scale =
              std::max({glm::length(glm::vec3(instance.model[0])),
                        glm::length(glm::vec3(instance.model[1])),
                        glm::length(glm::vec3(instance.model[2]))});
          const float radius = mesh->boundsRadius * scale;
          const float across = 1.0f + radius * toClip;
          const float deep = radius * toDepth;
          if (std::abs(center.x) > across || std::abs(center.y) > across ||
              center.z < -deep || center.z > 1.0f + deep)
            continue;
          m_instances.push_back(instance);
          ++draw.instanceCount;
        }
        if (draw.instanceCount > 0)
          m_cascadeDraws[c].push_back(draw);
      }
    }

  // Each shadowed light's cube faces.
  m_faces.clear();
  for (const ShadowedLight& shadowed : m_shadowedLights)
  {
    const PointLight& light = m_pointLights[shadowed.light];
    const float reach = std::max(light.radius, 1e-3f);
    const float near = std::clamp(light.shadowNear, 0.01f, reach * 0.5f);
    const glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::half_pi<float>(), 1.0f, near, reach);
    for (int face = 0; face < 6; ++face)
      m_faces.push_back({projection * cubeFaceView(face, light.position),
                         glm::vec4(light.position, 1.0f / reach)});
  }

  // The frame's uniforms.
  const SceneLighting& lighting = m_frame.lighting;
  const ShadowSettings& shadows = m_frame.shadows;
  const float resolution =
      static_cast<float>(std::max(shadowMapResolution, 1u));
  const float sunLength = glm::length(lighting.sun.direction);
  const glm::vec3 toSun = sunLength > 1.0e-4f
                              ? lighting.sun.direction / sunLength
                              : glm::vec3(0.0f, 0.0f, 1.0f);
  m_uniforms = {};
  m_uniforms.viewProjection = m_frame.viewProjection;
  m_uniforms.lightViewProjection = m_frame.lightViewProjection;
  m_uniforms.cameraPosition = glm::vec4(m_frame.cameraPosition, 1.0f);
  m_uniforms.sunDirection = glm::vec4(toSun, lighting.sun.diffuseStrength);
  m_uniforms.sunColor = glm::vec4(lighting.sun.color, lighting.ambient);
  // Under cloud the sun's light comes from more of the sky around it: its
  // shadows soften (up to x3 overcast).
  const float softness =
      shadows.softness *
      (1.0f + 2.0f * std::clamp(lighting.cloudCover, 0.0f, 1.0f));
  m_uniforms.shadow = glm::vec4(shadows.enabled ? 1.0f : 0.0f,
                                shadows.depthBias,
                                softness / resolution,
                                2.0f * shadows.distance / resolution);
  m_uniforms.sky = glm::vec4(lighting.skyColor, 1.0f);
  m_uniforms.time = glm::vec4(m_frame.time, 0.0f, 0.0f, 0.0f);
  m_uniforms.cameraForward = glm::vec4(m_frame.cameraForward, 0.0f);
  const float windAngle = glm::radians(m_frame.wind.direction);
  m_uniforms.wind = glm::vec4(std::cos(windAngle),
                              std::sin(windAngle),
                              std::max(m_frame.wind.strength, 0.0f),
                              std::max(m_frame.wind.gustiness, 0.0f));
  m_uniforms.haze = glm::vec4(std::clamp(m_frame.haze.strength, 0.0f, 1.0f),
                              1.0f / std::max(m_frame.haze.distance, 1e-3f),
                              0.0f,
                              0.0f);
  // Height fog, which (on) takes haze's place.
  const FogSettings& fog = m_frame.fog;
  if (fog.enabled)
  {
    m_uniforms.haze.x = 0.0f;
    m_uniforms.fog = glm::vec4(std::max(fog.density, 0.0f),
                               std::max(fog.heightFalloff, 0.0f),
                               fog.baseHeight,
                               std::max(fog.startDistance, 0.0f));
    m_uniforms.fogLight = glm::vec4(std::clamp(fog.maxOpacity, 0.0f, 1.0f),
                                    std::max(fog.sunGlow, 0.0f),
                                    1.0f,
                                    m_frame.farPlane);
  }
  // Ambient occlusion and contact shadows: their map counts where it's
  // within a twentieth of a pixel's depth (else, e.g. grass blades, which it
  // doesn't hold, go without).
  if (screenOcclusionOn())
    m_uniforms.ambientOcclusion =
        glm::vec4(1.0f, 0.05f, contactShadowsOn() ? 1.0f : 0.0f, 0.0f);
  // A visual style's balance of sun and ambient light; and, tone mapped,
  // light may go past white (the curve rolls it off), else it stops there.
  const LightBalance& balance = m_frame.lightBalance;
  m_uniforms.sunBalance = glm::vec4(
      glm::max(balance.sun, glm::vec3(0.0f)), balance.neutral() ? 0.0f : 1.0f);
  m_uniforms.ambientBalance =
      glm::vec4(glm::max(balance.ambient, glm::vec3(0.0f)),
                m_frame.grading.toneMapper == ToneMapper::None ? 1.0f : 64.0f);
  m_uniforms.inverseViewProjection = glm::inverse(m_frame.viewProjection);

  // The cascades: each a tile of the shadow atlas (2 side by side, 3 or 4 in
  // a square), its texel's world size, its depth bias (the scene's, kept the
  // same in world units relative to its size as the one map's), and its
  // filter's spread across the atlas.
  m_uniforms.cascadeInfo = glm::vec4(1.0f, 0.0f, 1.0f, 0.0f);
  if (cascades.count > 1)
  {
    const uint32_t count = std::min(cascades.count, kMaxShadowCascades);
    const glm::vec2 atlas(shadowAtlasSize(count, shadowMapResolution));
    const glm::vec2 tile = glm::vec2(resolution) / atlas;
    for (uint32_t c = 0; c < count; ++c)
    {
      const SunCascade& cascade = cascades.cascades[c];
      m_uniforms.cascades[c] = cascade.viewProjection;
      m_uniforms.cascadeRects[c] = glm::vec4(
          glm::vec2(static_cast<float>(c % 2), static_cast<float>(c / 2)) *
              tile,
          tile);
      const float texel = 2.0f * cascade.radius / resolution;
      const float bias = shadows.depthBias * 4.0f * cascade.radius /
                         std::max(cascade.depthRange, 1e-4f);
      m_uniforms.cascadeParams[c] =
          glm::vec4(texel, bias, glm::vec2(softness) / atlas);
    }
    // Blend over the outer tenth of each; the last fades out over the final
    // fifth of the distance it reaches.
    const SunCascade& last = cascades.cascades[count - 1];
    m_uniforms.cascadeInfo =
        glm::vec4(static_cast<float>(count),
                  0.1f,
                  last.splitNear + (last.splitFar - last.splitNear) * 0.8f,
                  last.splitFar);
  }

  // A physical sky: its sun, its diffuse light, its reflections (once the
  // backend has rendered them: skyMode.y), the disk.
  const FrameSky& sky = m_frame.sky;
  if (sky.physical)
  {
    const SkySettings& settings = sky.physical->settings();
    // Eyes adapted to a dim sky see its light brighter: the sun's, its
    // diffuse light and the background here, its reflections in the shader.
    const float adapted = std::max(sky.adaptation, 0.0f);
    // (Clouds hide the stars.)
    m_uniforms.skyMode = glm::vec4(
        1.0f, 0.0f, 0.0f, settings.nightBrightness * (1.0f - sky.cloudCover));
    // (The sun's light is faded as it rises clear of the horizon; its disk
    // isn't.)
    m_uniforms.skyLight =
        glm::vec4(adapted,
                  sky.moonLit ? 1.0f : PhysicalSky::clearOfHorizon(sky.toSun),
                  std::clamp(sky.lampScale, 0.0f, 1.0f),
                  0.0f);
    m_uniforms.sunLight =
        glm::vec4(sky.sunLight * adapted,
                  PhysicalSky::kSunRadius * std::max(settings.sunSize, 0.0f));
    m_uniforms.skyScale =
        glm::vec4(glm::max(sky.scale, glm::vec3(0.0f)) * adapted,
                  settings.sunSize > 0.0f ? 40.0f : 0.0f);
    for (std::size_t i = 0; i < 9; ++i)
      m_uniforms.skyIrradiance[i] =
          glm::vec4(sky.irradiance.coefficients[i] * adapted, 0.0f);
    // The moon's disk, bigger than the sun's (as it looks), its light the
    // sky's own through the air, behind whatever clouds.
    const float moonRadius =
        2.5f * PhysicalSky::kSunRadius * std::max(settings.sunSize, 0.0f);
    m_uniforms.moon =
        glm::vec4(sky.toMoon, sky.toMoon.z > -0.02f ? moonRadius : 0.0f);
    m_uniforms.moonSun = glm::vec4(sky.toSun, sky.moonPhase);
    m_uniforms.moonLight =
        glm::vec4(sky.physical->moonLight(sky.toMoon, 1.0f) *
                      (kMoonDiskGlow * (1.0f - sky.cloudCover)),
                  sky.moonLit ? 1.0f : 0.0f);
  }
  else
    m_uniforms.skyScale = glm::vec4(1.0f);

  // The weather; and a physical sky's clouds, where there are any.
  const SceneWeather& weather = m_frame.weather;
  m_uniforms.weather = glm::vec4(weather.rain,
                                 weather.wetness,
                                 weather.puddles,
                                 std::clamp(lighting.cloudCover, 0.0f, 1.0f));
  m_uniforms.weatherWind = glm::vec4(weather.wind, m_frame.cloudDrift);
  if (sky.physical && sky.cloudCover > 0.0f)
  {
    m_uniforms.cloudLight = glm::vec4(sky.cloudLight, sky.cloudDarkness);
    m_uniforms.cloudBase =
        glm::vec4(sky.cloudBase, 1.0f / clearSky(sky.cloudCover));
  }

  // Rain: its overhead map (where the sky is open), the streaks and splashes
  // falling, how they look, and its veil.
  const RainSettings& rain = m_frame.rain;
  m_rainMap = rainMapFor(m_frame.cameraPosition, m_frame.cameraForward);
  m_uniforms.rainView = m_rainMap.viewProjection;
  m_uniforms.rainMap = glm::vec4(
      m_rainMap.center, m_rainMap.halfSize, rainMapOn() ? 1.0f : 0.0f);
  m_uniforms.rainDepth =
      glm::vec4(m_rainMap.top,
                m_rainMap.depthRange,
                static_cast<float>(rainStreaks(rain, weather.rain)),
                static_cast<float>(rainSplashes(rain, weather.rain)));
  m_uniforms.rainStreaks =
      glm::vec4(std::max(rain.length, 0.0f),
                std::max(rain.width, 0.0f) * 0.001f,
                std::max(rain.brightness, 0.0f),
                rain.enabled ? std::max(rain.ripples, 0.0f) : 0.0f);
  m_uniforms.rainVeil = glm::vec4(rainVeilDensity(rain, weather.rain),
                                  static_cast<float>(viewport.x),
                                  static_cast<float>(viewport.y),
                                  0.0f);
  m_uniforms.rainDrift = glm::vec4(m_frame.rainDrift, m_frame.rainWind);
  // Drips off edges: they're found on the overhead map, so none without it.
  if (rainMapOn())
  {
    const RainDrips drips = rainDrips(rain, weather.rain, weather.wetness);
    m_uniforms.rainDrips = glm::vec4(
        static_cast<float>(drips.count), drips.rate, drips.trickle, drips.line);
  }

  // Lightning, under a physical sky, adapted as the sky's light is.
  const FrameLightning& lightning = m_frame.lightning;
  if (sky.physical && lightning.active)
  {
    const float adapted = std::max(sky.adaptation, 0.0f);
    m_uniforms.lightningLow =
        glm::vec4(lightning.low,
                  strikeShadowed ? static_cast<float>(strikeShadow) : -1.0f);
    m_uniforms.lightningHigh =
        glm::vec4(lightning.high, lightning.channel ? 1.0f : 0.0f);
    m_uniforms.lightningLight = glm::vec4(
        lightning.channelLight * adapted, std::max(lightning.glowRadius, 1.0f));
    m_uniforms.lightningGlow = glm::vec4(lightning.cloudGlow * adapted, 1.0f);
  }
  else
    m_uniforms.lightningLow.w = -1.0f;

  // The lights (nearest the camera's target first, as the render system
  // keeps them), and the clusters each reaches.
  m_grid = ClusterGrid::forViewport(
      glm::vec2(viewport), m_frame.nearPlane, m_frame.farPlane);
  m_uniforms.clusterSize =
      glm::uvec4(m_grid.size, static_cast<uint32_t>(count));
  m_uniforms.clusterDepth =
      glm::vec4(m_grid.nearPlane,
                m_grid.farPlane,
                static_cast<float>(m_grid.size.z) /
                    std::log(m_grid.farPlane / m_grid.nearPlane),
                m_grid.tilePixels);

  const std::span<const PointLight> lights(m_pointLights.data(), count);
  // (A lightning strike's shadow light lights nothing itself: it's kept out
  // of the clusters. It's always the last.)
  const std::size_t clustered =
      strikeShadow >= 0 && static_cast<std::size_t>(strikeShadow) + 1 == count
          ? count - 1
          : count;
  ClusterView view;
  view.viewProjection = m_frame.viewProjection;
  view.viewport = glm::vec2(viewport);
  view.cameraPosition = m_frame.cameraPosition;
  view.forward = m_frame.cameraForward;
  assignLights(m_grid, view, lights.first(clustered), m_clusters);

  const float texel =
      1.0f / static_cast<float>(std::max(pointShadowResolution, 1u));
  m_gpuLights.resize(count);
  for (std::size_t i = 0; i < count; ++i)
    m_gpuLights[i] = {glm::vec4(lights[i].position, lights[i].radius),
                      glm::vec4(lights[i].color, lights[i].intensity),
                      glm::vec4(-1.0f, texel, lights[i].sourceRadius, 0.0f)};
  for (std::size_t i = 0; i < m_shadowedLights.size(); ++i)
    m_gpuLights[m_shadowedLights[i].light].shadow.x = static_cast<float>(i);

  // The UI quads as instances, in batches sharing a texture. Boxes don't read
  // their texture, so they needn't break a run of glyphs.
  m_uiInstances.clear();
  m_uiBatches.clear();
  for (const UiQuad& quad : m_uiQuads)
  {
    const UiTexture* texture =
        quad.kind == UiQuad::Box ? nullptr : quad.texture;
    if (m_uiBatches.empty() ||
        (m_uiBatches.back().texture != texture && quad.kind != UiQuad::Box))
      m_uiBatches.push_back(
          {texture, static_cast<uint32_t>(m_uiInstances.size()), 0});
    ++m_uiBatches.back().count;
    m_uiInstances.push_back({quad.rect,
                             quad.uv,
                             quad.color,
                             quad.borderColor,
                             glm::vec4(quad.radius,
                                       quad.softness,
                                       quad.border,
                                       static_cast<float>(quad.kind))});
  }
  m_uiQuads.clear();
}

PostUniforms FramePlan::bloomUniforms(glm::ivec2 sourceSize) const
{
  const BloomSettings& bloom = m_frame.bloom;
  return {glm::vec4(1.0f / glm::vec2(sourceSize), 0.0f, 0.0f),
          glm::vec4(std::max(bloom.threshold, 0.0f),
                    std::max(bloom.softness, 0.0f),
                    std::max(bloom.radius, 0.0f),
                    std::max(bloom.intensity, 0.0f))};
}

PostUniforms FramePlan::compositeUniforms(glm::ivec2 viewport) const
{
  const BloomSettings& bloom = m_frame.bloom;
  const ColorGrading& grading = m_frame.grading;
  PostUniforms out{};
  out.texel = glm::vec4(
      1.0f / glm::vec2(glm::max(viewport, glm::ivec2(1))), 0.0f, 0.0f);
  out.bloom = glm::vec4(
      0.0f, 0.0f, 0.0f, bloom.enabled ? std::max(bloom.intensity, 0.0f) : 0.0f);
  out.tone = glm::vec4(static_cast<float>(grading.toneMapper),
                       std::exp2(grading.exposure),
                       std::max(grading.contrast, 0.0f),
                       std::max(grading.saturation, 0.0f));
  out.grade = glm::vec4(grading.neutral() ? 0.0f : 1.0f,
                        std::clamp(grading.vignette, 0.0f, 1.0f),
                        std::clamp(grading.vignetteSoftness, 0.0f, 1.0f),
                        static_cast<float>(std::max(viewport.x, 1)) /
                            static_cast<float>(std::max(viewport.y, 1)));
  const glm::mat3 balance =
      whiteBalanceMatrix(grading.temperature, grading.tint);
  for (int column = 0; column < 3; ++column)
    out.balance[column] = glm::vec4(balance[column], 0.0f);
  out.lift = glm::vec4(grading.lift, 0.0f);
  out.gamma = glm::vec4(1.0f / glm::max(grading.gamma, glm::vec3(0.01f)), 0.0f);
  out.gain = glm::vec4(grading.gain, 0.0f);
  return out;
}

ExposureUniforms FramePlan::exposureUniforms(glm::ivec2 viewport,
                                             float seconds) const
{
  const ColorGrading& grading = m_frame.grading;
  ExposureUniforms out{};
  out.params =
      glm::vec4(std::log2(0.18f) + grading.exposureTarget,
                std::max(grading.exposureDarken, 0.0f),
                std::max(grading.exposureBrighten, 0.0f),
                1.0f - std::exp(-std::max(seconds, 0.0f) *
                                std::max(grading.exposureSpeed, 0.0f)));
  // While lightning flashes it holds: the flash brightens the scene, and the
  // dark between strikes stays as dark as it was.
  if (m_frame.lightning.active)
    out.params.w = 0.0f;
  out.source = glm::vec4(glm::vec2(glm::max(viewport, glm::ivec2(1))),
                         grading.autoExposure ? 1.0f : 0.0f,
                         0.0f);
  return out;
}

SkyUniforms FramePlan::skyUniforms() const
{
  SkyUniforms out{};
  const FrameSky& sky = m_frame.sky;
  if (!sky.physical)
    return out;
  const PhysicalSky& model = *sky.physical;
  out.toSun = glm::vec4(sky.toSun, model.solarIrradiance().x);
  out.rayleigh =
      glm::vec4(model.rayleighScattering(), PhysicalSky::kRayleighHeight);
  out.mie = glm::vec4(model.mieScattering(),
                      model.mieExtinction(),
                      PhysicalSky::kMieHeight,
                      PhysicalSky::kMieG);
  out.ozone = glm::vec4(model.ozoneAbsorption(), 0.0f);
  out.ground = glm::vec4(sky.groundLight, 0.0f);
  out.night =
      glm::vec4(model.nightGlow(glm::vec3(0.0f, 0.0f, 1.0f), sky.night), 0.0f);
  out.planet = glm::vec4(PhysicalSky::kGroundRadius,
                         PhysicalSky::kTopRadius,
                         PhysicalSky::kViewerAltitude,
                         PhysicalSky::kMultipleScattering);
  // The clouds as they are where the camera is (its place in their pattern).
  const glm::vec2 origin =
      glm::vec2(m_frame.cameraPosition) * 0.001f + m_frame.cloudDrift;
  out.clouds = glm::vec4(origin, sky.cloudCover, sky.cloudDarkness);
  out.cloudLight = glm::vec4(sky.cloudLight, PhysicalSky::kCloudAltitude);
  out.cloudBase = glm::vec4(sky.cloudBase, 0.0f);
  const float lightLength = glm::length(m_frame.lighting.sun.direction);
  out.cloudToLight = glm::vec4(
      lightLength > 1e-6f ? m_frame.lighting.sun.direction / lightLength
                          : glm::vec3(0.0f, 0.0f, 1.0f),
      0.0f);
  return out;
}

OcclusionUniforms FramePlan::occlusionUniforms(glm::ivec2 size,
                                               glm::ivec2 blurStep) const
{
  const AmbientOcclusionSettings& settings = m_frame.ambientOcclusion;
  const glm::vec2 texels(std::max(size.x, 1), std::max(size.y, 1));
  // A perspective projection's w grows with depth; an orthographic one's
  // doesn't.
  const bool perspective = m_frame.projection[2][3] != 0.0f;
  OcclusionUniforms out{};
  out.inverseViewProjection = glm::inverse(m_frame.viewProjection);
  out.camera = glm::vec4(m_frame.cameraPosition, perspective ? 1.0f : 0.0f);
  out.forward = glm::vec4(m_frame.cameraForward, 0.0f);
  // Half the pass's height spans 1 / projection[1][1] world units at depth 1
  // (orthographic: at any depth).
  out.params = glm::vec4(
      std::max(settings.radius, 0.01f),
      std::max(settings.intensity, 0.0f),
      static_cast<float>(std::clamp(
          settings.samples, kMinOcclusionSamples, kMaxOcclusionSamples)),
      0.5f * texels.y * std::abs(m_frame.projection[1][1]));
  out.size = glm::vec4(texels, 1.0f / texels);
  // Fading out from 60% of the far plane (or 120 m) to 90% of it (180 m), so
  // far-off detail (which depth holds less finely) doesn't speckle.
  const float reach = std::min(m_frame.farPlane, 200.0f);
  out.blur = glm::vec4(glm::vec2(blurStep), reach * 0.6f, reach * 0.9f);

  // Contact shadows toward the frame's light (the moon's, when it's in the
  // sun's place), gone by half that reach (100 m at most): past it a texel's
  // too coarse for them, and the shadow maps hold what they would.
  out.viewProjection = m_frame.viewProjection;
  out.light = glm::vec4(
      glm::vec3(m_uniforms.sunDirection), settings.enabled ? 1.0f : 0.0f);
  if (contactShadowsOn())
  {
    const ContactShadowSettings& contact = m_frame.contactShadows;
    out.contact = glm::vec4(std::max(contact.length, 0.01f),
                            std::max(contact.thickness, 0.01f),
                            std::clamp(contact.strength, 0.0f, 1.0f),
                            reach * 0.5f);
  }
  return out;
}

bool FramePlan::contactShadowsOn() const
{
  const ContactShadowSettings& settings = m_frame.contactShadows;
  return settings.enabled && settings.strength > 0.0f &&
         settings.length > 0.0f && m_frame.sky.physical != nullptr;
}

bool FramePlan::rainMapOn() const
{
  const SceneWeather& weather = m_frame.weather;
  return m_frame.shadows.enabled &&
         (weather.rain > 0.0f || weather.wetness > 0.0f ||
          weather.puddles > 0.0f);
}

std::optional<FocusUniforms> FramePlan::focusUniforms(glm::ivec2 viewport) const
{
  const DepthOfFieldSettings& settings = m_frame.depthOfField;
  if (!settings.enabled || !(settings.blur > 0.0f))
    return std::nullopt;
  // Depth back to distance, by the projection (perspective or not); the blur
  // as wide on any screen, about 16 pixels a side at 1080p.
  const glm::mat4& projection = m_frame.projection;
  const glm::ivec2 half = glm::max(viewport / 2, glm::ivec2(1));
  return FocusUniforms{
      glm::vec4(projection[2][2],
                projection[3][2],
                projection[2][3] != 0.0f ? 1.0f : 0.0f,
                0.0f),
      glm::vec4(std::max(m_frame.focusDistance, 0.01f),
                std::max(settings.range, 0.0f) * 0.5f,
                settings.blur * 8.0f * static_cast<float>(viewport.y) / 1080.0f,
                0.0f),
      glm::vec4(1.0f / glm::vec2(half), 0.0f, 0.0f)};
}

float grassKeptShare(const GrassGrowUniforms& uniforms, float distance)
{
  const glm::vec4& thinning = uniforms.thinning;
  const float full = std::min(
      std::pow(thinning.x / std::max(distance, 1e-3f), thinning.y), 1.0f);
  const float far = uniforms.camera.w;
  const float t = std::clamp(
      (distance - thinning.z) / std::max(far - thinning.z, 1e-3f), 0.0f, 1.0f);
  return full * (1.0f - t * t * (3.0f - 2.0f * t));
}

bool grassLegacyThinning()
{
  static const bool legacy = std::getenv("SFS_GRASS_LEGACY_THINNING");
  return legacy;
}

float grassShadowReach(const GrassBatch& batch, const FrameParams& frame)
{
  if (!batch.castsShadows || !frame.shadows.enabled)
    return 0.0f;
  const float reach =
      frame.cascades.count > 1
          ? std::min(frame.cascades
                         .cascades[std::min(frame.cascades.count,
                                            kGrassShadowCascades) -
                                   1]
                         .splitFar,
                     kGrassShadowMostReach)
          : kGrassShadowOneMapReach;
  return std::clamp(reach, 0.0f, std::max(batch.drawDistance, 0.0f));
}

void grassFarGround(GrassUniforms& uniforms, const TerrainMaterial* terrain)
{
  uniforms.far.x = uniforms.far.y = 0.0f;
  uniforms.farMeadow = glm::vec4(0.0f);
  if (!terrain || terrain->layers.empty())
    return;
  const auto meadow = [&](std::size_t layer)
  {
    // (A channel with no layer of its own: the first's, as the terrain
    // draws it.)
    return std::clamp(
        terrain->layers[layer < terrain->layers.size() ? layer : 0].meadow,
        0.0f,
        1.0f);
  };
  uniforms.far.x = meadow(0);
  uniforms.far.y = 1.0f;
  for (int channel = 0; channel < 4; ++channel)
    uniforms.farMeadow[channel] = meadow(std::size_t(channel) + 1);
}

std::optional<GrassPlan> planGrass(const GrassBatch& batch,
                                   const GrassFieldState& field,
                                   const FrameParams& frame)
{
  const GrassShape& shape = batch.shape;
  if (!(shape.density > 0.0f) || !(shape.height > 0.0f))
    return std::nullopt;

  const glm::mat4& model = batch.model;
  const glm::vec3 acrossX(model[0]), acrossY(model[1]);
  // World area per square unit of its own XY, and the least a unit across it
  // is in the world.
  const float area = std::max(glm::length(glm::cross(acrossX, acrossY)), 1e-8f);
  const float least =
      std::max(std::min(glm::length(acrossX), glm::length(acrossY)), 1e-6f);
  const float density = std::min(shape.density, 20000.0f);
  const float cell = 1.0f / std::sqrt(density * area);
  const float distance = std::max(batch.drawDistance, 1.0f);
  const glm::mat4 normalMatrix(glm::transpose(glm::inverse(glm::mat3(model))));
  const glm::vec4 maps =
      field.mapsGrid.empty()
          ? glm::vec4(0.0f)
          : glm::vec4(field.mapsGrid.min,
                      1.0f / (field.mapsGrid.max - field.mapsGrid.min));

  const float shadowReach = grassShadowReach(batch, frame);

  GrassPlan plan;
  // How it's drawn (its ground, even with no blades in reach), and its
  // blades' shadows.
  plan.draw = {field.trampleBounds,
               glm::vec4(std::clamp(batch.stiffness, 0.0f, 1.0f),
                         distance,
                         kGrassGroundTint,
                         shape.height),
               maps,
               glm::vec4(std::clamp(batch.selfShadow, 0.0f, 1.0f),
                         shadowReach,
                         kGrassShadowWidth,
                         0.0f),
               model,
               normalMatrix,
               glm::inverse(model),
               glm::vec4(std::max({kGrassVertexSunFrom,
                                   distance * kGrassFineDetail,
                                   shadowReach}),
                         kGrassVertexSunBand,
                         0.0f,
                         0.0f)};
  plan.draw.far.z = kGrassFadeFrom * distance;

  GrassGrowUniforms& uniforms = plan.grow;
  uniforms.model = model;
  uniforms.normalMatrix = normalMatrix;
  uniforms.planes = Frustum::fromViewProjection(frame.viewProjection).planes();
  const glm::vec3 grassCamera = frame.cameraPosition + frame.grassCameraOffset;
  uniforms.camera = glm::vec4(grassCamera, distance);
  uniforms.viewProjection = frame.viewProjection;
  uniforms.eye = glm::vec4(frame.cameraPosition, 1.0f);
  // How far a blade's shadow falls from it, for its height (none: the sun
  // so low they'd fall too far to tell, see meshGrassGrow.wgsl's
  // hiddenTile()).
  const float sunLength = glm::length(frame.lighting.sun.direction);
  const float sunUp =
      sunLength > 1e-4f ? frame.lighting.sun.direction.z / sunLength : 0.0f;
  uniforms.forward =
      glm::vec4(frame.cameraForward,
                sunUp > kGrassLowestCulledSun
                    ? std::sqrt(std::max(1.0f - sunUp * sunUp, 0.0f)) / sunUp
                    : -1.0f);
  const GrassGrid& ground = field.groundGrid;
  uniforms.ground = glm::vec4(ground.min, ground.texelSize());
  uniforms.groundSize = glm::ivec4(ground.size, 0, 0);
  uniforms.maps = maps;
  uniforms.shape =
      glm::vec4(shape.height, shape.heightVariation, shape.width, cell);
  uniforms.detail = glm::vec4(distance * kGrassFineDetail,
                              distance * kGrassMidDetail,
                              shadowReach,
                              0.0f);
  const bool legacy = grassLegacyThinning();
  uniforms.thinning = glm::vec4(
      std::min(kGrassFullCells / std::sqrt(density), kGrassMostFull * distance),
      kGrassThinning,
      kGrassFadeFrom * distance,
      legacy ? 0.0f : 1.0f);

  // Each lattice's cells within its reach of the camera, on the ground (from
  // its own origin: see meshGrassGrow.wgsl).
  // Only those in the view's bearings (seen from above), when it says.
  const glm::mat4 toOwn = glm::inverse(model);
  const glm::vec2 camera(toOwn * glm::vec4(grassCamera, 1.0f));
  const glm::vec2 eye(toOwn * glm::vec4(frame.cameraPosition, 1.0f));
  const std::optional<glm::vec2> bearings = viewBearings(frame, toOwn);
  glm::ivec2 most(0);
  for (int k = 0; k < 4; ++k)
  {
    // As far as any of its blades are kept: a lattice's blades rank below
    // a quarter of the next finer's (see meshGrassGrow.wgsl), and the
    // coarsest's all the way down.
    const float lowest = std::pow(0.25f, static_cast<float>(k + 1));
    const float reach =
        legacy  ? distance * kGrassReach[k]
        : k < 3 ? std::min(uniforms.thinning.x *
                               std::pow((1.0f + kGrassGrowIn) / lowest,
                                        1.0f / kGrassThinning),
                           distance)
                : distance;
    uniforms.reach[k] = reach;
    const float size = cell * static_cast<float>(1 << k);
    const float own = reach / least;
    glm::vec2 low = camera - own, high = camera + own;
    if (bearings)
    {
      // The box round the sector from the eye, out to the reach (from the
      // camera grass grows from: the eye's, but when checking), with room
      // for a cell and a blade's lean.
      const float out = own + glm::length(camera - eye);
      glm::vec2 near = eye, far = eye;
      const auto take = [&](float bearing)
      {
        const glm::vec2 at =
            eye + out * glm::vec2(std::cos(bearing), std::sin(bearing));
        near = glm::min(near, at);
        far = glm::max(far, at);
      };
      take(bearings->x);
      take(bearings->y);
      for (int quarter = -4; quarter <= 4; ++quarter)
      {
        const float bearing = quarter * glm::half_pi<float>();
        if (bearing > bearings->x && bearing < bearings->y)
          take(bearing);
      }
      const float room = size * 2.0f + 1.5f / least;
      low = glm::max(low, near - room);
      high = glm::min(high, far + room);
    }
    low = glm::max(low, ground.min);
    high = glm::min(high, ground.max);
    glm::ivec2 first(glm::floor(low / size));
    glm::ivec2 count =
        glm::max(glm::ivec2(glm::ceil(high / size)) - first, glm::ivec2(0));
    // Too many: those nearest the camera.
    for (int axis = 0; axis < 2; ++axis)
      if (count[axis] > kGrassMostCells)
      {
        first[axis] = static_cast<int>(std::floor(camera[axis] / size)) -
                      kGrassMostCells / 2;
        count[axis] = kGrassMostCells;
      }
    uniforms.lattices[k] = glm::ivec4(first, count);
    most = glm::max(most, count);
  }
  plan.capacity = field.capacity;
  plan.shadowCapacity = field.shadowCapacity;
  if (most.x <= 0 || most.y <= 0)
    return plan;

  // Room for each list: about how many blades grow within its distances (all
  // round), but no more than the whole field has.
  const auto within = [&](float radius)
  {
    double count = 0.0;
    if (legacy)
      for (int k = 0; k < 4; ++k)
      {
        const double reach = std::min(radius, uniforms.reach[k]);
        count += density * (k < 3 ? 0.75 : 1.0) / double(1 << (2 * k)) *
                 glm::pi<double>() * reach * reach;
      }
    else
    {
      // Those kept in each ring (with those growing in).
      constexpr int kRings = 128;
      const double step = std::min(radius, distance) / kRings;
      for (int ring = 0; ring < kRings; ++ring)
      {
        const double middle = (ring + 0.5) * step;
        const double kept =
            std::min(grassKeptShare(uniforms, static_cast<float>(middle)) *
                         (1.0 + kGrassGrowIn),
                     1.0);
        count += density * kept * 2.0 * glm::pi<double>() * middle * step;
      }
    }
    return count;
  };
  const double whole = double(field.groundArea) * area * density;
  const std::array<double, 4> bands = {0.0,
                                       within(uniforms.detail.x),
                                       within(uniforms.detail.y),
                                       within(distance)};
  std::array<double, 3> needed{};
  double total = 0.0;
  for (int list = 0; list < 3; ++list)
  {
    needed[list] =
        std::min(bands[list + 1] - bands[list], whole) * 1.15 + 256.0;
    total += needed[list];
  }
  const double fit = std::min(1.0, kGrassMostBlades / total);
  bool grow = false;
  for (int list = 0; list < 3; ++list)
  {
    needed[list] *= fit;
    grow = grow || needed[list] > field.capacity[list];
  }
  // The shadow casters: those within its reach, whichever list they're in.
  const double casting =
      shadowReach > 0.0f
          ? std::min(within(shadowReach), whole) * fit * 1.15 + 256.0
          : 0.0;
  if (grow)
    // Room to spare, so it isn't made again as the camera moves a little; a
    // multiple of 8 blades a list, for the lists' bindings' offsets.
    for (int list = 0; list < 3; ++list)
    {
      const double room =
          std::max(needed[list] * 1.25, double(field.capacity[list]));
      plan.capacity[list] = static_cast<uint32_t>(
          (static_cast<uint64_t>(std::min(room, kGrassMostBlades)) + 7) / 8 *
          8);
    }
  // Theirs likewise (a multiple of 64, as it's cheap).
  if (casting > field.shadowCapacity)
    plan.shadowCapacity = static_cast<uint32_t>(
        (static_cast<uint64_t>(std::min(casting * 1.25, kGrassMostBlades)) +
         63) /
        64 * 64);
  uniforms.capacity = glm::uvec4(plan.capacity[0],
                                 plan.capacity[1],
                                 plan.capacity[2],
                                 plan.shadowCapacity);
  plan.workgroups = glm::uvec3(static_cast<uint32_t>(most.x + 7) / 8,
                               static_cast<uint32_t>(most.y + 7) / 8,
                               4);
  plan.grows = true;
  return plan;
}

} // namespace sfs
