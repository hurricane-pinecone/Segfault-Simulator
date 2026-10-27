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
 * ones nearest the camera target when a scene has more.
 */
inline constexpr std::size_t kMaxPointLights3D = 32;

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

  SceneLighting3D lighting;
  std::span<const PointLight3D> pointLights;

  ShadowSettings3D shadows;
  glm::mat4 lightViewProjection{1.0f};
};

/**
 * One mesh's draws for a frame. `instances` are the copies the camera can see
 * (drawn lit); `shadowInstances` are the copies that cast into the sun's
 * shadow map. They differ because an object just off screen can still throw a
 * shadow onto visible ground.
 */
struct MeshBatch3D
{
  MeshHandle mesh;
  std::span<const MeshInstance> instances;
  std::span<const MeshInstance> shadowInstances;
};

} // namespace sfs
