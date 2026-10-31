#include "engine/runtime/rendering/renderSystem.h"

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/cameraFollowComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/pointLightComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/core/rendering/sunShadow.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/util/activeCamera.h"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <tuple>
#include <variant>

namespace sfs
{

RenderSystem::RenderSystem(IRenderer& renderer)
    : m_renderer(renderer), m_meshes(renderer)
{
}

RenderSystem::~RenderSystem() = default;

Entity RenderSystem::sceneCamera() const
{
  return registry ? findActiveCamera(*registry) : Entity{};
}

bool RenderSystem::setVisualStyle(std::string_view name)
{
  std::string chosen(name); // `name` may be m_styleName itself
  m_style.reset();
  if (!chosen.empty())
  {
    if (m_styleSource)
      m_style = m_styleSource(chosen);
    else
      LOG_ERROR("No visual styles to load '" + chosen + "' from");
  }
  m_styleName = std::move(chosen);
  m_meshes.setMaterialVariant(m_style ? m_style->materialVariant : "");
  m_meshes.setMaterialOverrides(m_style ? m_style->materials
                                        : std::map<std::string, std::string>{});
  return m_styleName.empty() || m_style.has_value();
}

void RenderSystem::setVisualStyle(std::string_view name,
                                  const VisualStyle& style)
{
  m_styleName = std::string(name);
  m_style = style;
  m_meshes.setMaterialVariant(style.materialVariant);
  m_meshes.setMaterialOverrides(style.materials);
}

ColorGrading RenderSystem::effectiveGrading() const
{
  return m_style ? m_style->grading : m_grading;
}

BloomSettings RenderSystem::effectiveBloom() const
{
  if (m_style && m_style->bloom)
    return *m_style->bloom;
  const Entity camera = sceneCamera();
  return camera.isValid()
             ? camera.getComponent<CameraComponent>().bloomSettings()
             : m_bloom;
}

HazeSettings RenderSystem::effectiveHaze() const
{
  return m_style && m_style->haze ? *m_style->haze : m_haze;
}

ShadowSettings RenderSystem::effectiveShadows() const
{
  return m_style ? m_style->shadows(m_shadows) : m_shadows;
}

SkySettings RenderSystem::effectiveSky() const
{
  return m_style && m_style->sky ? *m_style->sky : m_sky;
}

FogSettings RenderSystem::effectiveFog() const
{
  return m_style && m_style->fog ? *m_style->fog : m_fog;
}

AmbientOcclusionSettings RenderSystem::effectiveAmbientOcclusion() const
{
  return m_style && m_style->ambientOcclusion ? *m_style->ambientOcclusion
                                              : m_ambientOcclusion;
}

ContactShadowSettings RenderSystem::effectiveContactShadows() const
{
  return m_style && m_style->contactShadows ? *m_style->contactShadows
                                            : m_contactShadows;
}

Ray RenderSystem::screenRay(const glm::vec2& pixel) const
{
  // The size the scene was last drawn at: between frames a backend may report
  // another (the window, while the scene draws into an editor viewport).
  glm::ivec2 size = m_context.viewportSize;
  if (size.x <= 0 || size.y <= 0)
    size = m_renderer.viewportSize();
  return m_camera.screenRay(
      pixel, glm::vec2(std::max(size.x, 1), std::max(size.y, 1)));
}

void RenderSystem::render()
{
  m_stats = {};

  // Late as possible, so this frame's moves of the camera entity are seen.
  if (m_followSceneCamera && m_space2D)
  {
    // A 2D game's camera: over its point of the plane, looking straight at
    // it (up the screen: the plane's up), shaken across it.
    if (const Entity camera = sceneCamera(); camera.isValid())
    {
      const auto& lens = camera.getComponent<CameraComponent>();
      glm::vec2 point = hierarchy::worldTransform(camera).position2D();
      if (camera.hasComponent<CameraFollowComponent>())
        point += camera.getComponent<CameraFollowComponent>().shakeOffset() *
                 glm::vec2(1.0f, m_space2D->ySign());
      const glm::vec3 centre = m_space2D->place(point);
      const float back = std::max(lens.farPlane * 0.5f, 1.0f);
      m_camera.setPose(glm::vec3(centre.x, centre.y, back),
                       glm::vec3(0.0f, 0.0f, -1.0f),
                       back,
                       glm::vec3(0.0f, 1.0f, 0.0f));
      m_camera.projection = lens.orthographic ? Camera::Projection::Orthographic
                                              : Camera::Projection::Perspective;
      const float magnify = std::max(lens.zoom, 0.01f);
      m_camera.orthoHeight = lens.orthoHeight / magnify;
      m_camera.fieldOfView = glm::radians(lens.fieldOfView);
      m_camera.nearPlane = lens.nearPlane;
      m_camera.farPlane = lens.farPlane;
    }
    else
    {
      // No camera entity: the view the scene was saved with, over the same
      // point and as zoomed in, but always straight at the plane.
      constexpr float kBack = 50000.0f;
      m_camera.setPose(glm::vec3(m_camera.target.x, m_camera.target.y, kBack),
                       glm::vec3(0.0f, 0.0f, -1.0f),
                       kBack,
                       glm::vec3(0.0f, 1.0f, 0.0f));
      m_camera.projection = Camera::Projection::Orthographic;
      m_camera.nearPlane = 1.0f;
      m_camera.farPlane = kBack * 2.0f;
    }
  }
  else if (m_followSceneCamera)
    if (const Entity camera = sceneCamera(); camera.isValid())
    {
      camera.getComponent<CameraComponent>().applyTo(
          m_camera, hierarchy::worldMatrix(camera));

      // A follow's shake moves the view across itself (as it moves a flat
      // or isometric camera across the ground).
      if (camera.hasComponent<CameraFollowComponent>())
      {
        const glm::vec2 shake =
            camera.getComponent<CameraFollowComponent>().shakeOffset();
        const glm::vec3 right = m_camera.groundRight();
        const glm::vec3 up = glm::cross(right, m_camera.viewDirection());
        m_camera.target += right * shake.x + up * shake.y;
      }
    }

  if (!buildContext(m_renderer.viewportSize()))
    return;

  m_commands.clear();
  for (auto& [type, module] : m_modules)
    module->emit(m_context, m_commands);

  // Opaque before transparent; within a pass, group by mesh so a backend can
  // keep its bindings between neighbouring draws.
  // Grass is opaque, after the meshes.
  const auto order = [](const AnyRenderCommand& command)
  {
    if (const auto* mesh = std::get_if<MeshDrawCommand>(&command))
      return std::tuple(static_cast<int>(mesh->pass), 0, mesh->mesh.id);
    if (const auto* grass = std::get_if<GrassDrawCommand>(&command))
      return std::tuple(
          static_cast<int>(RenderPass::Opaque), 1, grass->grass.id);
    // UI last (it's drawn over the finished frame anyway).
    return std::tuple(std::numeric_limits<int>::max(), 0, 0u);
  };
  std::stable_sort(m_commands.begin(),
                   m_commands.end(),
                   [&](const AnyRenderCommand& a, const AnyRenderCommand& b)
                   { return order(a) < order(b); });

  FrameParams frame;
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
  // The look: the scene's own, or its visual style's.
  frame.bloom = effectiveBloom();
  frame.grading = effectiveGrading();
  if (m_style)
    frame.lightBalance = m_style->lightBalance();
  frame.lighting = m_worldLighting;
  frame.wind = m_wind;
  // Haze is distance from the camera, which in a 2D game is the same for
  // everything on its plane: there it would only fog the whole picture.
  const HazeSettings haze = effectiveHaze();
  frame.haze = m_space2D ? HazeSettings{0.0f, haze.distance} : haze;
  // So are height fog, ambient occlusion and contact shadows: a 3D world's.
  if (!m_space2D)
  {
    frame.fog = effectiveFog();
    frame.ambientOcclusion = effectiveAmbientOcclusion();
    frame.contactShadows = effectiveContactShadows();
  }
  // Depth of field, like bloom, is the scene camera's; focused where the view
  // is (the editor's orbit target, while it looks around freely).
  if (camera.isValid())
    frame.depthOfField =
        camera.getComponent<CameraComponent>().depthOfFieldSettings();
  frame.focusDistance = m_camera.distance;
  frame.pointLights = m_pointLights;
  frame.shadows = effectiveShadows();
  frame.shadows.enabled = m_context.shadowsEnabled;
  frame.lightViewProjection = m_lightViewProjection;
  frame.cascades = m_cascades;
  frame.time = static_cast<float>(m_time);
  frame.sky = m_frameSky;

  m_renderer.beginFrame(frame);
  for (const AnyRenderCommand& command : m_commands)
    std::visit([this](const auto& concrete) { submit(concrete); }, command);
  m_renderer.endFrame();

  m_stats.pointLights = m_pointLights.size();
}

bool RenderSystem::buildContext(const glm::ivec2& viewport)
{
  if (viewport.x <= 0 || viewport.y <= 0)
    return false;

  const float aspect =
      static_cast<float>(viewport.x) / static_cast<float>(viewport.y);

  m_context.viewportSize = viewport;
  m_context.space2D = m_space2D ? &*m_space2D : nullptr;
  m_context.camera = &m_camera;
  m_context.viewProjection = m_camera.viewProjection(aspect);
  m_context.frustum = Frustum::fromViewProjection(m_context.viewProjection);

  // A 2D game's sun shines from a direction of its own space (its plane's x
  // and y, and z up from it): into the world's.
  m_worldLighting = m_lighting;
  // A visual style tints the sky (its sun and ambient light are re-balanced
  // as they light each surface: FrameParams::lightBalance).
  if (m_style)
    m_worldLighting.skyColor *= m_style->skyScale();
  if (m_space2D)
  {
    const glm::vec3 sun = m_space2D->direction(m_lighting.sun.direction);
    if (glm::dot(sun, sun) > 0.0f)
      m_worldLighting.sun.direction = glm::normalize(sun);
  }
  const glm::vec3 sun = m_worldLighting.sun.direction;
  m_toSun = glm::dot(sun, sun) > 1e-12f ? glm::normalize(sun)
                                        : glm::vec3(0.0f, 0.0f, 1.0f);

  // Clouds dim the sun. Once it has set, the moon (a 3D world's) is the
  // light that shines and casts shadows, fading in through twilight.
  const float clear = clearSky(m_worldLighting.cloudCover);
  m_worldLighting.sun.diffuseStrength *= clear;
  m_moonShare = m_space2D ? 0.0f : moonlight(m_worldLighting);
  m_moonLit = m_moonShare > 0.0f && m_toSun.z <= 0.0f;
  if (m_moonLit)
  {
    const MoonLight& moon = m_worldLighting.moon;
    m_worldLighting.sun.direction = glm::normalize(moon.direction);
    m_worldLighting.sun.color = moon.color;
    m_worldLighting.sun.diffuseStrength = moon.diffuseStrength * m_moonShare;
  }

  // A sun (or moon) below the horizon casts nothing, so skip the shadow pass
  // entirely.
  m_context.shadowsEnabled =
      m_shadows.enabled && m_worldLighting.sun.direction.z > 0.0f;
  m_cascades = {};
  if (m_context.shadowsEnabled)
    fitShadows(effectiveShadows(), aspect);
  updateSky();

  gatherPointLights();
  m_context.lighting = &m_worldLighting;
  m_context.pointLights = &m_pointLights;
  m_context.shadowedLights = &m_shadowedLights;
  m_context.meshes = &m_meshes;
  return true;
}

void RenderSystem::fitShadows(const ShadowSettings& shadows, float aspect)
{
  const uint32_t resolution = m_renderer.shadowMapResolution();
  // Cascades are a 3D world's, and only as many as the renderer can hold.
  const uint32_t cascades =
      m_space2D ? 1u
                : std::min({shadows.cascades,
                            m_renderer.capabilities().sunShadowCascades,
                            kMaxShadowCascades});
  if (cascades <= 1)
  {
    m_lightViewProjection = fitSunShadow(m_worldLighting.sun.direction,
                                         m_camera.target,
                                         shadows.distance,
                                         resolution);
    m_context.shadowFrustum =
        Frustum::fromViewProjection(m_lightViewProjection);
    return;
  }

  CascadeView view;
  view.position = m_camera.position();
  view.forward = m_camera.viewDirection();
  view.perspective = m_camera.projection == Camera::Projection::Perspective;
  if (view.perspective)
  {
    const float up = std::tan(m_camera.fieldOfView * 0.5f);
    view.halfExtent = {up * aspect, up};
  }
  else
    view.halfExtent = glm::vec2(aspect, 1.0f) * (m_camera.orthoHeight * 0.5f);
  view.nearPlane = m_camera.nearPlane;
  view.farPlane = m_camera.farPlane;
  m_cascades = fitSunCascades(m_worldLighting.sun.direction,
                              view,
                              cascades,
                              shadows.cascadeDistance,
                              shadows.cascadeSplit,
                              resolution);
  m_lightViewProjection = m_cascades.cascades[0].viewProjection;
  m_context.shadowFrustum = Frustum::fromViewProjection(
      cascadeBounds(m_worldLighting.sun.direction, m_cascades));
}

void RenderSystem::updateSky()
{
  const SkySettings settings = effectiveSky();
  if (!settings.physical() || m_space2D)
  {
    m_frameSky.physical.reset();
    return;
  }

  const bool rebuilt = !m_physicalSky || settings != m_physicalSkySettings;
  if (rebuilt)
  {
    m_physicalSky = std::make_shared<const PhysicalSky>(settings);
    m_physicalSkySettings = settings;
  }
  const glm::vec3 toSun = m_toSun;
  const float night = PhysicalSky::nightScale(m_moonShare);

  // The sky's diffuse light and reflections change slowly with the sun (and
  // the moon's glow): work them out again once it's moved a quarter of a
  // degree (a day/night cycle moves it every frame) or the glow changed a
  // few percent. That takes a couple of milliseconds, so it's done in the
  // background, the frame keeping the last until it's ready; but a new sky
  // has none to keep.
  auto workOut =
      [](std::shared_ptr<const PhysicalSky> model, glm::vec3 sun, float glow)
  {
    SkyLight light;
    light.irradiance = model->irradiance(sun, glow);
    light.groundLight = model->groundRadiance(sun, glow);
    light.model = std::move(model);
    light.toSun = sun;
    light.night = glow;
    return light;
  };
  auto use = [this](const SkyLight& light)
  {
    m_skySun = light.toSun;
    m_skyNight = light.night;
    m_frameSky.toSun = light.toSun;
    m_frameSky.irradiance = light.irradiance;
    m_frameSky.groundLight = light.groundLight;
    m_frameSky.night = light.night;
    ++m_frameSky.revision;
  };
  if (m_skyJob.valid() &&
      m_skyJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    const SkyLight light = m_skyJob.get();
    // (Unless the sky it was for has gone since.)
    if (light.model == m_physicalSky)
      use(light);
  }
  if (rebuilt || !m_frameSky.physical)
    use(workOut(m_physicalSky, toSun, night));
  else
  {
    constexpr float kResky = 0.99999048f; // cos(0.25 degrees)
    const bool moved = glm::dot(toSun, m_skySun) < kResky ||
                       std::abs(night - m_skyNight) > 0.03f * m_skyNight;
    if (moved && !m_skyJob.valid())
    {
      if (!m_skyWorker)
        m_skyWorker = std::make_unique<WorkerPool>(1);
      auto job = std::make_shared<std::packaged_task<SkyLight()>>(
          [workOut, model = m_physicalSky, toSun, night]
          { return workOut(model, toSun, night); });
      m_skyJob = job->get_future();
      m_skyWorker->submit([job] { (*job)(); });
    }
  }
  m_frameSky.physical = m_physicalSky;
  // The sun itself, every frame (it's cheap), so it sets smoothly; once it
  // has, the moon. Then what eyes adapt to.
  const float clear = clearSky(m_worldLighting.cloudCover);
  const glm::vec3 toLight =
      m_moonLit ? glm::normalize(m_worldLighting.sun.direction) : toSun;
  m_frameSky.sunLight = m_moonLit
                            ? m_physicalSky->moonLight(toLight, m_moonShare)
                            : m_physicalSky->sunLight(toSun) * clear;
  m_frameSky.adaptation = m_physicalSky->adaptation(
      toLight,
      m_frameSky.sunLight,
      m_frameSky.irradiance.at(glm::vec3(0.0f, 0.0f, 1.0f)));
  m_frameSky.lampScale = m_physicalSky->lampScale(m_frameSky.adaptation);
  m_frameSky.scale = m_style ? m_style->skyScale() : glm::vec3(1.0f);
  // The moon's disk, unless it's below the horizon or behind clouds.
  const MoonLight& moon = m_worldLighting.moon;
  m_frameSky.toMoon = glm::dot(moon.direction, moon.direction) > 1e-12f
                          ? glm::normalize(moon.direction)
                          : glm::vec3(0.0f, 0.0f, -1.0f);
  m_frameSky.moonPhase = std::clamp(moon.phase, 0.0f, 1.0f);
  m_frameSky.cloudCover = std::clamp(m_worldLighting.cloudCover, 0.0f, 1.0f);
  m_frameSky.moonLit = m_moonLit;
}

void RenderSystem::finishSkyWork()
{
  if (m_skyJob.valid())
    m_skyJob.wait();
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
  case AlphaMode::Additive:
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

void RenderSystem::gatherEmissionLights()
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
       registry->view<TransformComponent, MeshRendererComponent>())
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

void RenderSystem::gatherPointLights()
{
  m_pointLights.clear();
  if (!registry)
    return;

  for (const Entity& entity :
       registry->view<TransformComponent, PointLightComponent>())
  {
    const auto& light = entity.getComponent<PointLightComponent>();
    if (light.intensity <= 0.0f || light.radius <= 0.0f)
      continue;

    // A 2D game's lights are where they are on its plane, at their height
    // above it (offset z). A flat game's shine from in front of the plane.
    float radius = light.radius;
    glm::vec3 position;
    if (m_space2D)
    {
      radius *= m_space2D->scale();
      const TransformComponent transform = hierarchy::worldTransform(entity);
      const glm::vec2 offset(light.offset.x, light.offset.y);
      if (m_space2D->projection == Space2D::Projection::Isometric)
        position = m_space2D->place(transform.position2D() + offset,
                                    transform.position.z + light.offset.z);
      else
      {
        position = m_space2D->place(transform.position2D() + offset);
        position.z += std::max(light.offset.z, radius * 0.25f);
      }
    }
    else
      position = glm::vec3(hierarchy::worldMatrix(entity) *
                           glm::vec4(light.offset, 1.0f));
    m_pointLights.push_back(PointLight{
        position,
        light.color,
        light.intensity,
        radius,
        light.castsShadow,
        PointLight{}.shadowNear,
        light.sourceRadius,
    });
  }
  const std::size_t placed = m_pointLights.size();
  gatherEmissionLights();
  m_stats.emissionLights = m_pointLights.size() - placed;

  if (m_pointLights.size() > kMaxPointLights)
  {
    // Keep the lights nearest what the camera is looking at.
    const glm::vec3 focus = m_camera.target;
    const auto nearer = [&focus](const PointLight& a, const PointLight& b)
    {
      const glm::vec3 da = a.position - focus;
      const glm::vec3 db = b.position - focus;
      return glm::dot(da, da) < glm::dot(db, db);
    };
    std::partial_sort(m_pointLights.begin(),
                      m_pointLights.begin() + kMaxPointLights,
                      m_pointLights.end(),
                      nearer);
    m_pointLights.resize(kMaxPointLights);
  }
  chooseShadowedLights();
}

void RenderSystem::chooseShadowedLights()
{
  // Of the lights that cast shadows and reach into view, the ones nearest
  // what the camera looks at (nearer the more they reach), up to the budget.
  // The rest cast none this frame.
  m_shadowedLights.clear();
  std::vector<std::size_t>& candidates = m_shadowCandidates;
  candidates.clear();
  for (std::size_t i = 0; i < m_pointLights.size(); ++i)
  {
    PointLight& light = m_pointLights[i];
    if (!light.castsShadow)
      continue;
    // By day, under a physical sky, a lamp too dim to see casts nothing
    // either.
    if (m_frameSky.physical &&
        PhysicalSky::lampShadow(light.intensity, m_frameSky.lampScale) <= 0.0f)
    {
      light.castsShadow = false;
      continue;
    }
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
      const PointLight& light = m_pointLights[i];
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

void RenderSystem::submit(const UiDrawCommand& command)
{
  if (command.list && !command.list->quads.empty())
    m_renderer.drawUi(*command.list);
}

void RenderSystem::submit(const GrassDrawCommand& command)
{
  if (!m_renderer.capabilities().grass)
    return;
  m_renderer.drawGrass(GrassBatch{command.grass,
                                  command.material,
                                  command.mesh,
                                  command.model,
                                  command.shape,
                                  command.stiffness,
                                  command.drawDistance,
                                  command.castsShadows,
                                  command.selfShadow});
  m_stats.grassFields += 1;
}

void RenderSystem::submit(const MeshDrawCommand& command)
{
  if (command.instances.empty() && command.shadowInstances.empty() &&
      command.lightShadowInstances.empty())
    return;

  // Commands name meshes by library handle; a stand-in that has loaded since
  // is a different renderer mesh.
  m_renderer.drawMesh(MeshBatch{m_meshes.gpuHandle(command.mesh),
                                command.pass == RenderPass::Transparent,
                                command.materials,
                                command.instances,
                                command.shadowInstances,
                                command.lightShadowInstances});

  m_stats.drawCalls += 1;
  m_stats.instances += command.instances.size();
  m_stats.shadowInstances += command.shadowInstances.size();
  m_stats.lightShadowInstances += command.lightShadowInstances.size();
}

std::vector<ModuleSetting> RenderSystem::settings()
{
  std::vector<ModuleSetting> out = {
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
      settings::floatRange(
          "Moon phase",
          0.0f,
          1.0f,
          [this] { return m_lighting.moon.phase; },
          [this](float v) { m_lighting.moon.phase = v; }),
      settings::floatRange(
          "Moon strength",
          0.0f,
          1.0f,
          [this] { return m_lighting.moon.diffuseStrength; },
          [this](float v) { m_lighting.moon.diffuseStrength = v; }),
      settings::floatRange(
          "Cloud cover",
          0.0f,
          1.0f,
          [this] { return m_lighting.cloudCover; },
          [this](float v) { m_lighting.cloudCover = v; }),
      settings::boolean(
          "Sun shadows",
          [this] { return m_shadows.enabled; },
          [this](bool v) { m_shadows.enabled = v; }),
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
  };
  // One shadow map covers a distance around what the camera looks at;
  // cascades (Scene Settings' Sky & Shadows) reach along the view instead.
  if (m_shadows.cascades <= 1 || m_space2D)
    out.insert(out.begin() + 3,
               settings::floatRange(
                   "Shadow distance",
                   5.0f,
                   200.0f,
                   [this] { return m_shadows.distance; },
                   [this](float v) { m_shadows.distance = v; }));
  // Wind (what grass sways in) and haze are a 3D world's.
  if (!m_space2D)
    out.insert(out.end(),
               {
                   settings::floatRange(
                       "Wind direction",
                       0.0f,
                       360.0f,
                       [this] { return m_wind.direction; },
                       [this](float v) { m_wind.direction = v; }),
                   settings::floatRange(
                       "Wind strength",
                       0.0f,
                       1.0f,
                       [this] { return m_wind.strength; },
                       [this](float v) { m_wind.strength = v; }),
                   settings::floatRange(
                       "Wind gustiness",
                       0.0f,
                       1.0f,
                       [this] { return m_wind.gustiness; },
                       [this](float v) { m_wind.gustiness = v; }),
                   settings::floatRange(
                       "Haze",
                       0.0f,
                       1.0f,
                       [this] { return m_haze.strength; },
                       [this](float v) { m_haze.strength = v; }),
                   settings::floatRange(
                       "Haze distance",
                       5.0f,
                       1000.0f,
                       [this] { return m_haze.distance; },
                       [this](float v) { m_haze.distance = v; }),
               });
  out.push_back(settings::enumChoice(
      "Projection",
      {"Perspective", "Orthographic"},
      [this] { return static_cast<int>(m_camera.projection); },
      [this](int v)
      { m_camera.projection = static_cast<Camera::Projection>(v); }));
  return out;
}

void RenderSystem::forEachModule(
    const std::function<void(std::type_index, Module&, const RenderContext&)>&
        fn)
{
  for (auto& [type, module] : m_modules)
    fn(type, *module, m_context);
}

} // namespace sfs
