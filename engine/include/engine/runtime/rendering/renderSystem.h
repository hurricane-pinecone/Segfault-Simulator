#pragma once

#include "engine/core/ecs/ecs.h" // IWYU pragma: keep
#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/lighting.h"
#include "engine/core/rendering/sky.h"
#include "engine/core/rendering/visualStyle.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/rendering/space2D.h"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace sfs
{

class WorkerPool;

class IRenderer;

/** What the last frame drew, for HUDs and profiling. */
struct RenderStats
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
 * The render system, for every style of game: owns the scene's camera,
 * lighting and meshes, hosts the render modules (sprites, particles, decals,
 * voxels, grass, UI...), and drives an IRenderer each frame. A flat or
 * isometric game gives it a Space2D (setSpace2D()).
 *
 * Each frame it builds a RenderContext (camera, frusta, lighting, the point
 * lights gathered from PointLightComponent entities), lets every module emit
 * commands, sorts them, and submits them to the renderer. Features are modules
 * (registration is the enable); MeshModule draws MeshRendererComponent
 * entities.
 */
class RenderSystem : public System, public RenderModuleHost<RenderContext>
{
public:
  explicit RenderSystem(IRenderer& renderer);
  ~RenderSystem() override;

  RenderSystem(const RenderSystem&) = delete;
  RenderSystem& operator=(const RenderSystem&) = delete;

  /**
   * The camera the scene renders through. While following the scene camera
   * (below) and the scene has a camera entity, each frame overwrites it from
   * that entity, so move the entity instead; otherwise game code drives it.
   */
  Camera& camera() { return m_camera; }
  const Camera& camera() const { return m_camera; }

  /**
   * Render through the scene's camera entity (sceneCamera()) when it has one.
   * On by default; an editor turns it off to look around freely while editing.
   */
  void setFollowSceneCamera(bool follow) { m_followSceneCamera = follow; }
  bool followsSceneCamera() const { return m_followSceneCamera; }

  /**
   * The first entity with an active CameraComponent and a
   * TransformComponent, in id order; invalid if there is none.
   */
  Entity sceneCamera() const;

  /**
   * The world-space ray through a pixel of the scene's view (e.g. the mouse
   * position, which is measured in the same pixels), from the camera as last
   * rendered.
   */
  Ray screenRay(const glm::vec2& pixel) const;

  /** The scene's uploaded meshes. Freed with this system. */
  MeshLibrary& meshes() { return m_meshes; }

  /**
   * Make this a 2D game's render system: its entities' positions are on the
   * game's plane, placed in the world by `space` -- sprites, particles and
   * decals (their modules), and the camera entity (looking straight at the
   * plane) and point lights. An entity's height above the plane is its
   * transform's z (a light's offset z adds to it); a flat game's lights shine
   * from in front of the plane (offset z: how far; at least a quarter of
   * their reach).
   */
  void setSpace2D(const Space2D& space) { m_space2D = space; }
  /** Back to a 3D game's: no Space2D. */
  void clearSpace2D() { m_space2D.reset(); }
  const std::optional<Space2D>& space2D() const { return m_space2D; }

  /**
   * Sun, ambient and sky (e.g. driven by a day/night cycle). A 2D game's sun
   * direction is in its own space: x and y across its plane (y down the
   * screen if it runs down), z up from it (toward the viewer, in a flat game).
   */
  void setLighting(const SceneLighting& lighting) { m_lighting = lighting; }
  const SceneLighting& lighting() const { return m_lighting; }

  void setShadowSettings(const ShadowSettings& settings)
  {
    m_shadows = settings;
  }
  const ShadowSettings& shadowSettings() const { return m_shadows; }

  /**
   * The scene's sky (see SkySettings): flat (its sky colour, the classic
   * lighting; the default) or physical (an atmosphere that lights the scene).
   * A 3D scene's only: a 2D game's sky is always flat.
   */
  void setSkySettings(const SkySettings& settings) { m_sky = settings; }
  const SkySettings& skySettings() const { return m_sky; }

  /**
   * Wait for a physical sky's diffuse light being worked out in the
   * background (as the sun moves), so the next frame has it. For tests.
   */
  void finishSkyWork();

  /** The scene's wind (what grass sways in). */
  void setWindSettings(const WindSettings& settings) { m_wind = settings; }
  const WindSettings& windSettings() const { return m_wind; }

  /** The scene's haze (see HazeSettings); a 3D scene's only. */
  void setHazeSettings(const HazeSettings& settings) { m_haze = settings; }
  const HazeSettings& hazeSettings() const { return m_haze; }

  /**
   * The scene's height fog (see FogSettings; on, it takes haze's place) and
   * ambient occlusion (see AmbientOcclusionSettings) and contact shadows (see
   * ContactShadowSettings); all off by default, and a 3D scene's only.
   */
  void setFogSettings(const FogSettings& settings) { m_fog = settings; }
  const FogSettings& fogSettings() const { return m_fog; }
  void setAmbientOcclusionSettings(const AmbientOcclusionSettings& settings)
  {
    m_ambientOcclusion = settings;
  }
  const AmbientOcclusionSettings& ambientOcclusionSettings() const
  {
    return m_ambientOcclusion;
  }
  /**
   * Temporal anti-aliasing (a 3D scene's; off by default): the camera moves
   * a fraction of a pixel each frame and the frames are blended, smoothing
   * edges and the shimmer of fine detail (grass, fences) as it moves.
   */
  void setTemporalAntiAliasing(bool on) { m_temporalAntiAliasing = on; }
  bool temporalAntiAliasing() const { return m_temporalAntiAliasing; }
  /** The scene's, or its visual style's (or what's forced). */
  bool effectiveTemporalAntiAliasing() const;
  /** Over the scene's and its style's, to compare (a development key). */
  void forceTemporalAntiAliasing(std::optional<bool> on)
  {
    m_forcedTemporalAntiAliasing = on;
  }

  void setContactShadowSettings(const ContactShadowSettings& settings)
  {
    m_contactShadows = settings;
  }
  const ContactShadowSettings& contactShadowSettings() const
  {
    return m_contactShadows;
  }

  /**
   * Bloom (see BloomSettings) is the scene camera's (sceneCamera(), its
   * CameraComponent's settings), even while not following it, so the
   * editor shows what the game camera sees. These are for a scene without a
   * camera entity.
   */
  void setBloomSettings(const BloomSettings& settings) { m_bloom = settings; }
  const BloomSettings& bloomSettings() const { return m_bloom; }

  /**
   * The scene's own colour grading and tone mapping (see ColorGrading): the
   * look's last step, while no visual style is active. None by default: the
   * picture as the engine always drew it.
   */
  void setColorGrading(const ColorGrading& grading) { m_grading = grading; }
  const ColorGrading& colorGrading() const { return m_grading; }

  // --- visual style ----------------------------------------------------------

  /**
   * Where visual styles come from by name ("asset:<id>" of a .style asset):
   * a StandardScene reads them from its asset store.
   */
  using StyleSource =
      std::function<std::optional<VisualStyle>(std::string_view name)>;
  void setStyleSource(StyleSource source) { m_styleSource = std::move(source); }

  /**
   * Make the scene look as a visual style says (see VisualStyle for what it
   * overrides and what it multiplies); "" for none: the scene's own settings.
   * Live: the next frame draws with it (materials switch to its variants).
   * The name is kept (and saved with the scene) even if the style can't be
   * loaded; the scene then looks as with none.
   *
   * @return whether the style is active (true for "")
   */
  bool setVisualStyle(std::string_view name);

  /**
   * Use `style` as the visual style `name` without loading it (e.g. one being
   * edited, not saved yet).
   */
  void setVisualStyle(std::string_view name, const VisualStyle& style);

  /** Load the current style again (its file changed). */
  void reloadVisualStyle() { setVisualStyle(std::string(m_styleName)); }

  /** The visual style's name ("" for none) and, if loaded, the style. */
  const std::string& visualStyleName() const { return m_styleName; }
  const VisualStyle* visualStyle() const
  {
    return m_style ? &*m_style : nullptr;
  }

  /**
   * What the scene looks like this frame: the scene's own settings with the
   * visual style (if any) applied. Bloom is the scene camera's, as the frame
   * uses it.
   */
  ColorGrading effectiveGrading() const;
  BloomSettings effectiveBloom() const;
  HazeSettings effectiveHaze() const;
  ShadowSettings effectiveShadows() const;
  SkySettings effectiveSky() const;
  FogSettings effectiveFog() const;
  AmbientOcclusionSettings effectiveAmbientOcclusion() const;
  ContactShadowSettings effectiveContactShadows() const;

  const RenderStats& stats() const { return m_stats; }

  /** Lighting, shadow and camera knobs for the debug UI. */
  std::vector<ModuleSetting> settings() override;

  /**
   * Visit each registered module with its type and the last frame's context,
   * for debug/editor UIs that read module settings().
   */
  void forEachModule(
      const std::function<void(std::type_index, Module&, const RenderContext&)>&
          fn);

  /**
   * Whether its clock runs: what animated materials and grass's wind move by
   * (FrameParams::time). Off, they hold still (the editor's Animate toggle,
   * while editing).
   */
  void setAnimating(bool animating) { m_animating = animating; }

  /** The renderer it draws through (e.g. to update a texture being painted). */
  IRenderer& renderer() { return m_renderer; }
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

  // Fit the sun's shadow (its one map, or its cascades) for this frame.
  void fitShadows(const ShadowSettings& shadows, float aspect);

  // Bring m_frameSky up to date: a physical sky's model (rebuilt when its
  // settings change), its sun's (or moon's) light, and its diffuse light
  // (worked out again, in the background, when the sun has moved or the
  // moon's glow changed far enough to show).
  void updateSky();

  // Rebuild m_pointLights from PointLightComponent entities and glowing
  // meshes (gatherEmissionLights), keeping the kMaxPointLights nearest the
  // camera target.
  void gatherPointLights();

  // Append the lights standing in for MeshRendererComponent entities' glow
  // (see emissionLights.h).
  void gatherEmissionLights();

  // Pick the frame's shadowed point lights (ShadowSettings::pointLights of
  // them) into m_shadowedLights, clearing castsShadow on the rest.
  void chooseShadowedLights();

  void submit(const MeshDrawCommand& command);
  void submit(const GrassDrawCommand& command);
  void submit(const UiDrawCommand& command);

  IRenderer& m_renderer;
  MeshLibrary m_meshes;

  Camera m_camera;
  std::optional<Space2D> m_space2D;
  bool m_followSceneCamera = true;
  SceneLighting m_lighting;
  SceneLighting m_worldLighting; // this frame's: the sun in the world's space
  ShadowSettings m_shadows;
  BloomSettings m_bloom;
  WindSettings m_wind;
  HazeSettings m_haze;
  FogSettings m_fog;
  AmbientOcclusionSettings m_ambientOcclusion;
  ContactShadowSettings m_contactShadows;
  bool m_temporalAntiAliasing = false;
  std::optional<bool> m_forcedTemporalAntiAliasing;
  SkySettings m_sky;
  ColorGrading m_grading;
  StyleSource m_styleSource;
  std::string m_styleName;
  std::optional<VisualStyle> m_style;

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

  RenderContext m_context;
  glm::mat4 m_lightViewProjection{1.0f};
  SunCascades m_cascades;
  // A physical sky: its model (for the settings it was built from), the sun
  // its diffuse light was worked out for, and what the frame gets.
  std::shared_ptr<const PhysicalSky> m_physicalSky;
  SkySettings m_physicalSkySettings;
  glm::vec3 m_skySun{0.0f};
  float m_skyNight = 1.0f; // the night glow's scale it was worked out for
  FrameSky m_frameSky;
  // This frame's real sun (m_worldLighting's is the moon's once it takes
  // over), and the moon's share of the light (moonlight()).
  glm::vec3 m_toSun{0.0f, 0.0f, 1.0f};
  float m_moonShare = 0.0f;
  bool m_moonLit = false; // the moon is the frame's directional light
  // The sky's diffuse light for a sun and night glow, worked out on
  // m_skyWorker.
  struct SkyLight
  {
    std::shared_ptr<const PhysicalSky> model;
    glm::vec3 toSun{0.0f};
    float night = 1.0f;
    SkyIrradiance irradiance;
    glm::vec3 groundLight{0.0f};
  };
  std::future<SkyLight> m_skyJob;
  std::vector<PointLight> m_pointLights;
  std::vector<PointLight> m_shadowedLights; // those casting shadows
  std::vector<std::size_t> m_shadowCandidates;
  std::vector<AnyRenderCommand> m_commands;
  double m_time = 0.0; // seconds of update() (while animating)
  bool m_animating = true;
  RenderStats m_stats;
  std::unique_ptr<WorkerPool> m_skyWorker; // last: stopped before the rest goes
};

} // namespace sfs
