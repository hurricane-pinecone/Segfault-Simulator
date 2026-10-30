#pragma once

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/frame.h"
#include "engine/runtime/rendering/uiDraw.h"

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace sfs
{

/**
 * Coarse draw order for draw commands. Opaque geometry draws first (sorted to
 * minimise state changes). A Transparent command's see-through instances (a
 * particle effect, a 2D game's soft sprites) draw together after it, sorted
 * back to front among themselves, in one call however many there are; an
 * Opaque command's each sort among every see-through thing in the scene.
 */
enum class RenderPass : uint8_t
{
  Opaque = 0,
  Transparent,
};

/** Every instance of one mesh this frame, split into seen and shadow-casting.
 */
struct MeshDrawCommand
{
  RenderPass pass = RenderPass::Opaque;
  MeshHandle mesh;
  std::vector<MaterialHandle>
      materials; // per section, in place of its own (see MeshBatch)
  std::vector<MeshInstance> instances;
  std::vector<MeshInstance> shadowInstances;
  std::vector<MeshInstance> lightShadowInstances; // see MeshBatch
};

/** A grass field to draw (see GrassBatch), drawn with the opaque things. */
struct GrassDrawCommand
{
  GrassHandle grass;
  MaterialHandle material;
  MeshHandle mesh;
  glm::mat4 model{1.0f};
  GrassShape shape;
  float stiffness = 0.4f;
  float drawDistance = 40.0f;
};

/** A frame's UI (see UiModule), drawn over everything else. */
struct UiDrawCommand
{
  std::shared_ptr<const UiDrawList> list;
};

/** The command set render modules emit (see RenderCommandTraits). */
using AnyRenderCommand =
    std::variant<MeshDrawCommand, GrassDrawCommand, UiDrawCommand>;

} // namespace sfs
