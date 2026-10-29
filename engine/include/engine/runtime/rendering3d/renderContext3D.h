#pragma once

#include "engine/core/rendering/3d/camera3D.h"
#include "engine/core/rendering/3d/frustum.h"
#include "engine/core/rendering/3d/lighting3D.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering3d/commands3D.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <vector>

namespace sfs
{

class MeshLibrary;

/**
 * Per-frame data RenderSystem3D shares with its render modules: the camera and
 * its culling frustum, the sun's shadow volume, and the frame's lighting. The
 * pointers are non-null while modules emit.
 */
struct RenderContext3D
{
  glm::ivec2 viewportSize{0, 0};

  const Camera3D* camera = nullptr;
  glm::mat4 viewProjection{1.0f};
  Frustum frustum;

  // The sun's shadow volume; a module includes a caster only if it falls
  // inside. Meaningless while shadowsEnabled is false.
  bool shadowsEnabled = false;
  Frustum shadowFrustum;

  const SceneLighting3D* lighting = nullptr;
  const std::vector<PointLight3D>* pointLights = nullptr;
  // The point lights casting shadows this frame; a module includes a caster
  // for them if it's within one's reach.
  const std::vector<PointLight3D>* shadowedLights = nullptr;

  // Local bounds of the scene's meshes, for culling, and the textures objects
  // use in place of their meshes' (uploaded when first asked for).
  MeshLibrary* meshes = nullptr;
};

/** 3D render modules emit AnyRenderCommand3D. */
template <>
struct RenderCommandTraits<RenderContext3D>
{
  using Command = AnyRenderCommand3D;
};

} // namespace sfs
