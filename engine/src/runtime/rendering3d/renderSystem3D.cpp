#include "engine/runtime/rendering3d/renderSystem3D.h"

#include "engine/core/components/camera3DComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/3d/sunShadow.h"
#include "engine/runtime/rendering3d/iRenderer3D.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>
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
  for (const Entity& entity : registry->view<Transform3DComponent, Camera3DComponent>())
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
  return m_camera.screenRay(pixel, glm::vec2(std::max(size.x, 1), std::max(size.y, 1)));
}

void RenderSystem3D::render()
{
  m_stats = {};

  // Late as possible, so this frame's moves of the camera entity are seen.
  if (m_followSceneCamera)
    if (const Entity camera = sceneCamera(); camera.isValid())
      camera.getComponent<Camera3DComponent>().applyTo(m_camera,
                                                       hierarchy::worldMatrix(camera));

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
  frame.projection = m_camera.projectionMatrix(
      static_cast<float>(m_context.viewportSize.x) /
      static_cast<float>(m_context.viewportSize.y));
  frame.viewProjection = m_context.viewProjection;
  frame.cameraPosition = m_camera.position();
  frame.lighting = m_lighting;
  frame.pointLights = m_pointLights;
  frame.shadows = m_shadows;
  frame.shadows.enabled = m_context.shadowsEnabled;
  frame.lightViewProjection = m_lightViewProjection;

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
    m_context.shadowFrustum = Frustum::fromViewProjection(m_lightViewProjection);
  }

  gatherPointLights();
  m_context.lighting = &m_lighting;
  m_context.pointLights = &m_pointLights;
  m_context.meshes = &m_meshes;
  return true;
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
    });
  }

  if (m_pointLights.size() <= kMaxPointLights3D)
    return;

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

void RenderSystem3D::submit(const MeshDrawCommand& command)
{
  if (command.instances.empty() && command.shadowInstances.empty())
    return;

  m_renderer.drawMesh(
      MeshBatch3D{command.mesh, command.materials, command.instances, command.shadowInstances});

  m_stats.drawCalls += 1;
  m_stats.instances += command.instances.size();
  m_stats.shadowInstances += command.shadowInstances.size();
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
