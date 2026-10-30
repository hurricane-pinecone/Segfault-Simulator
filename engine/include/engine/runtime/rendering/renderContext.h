#pragma once

#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/frustum.h"
#include "engine/core/rendering/lighting.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/space2D.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <vector>

namespace sfs
{

class MeshLibrary;

/**
 * Per-frame data RenderSystem shares with its render modules: the camera and
 * its culling frustum, the sun's shadow volume, and the frame's lighting. The
 * pointers are non-null while modules emit.
 */
struct RenderContext
{
  glm::ivec2 viewportSize{0, 0};

  const Camera* camera = nullptr;
  glm::mat4 viewProjection{1.0f};
  Frustum frustum;

  // The sun's shadow volume; a module includes a caster only if it falls
  // inside. Meaningless while shadowsEnabled is false.
  bool shadowsEnabled = false;
  Frustum shadowFrustum;

  const SceneLighting* lighting = nullptr;
  const std::vector<PointLight>* pointLights = nullptr;
  // The point lights casting shadows this frame; a module includes a caster
  // for them if it's within one's reach.
  const std::vector<PointLight>* shadowedLights = nullptr;

  // Local bounds of the scene's meshes, for culling, and the textures objects
  // use in place of their meshes' (uploaded when first asked for).
  MeshLibrary* meshes = nullptr;

  // A 2D game's: where its positions sit in the world (null in a 3D world).
  const Space2D* space2D = nullptr;
};

/** Render modules emit AnyRenderCommand. */
template <>
struct RenderCommandTraits<RenderContext>
{
  using Command = AnyRenderCommand;
};

} // namespace sfs
