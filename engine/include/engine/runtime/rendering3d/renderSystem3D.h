#pragma once

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep
#include "engine/core/rendering/3d/camera3D.h"
#include "engine/core/rendering/3d/lighting3D.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "engine/runtime/rendering3d/commands3D.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderContext3D.h"

#include <cstddef>
#include <functional>
#include <typeindex>
#include <vector>

namespace sfs
{

class IRenderer3D;

/** What the last frame drew, for HUDs and profiling. */
struct RenderStats3D
{
  std::size_t drawCalls = 0;
  std::size_t instances = 0;
  std::size_t shadowInstances = 0;
  std::size_t pointLights = 0;
};

/**
 * The 3D render system: owns the scene's camera, lighting and meshes, hosts
 * the 3D render modules, and drives an IRenderer3D each frame. The 3D
 * counterpart of IsometricRenderSystem.
 *
 * Each frame it builds a RenderContext3D (camera, frusta, lighting, the point
 * lights gathered from PointLight3DComponent entities), lets every module emit
 * commands, sorts them, and submits them to the renderer. Features are modules
 * (registration is the enable); MeshModule draws MeshRendererComponent
 * entities.
 */
class RenderSystem3D : public System, public RenderModuleHost<RenderContext3D>
{
public:
  explicit RenderSystem3D(IRenderer3D& renderer);

  RenderSystem3D(const RenderSystem3D&) = delete;
  RenderSystem3D& operator=(const RenderSystem3D&) = delete;

  /** The camera the scene renders through; game code drives it. */
  Camera3D& camera() { return m_camera; }
  const Camera3D& camera() const { return m_camera; }

  /** The scene's uploaded meshes. Freed with this system. */
  MeshLibrary& meshes() { return m_meshes; }

  /** Sun, ambient and sky (e.g. driven by a day/night cycle). */
  void setLighting(const SceneLighting3D& lighting) { m_lighting = lighting; }
  const SceneLighting3D& lighting() const { return m_lighting; }

  void setShadowSettings(const ShadowSettings3D& settings)
  {
    m_shadows = settings;
  }
  const ShadowSettings3D& shadowSettings() const { return m_shadows; }

  const RenderStats3D& stats() const { return m_stats; }

  /** Lighting, shadow and camera knobs for the debug UI. */
  std::vector<ModuleSetting> settings() override;

  /**
   * Visit each registered module with its type and the last frame's context,
   * for debug/editor UIs that read module settings().
   */
  void forEachModule(
      const std::function<void(std::type_index, Module&, const RenderContext3D&)>&
          fn);

protected:
  void update(double deltaTime) override { updateModules(deltaTime); }
  void render() override;

  ModuleInit moduleInit() override { return {registry, nullptr}; }

private:
  // Fill m_context for a viewport; false if there is nothing to draw into.
  bool buildContext(const glm::ivec2& viewport);

  // Rebuild m_pointLights from PointLight3DComponent entities, keeping the
  // kMaxPointLights3D nearest the camera target.
  void gatherPointLights();

  void submit(const MeshDrawCommand& command);

  IRenderer3D& m_renderer;
  MeshLibrary m_meshes;

  Camera3D m_camera;
  SceneLighting3D m_lighting;
  ShadowSettings3D m_shadows;

  RenderContext3D m_context;
  glm::mat4 m_lightViewProjection{1.0f};
  std::vector<PointLight3D> m_pointLights;
  std::vector<AnyRenderCommand3D> m_commands;
  RenderStats3D m_stats;
};

} // namespace sfs
