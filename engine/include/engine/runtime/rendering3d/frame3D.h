#pragma once

#include "engine/core/rendering/3d/grass.h"
#include "engine/core/rendering/3d/lighting3D.h"
#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstddef>
#include <span>

namespace sfs
{

/**
 * Most point lights a frame hands the renderer. The render system keeps the
 * ones nearest the camera target when a scene has more. Each pixel only
 * lights from the ones near it (clustered lighting; see lightClusters.h), so
 * many lights cost little where they don't overlap.
 */
inline constexpr std::size_t kMaxPointLights3D = 4096;

/** One drawn copy of a mesh: where it is and how it's tinted. */
struct MeshInstance
{
  glm::mat4 model{1.0f};
  glm::vec4 tint{1.0f};
};

/**
 * Everything a 3D renderer needs for a frame besides the draws themselves:
 * camera matrices, lighting and the sun's shadow projection. Built by
 * RenderSystem3D; spans point into storage it keeps alive until endFrame().
 */
struct FrameParams3D
{
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::mat4 viewProjection{1.0f};
  glm::vec3 cameraPosition{0.0f};
  glm::vec3 cameraForward{0.0f, 0.0f, -1.0f}; // unit view direction
  float nearPlane = 0.1f;                     // the camera's depth range
  float farPlane = 500.0f;

  SceneLighting3D lighting;
  std::span<const PointLight3D> pointLights;

  ShadowSettings3D shadows;
  glm::mat4 lightViewProjection{1.0f};

  float time = 0.0f; // seconds the scene has run (animated materials)

  BloomSettings3D bloom;
  DepthOfFieldSettings3D depthOfField;
  float focusDistance = 20.0f; // the view's (where depth of field is sharp)
  WindSettings3D wind;
  HazeSettings3D haze;
};

/** A grass field the renderer holds (IRenderer3D::createGrass). */
struct GrassHandle
{
  uint32_t id = 0;

  bool valid() const { return id != 0; }

  bool operator==(const GrassHandle&) const = default;
};

/**
 * Where a grass field is flattened (GrassTrample::image()): RGBA8 pixels,
 * `size` of them, rows from `min`'s y, over the world XY rectangle `min` to
 * `max`.
 */
struct GrassTrampleImage
{
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  glm::ivec2 size{0, 0};
  std::span<const uint8_t> pixels;
};

/**
 * A rectangle of a grass field's maps (GrassMaps::pixels()): `shape` and
 * `data` RGBA8 pixels from texel `from` to `to` (exclusive), rows from its min
 * y, of maps over `grid`. An empty grid: unpainted maps (full, full-length
 * grass), no pixels.
 */
struct GrassMapsImage
{
  GrassGrid grid;
  glm::ivec2 from{0, 0};
  glm::ivec2 to{0, 0};
  std::span<const uint8_t> shape;
  std::span<const uint8_t> data;
};

/**
 * A grass field's draw for a frame. The renderer grows its blades over its
 * ground (createGrass), placed by `model`, as its maps say, around the camera:
 * densest near it, thinning out to `drawDistance`.
 */
struct GrassBatch3D
{
  GrassHandle grass;
  MaterialHandle
      material;    // the blades' look; invalid: the renderer's own green
  MeshHandle mesh; // what it grows on (its ground's tinted to match)
  glm::mat4 model{1.0f};
  GrassShape shape;
  float stiffness = 0.4f; // how little it sways (GrassComponent::stiffness)
  float drawDistance = 40.0f;
};

/**
 * One mesh's draws for a frame. `instances` are the copies the camera can see
 * (drawn lit); `shadowInstances` are the copies that cast into the sun's
 * shadow map. They differ because an object just off screen can still throw a
 * shadow onto visible ground. `lightShadowInstances` are the copies within
 * reach of a shadowed point light (PointLight3D::castsShadow), which cast into
 * their shadow maps. An instance whose tint alpha is below 1 is drawn
 * see-through.
 */
struct MeshBatch3D
{
  MeshHandle mesh;
  // A material for each of the mesh's sections, in order, in place of its own;
  // an invalid handle (or none, past the end) keeps the section's own.
  std::span<const MaterialHandle> materials;
  std::span<const MeshInstance> instances;
  std::span<const MeshInstance> shadowInstances;
  std::span<const MeshInstance> lightShadowInstances;
};

} // namespace sfs
