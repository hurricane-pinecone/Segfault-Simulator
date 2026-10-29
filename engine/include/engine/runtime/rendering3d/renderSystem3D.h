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
#include <unordered_map>
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
  std::size_t pointLights = 0; // all of them, emission's included
  std::size_t emissionLights = 0;
  std::size_t shadowedLights = 0;       // point lights casting shadows
  std::size_t lightShadowInstances = 0; // casters for them
  std::size_t grassFields = 0;
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

  /**
   * The camera the scene renders through. While following the scene camera
   * (below) and the scene has a camera entity, each frame overwrites it from
   * that entity, so move the entity instead; otherwise game code drives it.
   */
  Camera3D& camera() { return m_camera; }
  const Camera3D& camera() const { return m_camera; }

  /**
   * Render through the scene's camera entity (sceneCamera()) when it has one.
   * On by default; an editor turns it off to look around freely while editing.
   */
  void setFollowSceneCamera(bool follow) { m_followSceneCamera = follow; }
  bool followsSceneCamera() const { return m_followSceneCamera; }

  /**
   * The first entity with an active Camera3DComponent and a
   * Transform3DComponent, in id order; invalid if there is none.
   */
  Entity sceneCamera() const;

  /**
   * The world-space ray through a pixel of the scene's view (e.g. the mouse
   * position, which is measured in the same pixels), from the camera as last
   * rendered.
   */
  Ray3D screenRay(const glm::vec2& pixel) const;

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

  /** The scene's wind (what grass sways in). */
  void setWindSettings(const WindSettings3D& settings) { m_wind = settings; }
  const WindSettings3D& windSettings() const { return m_wind; }

  /** The scene's haze (see HazeSettings3D). */
  void setHazeSettings(const HazeSettings3D& settings) { m_haze = settings; }
  const HazeSettings3D& hazeSettings() const { return m_haze; }

  /**
   * Bloom (see BloomSettings3D) is the scene camera's (sceneCamera(), its
   * Camera3DComponent's settings), even while not following it, so the
   * editor shows what the game camera sees. These are for a scene without a
   * camera entity.
   */
  void setBloomSettings(const BloomSettings3D& settings) { m_bloom = settings; }
  const BloomSettings3D& bloomSettings() const { return m_bloom; }

  const RenderStats3D& stats() const { return m_stats; }

  /** Lighting, shadow and camera knobs for the debug UI. */
  std::vector<ModuleSetting> settings() override;

  /**
   * Visit each registered module with its type and the last frame's context,
   * for debug/editor UIs that read module settings().
   */
  void
  forEachModule(const std::function<
                void(std::type_index, Module&, const RenderContext3D&)>& fn);

  /**
   * Whether its clock runs: what animated materials and grass's wind move by
   * (FrameParams3D::time). Off, they hold still (the editor's Animate toggle,
   * while editing).
   */
  void setAnimating(bool animating) { m_animating = animating; }
  bool animating() const { return m_animating; }

protected:
  void update(double deltaTime) override
  {
    if (m_animating)
      m_time += deltaTime;
    updateModules(deltaTime);
  }
  void render() override;

  ModuleInit moduleInit() override { return {registry, nullptr}; }

private:
  // Fill m_context for a viewport; false if there is nothing to draw into.
  bool buildContext(const glm::ivec2& viewport);

  // Rebuild m_pointLights from PointLight3DComponent entities and glowing
  // meshes (gatherEmissionLights), keeping the kMaxPointLights3D nearest the
  // camera target.
  void gatherPointLights();

  // Append the lights standing in for MeshRendererComponent entities' glow
  // (see emissionLights.h).
  void gatherEmissionLights();

  // Pick the frame's shadowed point lights (ShadowSettings3D::pointLights of
  // them) into m_shadowedLights, clearing castsShadow on the rest.
  void chooseShadowedLights();

  void submit(const MeshDrawCommand& command);
  void submit(const GrassDrawCommand& command);
  void submit(const UiDrawCommand& command);

  IRenderer3D& m_renderer;
  MeshLibrary m_meshes;

  Camera3D m_camera;
  bool m_followSceneCamera = true;
  SceneLighting3D m_lighting;
  ShadowSettings3D m_shadows;
  BloomSettings3D m_bloom;
  WindSettings3D m_wind;
  HazeSettings3D m_haze;

  // Emission per surface sample, for one frame: per (samples, material) for
  // materials whose emission doesn't depend on where the object is (shared
  // by every object using the pair), so each is worked out once a frame.
  struct SampleKey
  {
    const SurfaceSample* samples;
    const MeshMaterial* material;
    bool operator==(const SampleKey&) const = default;
  };
  struct SampleKeyHash
  {
    std::size_t operator()(const SampleKey& key) const
    {
      return std::hash<const void*>()(key.samples) * 31u ^
             std::hash<const void*>()(key.material);
    }
  };
  std::unordered_map<SampleKey, std::vector<glm::vec3>, SampleKeyHash>
      m_sharedEmission;
  std::vector<GlowPoint> m_glow;

  RenderContext3D m_context;
  glm::mat4 m_lightViewProjection{1.0f};
  std::vector<PointLight3D> m_pointLights;
  std::vector<PointLight3D> m_shadowedLights; // those casting shadows
  std::vector<std::size_t> m_shadowCandidates;
  std::vector<AnyRenderCommand3D> m_commands;
  double m_time = 0.0; // seconds of update() (while animating)
  bool m_animating = true;
  RenderStats3D m_stats;
};

} // namespace sfs
