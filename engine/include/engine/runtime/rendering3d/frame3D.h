#pragma once

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
