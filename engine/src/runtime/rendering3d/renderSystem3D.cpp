#include "engine/runtime/rendering3d/renderSystem3D.h"

#include "engine/core/components/camera3DComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/3d/materialGraphCompiler.h"
#include "engine/core/rendering/3d/sunShadow.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

namespace sfs
{

RenderSystem3D::RenderSystem3D(IRenderer3D& renderer)
    : m_renderer(renderer), m_meshes(renderer)
{
}

Entity RenderSystem3D::sceneCamera() const
{
  if (!registry)
    return {};
  for (const Entity& entity :
       registry->view<Transform3DComponent, Camera3DComponent>())
    if (entity.getComponent<Camera3DComponent>().active)
      return entity;
  return {};
}

Ray3D RenderSystem3D::screenRay(const glm::vec2& pixel) const
{
  // The size the scene was last drawn at: between frames a backend may report
  // another (the window, while the scene draws into an editor viewport).
  glm::ivec2 size = m_context.viewportSize;
  if (size.x <= 0 || size.y <= 0)
    size = m_renderer.viewportSize();
  return m_camera.screenRay(
      pixel, glm::vec2(std::max(size.x, 1), std::max(size.y, 1)));
}

void RenderSystem3D::render()
{
  m_stats = {};

  // Late as possible, so this frame's moves of the camera entity are seen.
  if (m_followSceneCamera)
    if (const Entity camera = sceneCamera(); camera.isValid())
      camera.getComponent<Camera3DComponent>().applyTo(
          m_camera, hierarchy::worldMatrix(camera));

  if (!buildContext(m_renderer.viewportSize()))
    return;

  m_commands.clear();
  for (auto& [type, module] : m_modules)
    module->emit(m_context, m_commands);

  // Opaque before transparent; within a pass, group by mesh so a backend can
  // keep its bindings between neighbouring draws.
  std::stable_sort(m_commands.begin(),
                   m_commands.end(),
                   [](const AnyRenderCommand3D& a, const AnyRenderCommand3D& b)
                   {
                     const auto& ca = std::get<MeshDrawCommand>(a);
                     const auto& cb = std::get<MeshDrawCommand>(b);
                     if (ca.pass != cb.pass)
                       return ca.pass < cb.pass;
                     return ca.mesh.id < cb.mesh.id;
                   });

  FrameParams3D frame;
  frame.view = m_camera.viewMatrix();
  frame.projection =
      m_camera.projectionMatrix(static_cast<float>(m_context.viewportSize.x) /
                                static_cast<float>(m_context.viewportSize.y));
  frame.viewProjection = m_context.viewProjection;
  frame.cameraPosition = m_camera.position();
  frame.cameraForward = m_camera.viewDirection();
  frame.nearPlane = m_camera.nearPlane;
  frame.farPlane = m_camera.farPlane;
  const Entity camera = sceneCamera();
  frame.bloom = camera.isValid()
                    ? camera.getComponent<Camera3DComponent>().bloomSettings()
                    : m_bloom;
  frame.lighting = m_lighting;
  frame.pointLights = m_pointLights;
  frame.shadows = m_shadows;
  frame.shadows.enabled = m_context.shadowsEnabled;
  frame.lightViewProjection = m_lightViewProjection;
  frame.time = static_cast<float>(m_time);

  m_renderer.beginFrame(frame);
  for (const AnyRenderCommand3D& command : m_commands)
    std::visit([this](const auto& concrete) { submit(concrete); }, command);
  m_renderer.endFrame();

  m_stats.pointLights = m_pointLights.size();
}

bool RenderSystem3D::buildContext(const glm::ivec2& viewport)
{
  if (viewport.x <= 0 || viewport.y <= 0)
    return false;

  const float aspect =
      static_cast<float>(viewport.x) / static_cast<float>(viewport.y);

  m_context.viewportSize = viewport;
  m_context.camera = &m_camera;
  m_context.viewProjection = m_camera.viewProjection(aspect);
  m_context.frustum = Frustum::fromViewProjection(m_context.viewProjection);

  // A sun below the horizon casts nothing, so skip the shadow pass entirely.
  m_context.shadowsEnabled =
      m_shadows.enabled && m_lighting.sun.direction.z > 0.0f;
  if (m_context.shadowsEnabled)
  {
    m_lightViewProjection = fitSunShadow(m_lighting.sun.direction,
                                         m_camera.target,
                                         m_shadows.distance,
                                         m_renderer.shadowMapResolution());
    m_context.shadowFrustum =
        Frustum::fromViewProjection(m_lightViewProjection);
  }

  gatherPointLights();
  m_context.lighting = &m_lighting;
  m_context.pointLights = &m_pointLights;
  m_context.shadowedLights = &m_shadowedLights;
  m_context.meshes = &m_meshes;
  return true;
}

namespace
{

// The light a point of a graph material's surface gives off, as it's drawn:
// its emission, gone where the material cuts the point out, and faded with it
// where it's see-through.
glm::vec3 glowAt(const MeshMaterial& material,
                 const materialGraph::SurfacePoint& point)
{
  const materialGraph::SurfaceValues surface = material.shader->evaluate(
      point, material.shaderParameters, material.shaderTextures);
  const glm::vec3 emission = glm::max(surface.emission, glm::vec3(0.0f));
  switch (material.alphaMode)
  {
  case AlphaMode::Opaque:
    return emission;
  case AlphaMode::Mask:
    return surface.alpha < surface.alphaClip ? glm::vec3(0.0f) : emission;
  case AlphaMode::Blend:
    return emission * std::clamp(surface.alpha, 0.0f, 1.0f);
  }
  return emission;
}

// The light leaving a point in every direction (what lights its
// surroundings), for a material whose glow depends on where it's seen from (a
// glowing rim): its glow seen from eight directions spread as light leaves a
// surface (more straight out than sideways: two rings, at 30 and 60 degrees
// from the normal, of four each), averaged. Others' glow is the same from
// anywhere, and is worked out once.
glm::vec3 glowLeaving(const MeshMaterial& material,
                      materialGraph::SurfacePoint point,
                      bool viewDependent)
{
  if (!viewDependent)
    return glowAt(material, point);
  const glm::vec3 normal = point.normal;
  glm::vec3 sum(0.0f);
  for (int ring = 0; ring < 2; ++ring)
  {
    const float out = ring == 0 ? 0.8660254f : 0.5f;  // cos 30, cos 60
    const float side = ring == 0 ? 0.5f : 0.8660254f; // sin
    for (int k = 0; k < 4; ++k)
    {
      const float angle = 1.5707963f * (static_cast<float>(k) + 0.5f * ring);
      point.toView = normal * out + (point.tangent * std::cos(angle) +
                                     point.bitangent * std::sin(angle)) *
                                        side;
      sum += glowAt(material, point);
    }
  }
  return sum / 8.0f;
}

// What of the point a material's glow depends on (glowAt()).
materialGraph::PointInputs glowReads(const MeshMaterial& material)
{
  using namespace materialGraph;
  PointInputs reads = material.shader->reads(kEmission);
  if (material.alphaMode != AlphaMode::Opaque)
    reads |= material.shader->reads(kAlpha);
  if (material.alphaMode == AlphaMode::Mask)
    reads |= material.shader->reads(kAlphaClip);
  return reads;
}

} // namespace

namespace
{

// The point-shadow choices settings() offers.
constexpr std::array<uint32_t, 5> kPointShadowCounts{0, 4, 8, 16, 32};
constexpr std::array<uint32_t, 4> kPointShadowResolutions{128, 256, 512, 1024};

// The choice nearest `value` (it may have been set from code to another).
template <std::size_t N>
int choiceOf(const std::array<uint32_t, N>& choices, uint32_t value)
{
  int best = 0;
  for (std::size_t i = 1; i < N; ++i)
    if (std::abs(int64_t(choices[i]) - int64_t(value)) <
        std::abs(int64_t(choices[best]) - int64_t(value)))
      best = static_cast<int>(i);
  return best;
}

} // namespace

void RenderSystem3D::gatherEmissionLights()
{
  using materialGraph::PointInputs;
  m_sharedEmission.clear();
  if (!registry)
    return;

  const float time = static_cast<float>(m_time);
  // A sample as a point of the surface, in the mesh's space or (with `model`)
  // placed in the world.
  const auto pointOf = [time](const SurfaceSample& sample,
                              const glm::mat4* model,
                              const glm::mat3* normals)
  {
    materialGraph::SurfacePoint point;
    point.uv = sample.uv;
    point.vertexColor = sample.vertexColor;
    point.time = time;
    point.position = model
                         ? glm::vec3(*model * glm::vec4(sample.position, 1.0f))
                         : sample.position;
    point.normal =
        normals ? glm::normalize(*normals * sample.normal) : sample.normal;
    const glm::vec3 alongU =
        model ? glm::mat3(*model) * sample.alongU : sample.alongU;
    const glm::vec3 upImage =
        model ? glm::mat3(*model) * sample.upImage : sample.upImage;
    materialGraph::tangentFrame(
        point.normal, alongU, upImage, point.tangent, point.bitangent);
    point.toView = point.normal; // glowLeaving() looks from around it
    return point;
  };

  for (const Entity& entity :
       registry->view<Transform3DComponent, MeshRendererComponent>())
  {
    const auto& renderer = entity.getComponent<MeshRendererComponent>();
    if (!renderer.emitsLight || renderer.tint.a <= 0.0f ||
        renderer.lightStrength <= 0.0f || renderer.lightRange <= 0.0f)
      continue;
    const std::size_t sections = m_meshes.sectionCount(renderer.mesh);

    m_glow.clear();
    bool placed = false;
    glm::mat4 model(1.0f);
    glm::mat3 normals(1.0f);
    float areaScale =
        1.0f; // |det|: a sample's area scales by this x |normals x n|
    for (std::size_t s = 0; s < sections; ++s)
    {
      // The slot's material, as MeshModule draws it.
      const MeshMaterial* material = nullptr;
      if (s < renderer.materials.size())
        material =
            m_meshes.materialData(m_meshes.material(renderer.materials[s]));
      if (!material)
        material = m_meshes.sectionMaterial(renderer.mesh, s);
      if (!material || !material->shader)
        continue; // the standard surface gives off no light
      const std::span<const SurfaceSample> samples =
          m_meshes.surfaceSamples(renderer.mesh, s);
      if (samples.empty())
        continue;

      const PointInputs reads = glowReads(*material);
      const bool viewDependent = any(reads & PointInputs::ToView);
      const bool shared = !any(reads & materialGraph::kPlacedInputs);
      const std::vector<glm::vec3>* sharedGlow = nullptr;
      if (shared)
      {
        // Worked out once a frame for every object with these samples and
        // material; empty if none of it glows.
        auto [it, added] =
            m_sharedEmission.try_emplace(SampleKey{samples.data(), material});
        if (added)
        {
          std::vector<glm::vec3>& glow = it->second;
          if (!any(reads))
            glow.assign(
                samples.size(),
                glowAt(*material, pointOf(samples[0], nullptr, nullptr)));
          else
            for (const SurfaceSample& sample : samples)
              glow.push_back(glowLeaving(
                  *material, pointOf(sample, nullptr, nullptr), viewDependent));
          if (std::all_of(glow.begin(),
                          glow.end(),
                          [](const glm::vec3& g)
                          { return g == glm::vec3(0.0f); }))
            glow.clear();
        }
        if (it->second.empty())
          continue;
        sharedGlow = &it->second;
      }

      if (!placed)
      {
        placed = true;
        model = hierarchy::worldMatrix(entity);
        normals = glm::transpose(glm::inverse(glm::mat3(model)));
        areaScale = std::abs(glm::determinant(glm::mat3(model)));
      }
      for (std::size_t i = 0; i < samples.size(); ++i)
      {
        const SurfaceSample& sample = samples[i];
        const glm::vec3 glow =
            sharedGlow ? (*sharedGlow)[i]
                       : glowLeaving(*material,
                                     pointOf(sample, &model, &normals),
                                     viewDependent);
        if (glow == glm::vec3(0.0f))
          continue;
        m_glow.push_back(GlowPoint{
            glm::vec3(model * glm::vec4(sample.position, 1.0f)),
            sample.area * areaScale * glm::length(normals * sample.normal),
            glow * renderer.tint.a,
        });
      }
    }
    const std::size_t first = m_pointLights.size();
    emissionLights(
        m_glow, renderer.lightStrength, renderer.lightRange, m_pointLights);
    if (!renderer.lightCastsShadows)
      for (std::size_t i = first; i < m_pointLights.size(); ++i)
        m_pointLights[i].castsShadow = false;
  }
}

void RenderSystem3D::gatherPointLights()
{
  m_pointLights.clear();
  if (!registry)
    return;

  for (const Entity& entity :
       registry->view<Transform3DComponent, PointLight3DComponent>())
  {
    const auto& light = entity.getComponent<PointLight3DComponent>();
    if (light.intensity <= 0.0f || light.radius <= 0.0f)
      continue;

    const glm::mat4 model = hierarchy::worldMatrix(entity);
    m_pointLights.push_back(PointLight3D{
        glm::vec3(model * glm::vec4(light.offset, 1.0f)),
        light.color,
        light.intensity,
        light.radius,
        light.castsShadow,
        PointLight3D{}.shadowNear,
        light.sourceRadius,
    });
  }
  const std::size_t placed = m_pointLights.size();
  gatherEmissionLights();
  m_stats.emissionLights = m_pointLights.size() - placed;

  if (m_pointLights.size() > kMaxPointLights3D)
  {
    // Keep the lights nearest what the camera is looking at.
    const glm::vec3 focus = m_camera.target;
    const auto nearer = [&focus](const PointLight3D& a, const PointLight3D& b)
    {
      const glm::vec3 da = a.position - focus;
      const glm::vec3 db = b.position - focus;
      return glm::dot(da, da) < glm::dot(db, db);
    };
    std::partial_sort(m_pointLights.begin(),
                      m_pointLights.begin() + kMaxPointLights3D,
                      m_pointLights.end(),
                      nearer);
    m_pointLights.resize(kMaxPointLights3D);
  }
  chooseShadowedLights();
}

void RenderSystem3D::chooseShadowedLights()
{
  // Of the lights that cast shadows and reach into view, the ones nearest
  // what the camera looks at (nearer the more they reach), up to the budget.
  // The rest cast none this frame.
  m_shadowedLights.clear();
  std::vector<std::size_t>& candidates = m_shadowCandidates;
  candidates.clear();
  for (std::size_t i = 0; i < m_pointLights.size(); ++i)
  {
    PointLight3D& light = m_pointLights[i];
    if (!light.castsShadow)
      continue;
    const glm::vec3 reach(light.radius);
    if (!m_context.frustum.intersects(
            Aabb{light.position - reach, light.position + reach}))
    {
      light.castsShadow = false;
      continue;
    }
    candidates.push_back(i);
  }

  const std::size_t budget = m_shadows.pointLights;
  if (candidates.size() > budget)
  {
    const glm::vec3 focus = m_camera.target;
    const auto score = [&](std::size_t i)
    {
      const PointLight3D& light = m_pointLights[i];
      return glm::distance(light.position, focus) - light.radius;
    };
    std::nth_element(candidates.begin(),
                     candidates.begin() + static_cast<std::ptrdiff_t>(budget),
                     candidates.end(),
                     [&](std::size_t a, std::size_t b)
                     { return score(a) < score(b); });
    for (std::size_t k = budget; k < candidates.size(); ++k)
      m_pointLights[candidates[k]].castsShadow = false;
    candidates.resize(budget);
  }
  for (const std::size_t i : candidates)
    m_shadowedLights.push_back(m_pointLights[i]);
  m_stats.shadowedLights = m_shadowedLights.size();
}

void RenderSystem3D::submit(const MeshDrawCommand& command)
{
  if (command.instances.empty() && command.shadowInstances.empty() &&
      command.lightShadowInstances.empty())
    return;

  // Commands name meshes by library handle; a stand-in that has loaded since
  // is a different renderer mesh.
  m_renderer.drawMesh(MeshBatch3D{m_meshes.gpuHandle(command.mesh),
                                  command.materials,
                                  command.instances,
                                  command.shadowInstances,
                                  command.lightShadowInstances});

  m_stats.drawCalls += 1;
  m_stats.instances += command.instances.size();
  m_stats.shadowInstances += command.shadowInstances.size();
  m_stats.lightShadowInstances += command.lightShadowInstances.size();
}

std::vector<ModuleSetting> RenderSystem3D::settings()
{
  return {
      settings::floatRange(
          "Ambient",
          0.0f,
          1.0f,
          [this] { return m_lighting.ambient; },
          [this](float v) { m_lighting.ambient = v; }),
      settings::floatRange(
          "Sun strength",
          0.0f,
          1.0f,
          [this] { return m_lighting.sun.diffuseStrength; },
          [this](float v) { m_lighting.sun.diffuseStrength = v; }),
      settings::boolean(
          "Sun shadows",
          [this] { return m_shadows.enabled; },
          [this](bool v) { m_shadows.enabled = v; }),
      settings::floatRange(
          "Shadow distance",
          5.0f,
          200.0f,
          [this] { return m_shadows.distance; },
          [this](float v) { m_shadows.distance = v; }),
      settings::floatRange(
          "Shadow softness",
          0.0f,
          4.0f,
          [this] { return m_shadows.softness; },
          [this](float v) { m_shadows.softness = v; }),
      settings::floatRange(
          "Shadow bias",
          0.0f,
          0.01f,
          [this] { return m_shadows.depthBias; },
          [this](float v) { m_shadows.depthBias = v; }),
      settings::enumChoice(
          "Point light shadows",
          {"None", "4", "8", "16", "32"},
          [this]
          { return choiceOf(kPointShadowCounts, m_shadows.pointLights); },
          [this](int v) { m_shadows.pointLights = kPointShadowCounts[v]; }),
      settings::enumChoice(
          "Point shadow detail",
          {"128", "256", "512", "1024"},
          [this] {
            return choiceOf(kPointShadowResolutions, m_shadows.pointResolution);
          },
          [this](int v)
          { m_shadows.pointResolution = kPointShadowResolutions[v]; }),
      settings::enumChoice(
          "Projection",
          {"Perspective", "Orthographic"},
          [this] { return static_cast<int>(m_camera.projection); },
          [this](int v)
          { m_camera.projection = static_cast<Camera3D::Projection>(v); }),
  };
}

void RenderSystem3D::forEachModule(
    const std::function<void(std::type_index, Module&, const RenderContext3D&)>&
        fn)
{
  for (auto& [type, module] : m_modules)
    fn(type, *module, m_context);
}

} // namespace sfs
