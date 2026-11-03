#pragma once

#include "engine/core/rendering/ambientOcclusion.h"
#include "engine/core/rendering/colorGrading.h"
#include "engine/core/rendering/contactShadows.h"
#include "engine/core/rendering/fog.h"
#include "engine/core/rendering/grass.h"
#include "engine/core/rendering/lighting.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/rain.h"
#include "engine/core/rendering/sky.h"
#include "engine/core/rendering/sunShadow.h"
#include "engine/core/rendering/weather.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int4.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace sfs
{

/**
 * Most point lights a frame hands the renderer. The render system keeps the
 * ones nearest the camera target when a scene has more. Each pixel only
 * lights from the ones near it (clustered lighting; see lightClusters.h), so
 * many lights cost little where they don't overlap.
 */
inline constexpr std::size_t kMaxPointLights = 4096;

/**
 * One drawn copy of a mesh: where it is, how it's tinted, and which part of
 * its textures it shows. `uvRect` maps the mesh's UVs into a rectangle (x, y:
 * its corner; z, w: its size): the whole texture by default, or one frame of a
 * sprite sheet, so a sprite is a quad mesh whose instances pick their frames.
 * `emission` (rgb, linear) is light it gives off on top of its materials'
 * (MeshRendererComponent::emission): a lamp switched on.
 */
struct MeshInstance
{
  glm::mat4 model{1.0f};
  glm::vec4 tint{1.0f};
  glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
  glm::vec4 emission{0.0f};
};

/**
 * A frame's sky. Flat (no `physical` model): SceneLighting's sky colour and
 * the engine's classic lighting. Physical: the background is `physical`'s
 * atmosphere seen from the ground, lit by a sun toward `toSun`, and it lights
 * the scene: `sunLight` (in place of the scene's sun colour and strength),
 * `irradiance` (its diffuse light, in place of the ambient level) and its
 * reflections (the backend renders those from the model whenever `revision`
 * changes). Once the sun has set and the moon is up, the frame's lighting's
 * sun is the moon, and `sunLight` its light; `toSun` is still the sun.
 */
struct FrameSky
{
  std::shared_ptr<const PhysicalSky> physical;
  glm::vec3 toSun{0.0f, 0.0f, 1.0f}; // unit
  glm::vec3 sunLight{0.0f};
  SkyIrradiance irradiance;
  glm::vec3 groundLight{0.0f}; // PhysicalSky::groundRadiance()
  glm::vec3 scale{1.0f};       // a visual style's on the sky (its skyScale())
  float adaptation = 1.0f;     // eyes adapted: x the sky's light (see
                               // PhysicalSky::adaptation)
  float lampScale = 1.0f;      // what lamps (point lights, emission) look
                               // like to those eyes (PhysicalSky::lampScale)
  float night = 1.0f;          // x its night glow (the moon brightens it)
  // The moon's disk: toward it (unit; below the horizon, not drawn), how
  // much of it is lit (0 new .. 1 full), and how overcast it is (0..1).
  glm::vec3 toMoon{0.0f, 0.0f, -1.0f};
  float moonPhase = 1.0f;
  float cloudCover = 0.0f;
  bool moonLit = false; // the moon is the frame's light (see above)
  // The clouds' light (none drawn while cloudCover is 0): what lights their
  // tops (the sun's at their height, or the moon's) and their undersides
  // lit through (PhysicalSky::cloudBase), neither adapted; and how heavy
  // they are (SceneWeather::cloudDarkness).
  glm::vec3 cloudLight{0.0f};
  glm::vec3 cloudBase{0.0f};
  float cloudDarkness = 0.0f;
  uint64_t revision = 0;
};

/**
 * A frame's lightning (see LightningStrike): its brightest strike's light,
 * as it flashes now (not adapted: the backend adapts it as the sky's), under
 * a physical sky. `active` while any strike is still going (afterimages
 * too): auto exposure holds while it is, so a flash brightens the scene.
 */
struct FrameLightning
{
  bool active = false;
  // A cloud-to-ground strike's channel, a line of light from `low` (the
  // ground) to `high` (the cloud base), `channelLight` a metre of it
  // (rgb; none for an in-cloud strike); and the point light in the frame's
  // that casts its shadow (-1: none).
  bool channel = false;
  glm::vec3 low{0.0f};
  glm::vec3 high{0.0f};
  glm::vec3 channelLight{0.0f};
  int shadowLight = -1;
  // The cloud base glowing around `high` (in it), `glowRadius` metres
  // across: its light (rgb, at the middle; 0: none).
  glm::vec3 cloudGlow{0.0f};
  float glowRadius = 1.0f;
};

/**
 * Everything a renderer needs for a frame besides the draws themselves:
 * camera matrices, lighting and the sun's shadow projection. Built by
 * RenderSystem; spans point into storage it keeps alive until endFrame().
 */
struct FrameParams
{
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::mat4 viewProjection{1.0f};
  glm::vec3 cameraPosition{0.0f};
  // Where grass is grown and thinned from, past the camera (for checking
  // how it changes as the camera moves, with the view held still:
  // SFS_CAMERA_STEP, see RenderSystem).
  glm::vec3 grassCameraOffset{0.0f};
  glm::vec3 cameraForward{0.0f, 0.0f, -1.0f}; // unit view direction
  // What the camera looks at, and how far round the line of sight to it
  // things that clear the view fade away (MeshMaterial::fadeNearCamera; 0:
  // none, the default: what's in front of what's looked at stays).
  glm::vec3 cameraTarget{0.0f};
  float lineOfSight = 0.0f;
  float nearPlane = 0.1f; // the camera's depth range
  float farPlane = 500.0f;

  SceneLighting lighting;
  std::span<const PointLight> pointLights;

  ShadowSettings shadows;
  glm::mat4 lightViewProjection{1.0f};
  // The sun's shadow cascades (none: the one map, lightViewProjection).
  SunCascades cascades;

  float time = 0.0f; // seconds the scene has run (animated materials)

  BloomSettings bloom;
  DepthOfFieldSettings depthOfField;
  float focusDistance = 20.0f; // the view's (where depth of field is sharp)
  WindSettings wind;
  HazeSettings haze;
  // The weather (clamped), and how far the clouds have drifted (km, within
  // kCloudPeriod).
  SceneWeather weather;
  glm::vec2 cloudDrift{0.0f};
  // How rain looks (a 3D scene's; while the weather has it); how far its
  // drops have drifted on the wind (m, in the world's XY, within
  // kRainDriftPeriod), and the wind they slant in (eased: gusts don't flick
  // them).
  RainSettings rain;
  glm::vec2 rainDrift{0.0f};
  glm::vec2 rainWind{0.0f};
  FrameLightning lightning;
  // Height fog (on: in haze's place), ambient occlusion and contact shadows
  // (all off: none).
  FogSettings fog;
  AmbientOcclusionSettings ambientOcclusion;
  ContactShadowSettings contactShadows;
  // Temporal anti-aliasing: the camera jittered a fraction of a pixel each
  // frame, and the frames blended (3D only).
  bool temporalAntiAliasing = false;

  // The last pass's tone mapping and grading; and the scene's light
  // re-balanced by a visual style (see VisualStyle).
  ColorGrading grading;
  LightBalance lightBalance;

  FrameSky sky;
};

/** A grass field the renderer holds (IRenderer::createGrass). */
struct GrassHandle
{
  uint32_t id = 0;

  bool valid() const { return id != 0; }

  bool operator==(const GrassHandle&) const = default;
};

/**
 * Where a grass field is flattened (GrassTrample::image()): a map `size`
 * texels, rows from `min`'s y, over the world XY rectangle `min` to `max`.
 * `pixels` (RGBA8) are those of texels `from` to `to` (exclusive), rows from
 * its min y: just what changed; all of them if `to` is left empty, as they must
 * be when the map's size changes.
 */
struct GrassTrampleImage
{
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  glm::ivec2 size{0, 0};
  std::span<const uint8_t> pixels;
  glm::ivec2 from{0, 0};
  glm::ivec2 to{0, 0};
};

/**
 * A rectangle of a grass field's maps (GrassMaps::pixels()): `shape` and
 * `data` RGBA8 pixels from texel `from` to `to` (exclusive), rows from its min
 * y, of maps over `grid`. An empty grid: unpainted maps (full, full-length
 * grass), no pixels.
 *
 * The renderer keeps the blades it grows from them, and grows them again
 * only where `regrow` says coverage, length or bounds changed (rectangles of
 * texels: xy min, zw max exclusive; GrassMaps::takeGrownChanged()); without
 * it, all of `from` to `to`.
 */
struct GrassMapsImage
{
  GrassGrid grid;
  glm::ivec2 from{0, 0};
  glm::ivec2 to{0, 0};
  std::span<const uint8_t> shape;
  std::span<const uint8_t> data;
  std::optional<std::span<const glm::ivec4>> regrow;
};

/**
 * A grass field's draw for a frame. The renderer grows its blades over its
 * ground (createGrass), placed by `model`, as its maps say, around the camera:
 * densest near it, thinning out to `drawDistance`.
 */
struct GrassBatch
{
  GrassHandle grass;
  MaterialHandle
      material;    // the blades' look; invalid: the renderer's own green
  MeshHandle mesh; // what it grows on (its ground's tinted to match)
  glm::mat4 model{1.0f};
  GrassShape shape;
  float stiffness = 0.4f; // how little it sways (GrassComponent::stiffness)
  float drawDistance = 40.0f;
  // Blades near the camera cast the sun's shadow (GrassComponent).
  bool castsShadows = true;
  float selfShadow = 0.5f; // darker down toward the roots (GrassComponent)
  // Its ground's own material: a terrain's meadow layers carry its ground's
  // look on past its maps (see grassFarGround()).
  MaterialHandle ground;
};

/**
 * One mesh's draws for a frame. `instances` are the copies the camera can see
 * (drawn lit); `shadowInstances` are the copies that cast into the sun's
 * shadow map. They differ because an object just off screen can still throw a
 * shadow onto visible ground. `lightShadowInstances` are the copies within
 * reach of a shadowed point light (PointLight::castsShadow), which cast into
 * their shadow maps. An instance whose tint alpha is below 1 is drawn
 * see-through.
 */
struct MeshBatch
{
  MeshHandle mesh;
  // See-through instances draw together, sorted among themselves (see
  // RenderPass::Transparent), rather than each among everything.
  bool together = false;
  // A material for each of the mesh's sections, in order, in place of its own;
  // an invalid handle (or none, past the end) keeps the section's own.
  std::span<const MaterialHandle> materials;
  std::span<const MeshInstance> instances;
  std::span<const MeshInstance> shadowInstances;
  std::span<const MeshInstance> lightShadowInstances;
};

} // namespace sfs
