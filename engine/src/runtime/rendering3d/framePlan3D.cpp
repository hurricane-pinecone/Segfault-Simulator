#include "engine/runtime/rendering3d/framePlan3D.h"

#include "engine/core/rendering/3d/frustum.h"
#include "engine/core/rendering/3d/materialGraphCompiler.h"
#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/constants.hpp"
#include "glm/glm/matrix.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace sfs
{

namespace
{
// How a grass field's grown, by distance (fractions of its draw distance):
// each lattice's reach (see mesh3DGrassGrow.wgsl), and to where blades have
// 5 segments, then 2 (then 1).
constexpr std::array<float, 4> kGrassReach = {0.2f, 0.4f, 0.65f, 1.0f};
constexpr float kGrassFineDetail = 0.12f;
constexpr float kGrassMidDetail = 0.35f;
// Most blades a field grows a frame (its lists all together), and the most
// cells a side of a lattice.
constexpr double kGrassMostBlades = 4.0e6;
constexpr int kGrassMostCells = 2048;
// How much grass's colour covers the ground it grows on, where it's full.
constexpr float kGrassGroundTint = 0.9f;
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

void FramePlan3D::begin(const FrameParams3D& frame)
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
  m_casterBatches.clear();
  m_casters.clear();
  m_shadowedLights.clear();
  m_grassDraws.clear();
  m_uiQuads.clear();
  m_open = true;
}

void FramePlan3D::addMesh(const MeshBatch3D& batch,
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

void FramePlan3D::addGrass(const GrassBatch3D& batch)
{
  if (m_open && batch.grass.valid())
    m_grassDraws.push_back(batch);
}

void FramePlan3D::addUi(const UiDrawList& list)
{
  if (m_open)
    m_uiQuads.insert(m_uiQuads.end(), list.quads.begin(), list.quads.end());
}

void FramePlan3D::finish(
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
  const std::size_t count = std::min(m_pointLights.size(), kMaxPointLights3D);
  const std::size_t budget = std::min<std::size_t>(
      m_frame.shadows.pointLights, kMaxShadowedPointLights);
  for (std::size_t i = 0; i < count && m_shadowedLights.size() < budget; ++i)
    if (m_pointLights[i].castsShadow)
      m_shadowedLights.push_back({i, {}});
  for (ShadowedLight& shadowed : m_shadowedLights)
  {
    const PointLight3D& light = m_pointLights[shadowed.light];
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

  // Each shadowed light's cube faces.
  m_faces.clear();
  for (const ShadowedLight& shadowed : m_shadowedLights)
  {
    const PointLight3D& light = m_pointLights[shadowed.light];
    const float reach = std::max(light.radius, 1e-3f);
    const float near = std::clamp(light.shadowNear, 0.01f, reach * 0.5f);
    const glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::half_pi<float>(), 1.0f, near, reach);
    for (int face = 0; face < 6; ++face)
      m_faces.push_back({projection * cubeFaceView(face, light.position),
                         glm::vec4(light.position, 1.0f / reach)});
  }

  // The frame's uniforms.
  const SceneLighting3D& lighting = m_frame.lighting;
  const ShadowSettings3D& shadows = m_frame.shadows;
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
  m_uniforms.shadow = glm::vec4(shadows.enabled ? 1.0f : 0.0f,
                                shadows.depthBias,
                                shadows.softness / resolution,
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

  const std::span<const PointLight3D> lights(m_pointLights.data(), count);
  ClusterView view;
  view.viewProjection = m_frame.viewProjection;
  view.viewport = glm::vec2(viewport);
  view.cameraPosition = m_frame.cameraPosition;
  view.forward = m_frame.cameraForward;
  assignLights(m_grid, view, lights, m_clusters);

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

PostUniforms FramePlan3D::bloomUniforms(glm::ivec2 sourceSize) const
{
  const BloomSettings3D& bloom = m_frame.bloom;
  return {glm::vec4(1.0f / glm::vec2(sourceSize), 0.0f, 0.0f),
          glm::vec4(std::max(bloom.threshold, 0.0f),
                    std::max(bloom.softness, 0.0f),
                    std::max(bloom.radius, 0.0f),
                    std::max(bloom.intensity, 0.0f))};
}

PostUniforms FramePlan3D::compositeUniforms() const
{
  const BloomSettings3D& bloom = m_frame.bloom;
  return {glm::vec4(0.0f),
          glm::vec4(0.0f,
                    0.0f,
                    0.0f,
                    bloom.enabled ? std::max(bloom.intensity, 0.0f) : 0.0f)};
}

std::optional<FocusUniforms>
FramePlan3D::focusUniforms(glm::ivec2 viewport) const
{
  const DepthOfFieldSettings3D& settings = m_frame.depthOfField;
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

std::optional<GrassPlan> planGrass(const GrassBatch3D& batch,
                                   const GrassFieldState& field,
                                   const FrameParams3D& frame)
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

  GrassPlan plan;
  // How it's drawn (its ground, even with no blades in reach).
  plan.draw = {field.trampleBounds,
               glm::vec4(std::clamp(batch.stiffness, 0.0f, 1.0f),
                         distance,
                         kGrassGroundTint,
                         shape.height),
               maps,
               model,
               normalMatrix,
               glm::inverse(model)};

  GrassGrowUniforms& uniforms = plan.grow;
  uniforms.model = model;
  uniforms.normalMatrix = normalMatrix;
  uniforms.planes = Frustum::fromViewProjection(frame.viewProjection).planes();
  uniforms.camera = glm::vec4(frame.cameraPosition, distance);
  const GrassGrid& ground = field.groundGrid;
  uniforms.ground = glm::vec4(ground.min, ground.texelSize());
  uniforms.groundSize = glm::ivec4(ground.size, 0, 0);
  uniforms.maps = maps;
  uniforms.shape =
      glm::vec4(shape.height, shape.heightVariation, shape.width, cell);
  uniforms.detail = glm::vec4(
      distance * kGrassFineDetail, distance * kGrassMidDetail, 0.0f, 0.0f);

  // Each lattice's cells within its reach of the camera, on the ground.
  const glm::vec2 camera(glm::inverse(model) *
                         glm::vec4(frame.cameraPosition, 1.0f));
  glm::ivec2 most(0);
  for (int k = 0; k < 4; ++k)
  {
    const float reach = distance * kGrassReach[k];
    uniforms.reach[k] = reach;
    const float size = cell * static_cast<float>(1 << k);
    const float own = reach / least;
    const glm::vec2 low = glm::max(camera - own, ground.min);
    const glm::vec2 high = glm::min(camera + own, ground.max);
    glm::ivec2 first(glm::floor((low - ground.min) / size));
    glm::ivec2 count =
        glm::max(glm::ivec2(glm::ceil((high - ground.min) / size)) - first,
                 glm::ivec2(0));
    // Too many: those nearest the camera.
    for (int axis = 0; axis < 2; ++axis)
      if (count[axis] > kGrassMostCells)
      {
        first[axis] = static_cast<int>(std::floor(
                          (camera[axis] - ground.min[axis]) / size)) -
                      kGrassMostCells / 2;
        count[axis] = kGrassMostCells;
      }
    uniforms.lattices[k] = glm::ivec4(first, count);
    most = glm::max(most, count);
  }
  plan.capacity = field.capacity;
  if (most.x <= 0 || most.y <= 0)
    return plan;

  // Room for each list: about how many blades grow within its distances (all
  // round), but no more than the whole field has.
  const auto within = [&](float radius)
  {
    double count = 0.0;
    for (int k = 0; k < 4; ++k)
    {
      const double reach = std::min(radius, uniforms.reach[k]);
      count += density * (k < 3 ? 0.75 : 1.0) / double(1 << (2 * k)) *
               glm::pi<double>() * reach * reach;
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
  uniforms.capacity =
      glm::uvec4(plan.capacity[0], plan.capacity[1], plan.capacity[2], 0);
  plan.workgroups = glm::uvec3(static_cast<uint32_t>(most.x + 7) / 8,
                               static_cast<uint32_t>(most.y + 7) / 8,
                               4);
  plan.grows = true;
  return plan;
}

} // namespace sfs
