#pragma once

#include "engine/core/rendering/renderPass.h"
#include "engine/core/util/profiling.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/commands/renderCommand.h"
#include "glm/glm/common.hpp"

#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sfs
{

namespace detail
{
// Subpass breaks ties between commands at the same painter depth. Folding it
// into the depth as a tiny epsilon yields one monotonic key per command, which
// maps to clip-space z so the depth buffer orders them the way the painter sort
// intends.
constexpr float kClipSubpassEpsilon = 1e-4f;

inline float clipOrderKey(const RenderOrder& order)
{
  return order.depth +
         static_cast<float>(order.subpass) * kClipSubpassEpsilon;
}
} // namespace detail

// Map each command's painter sort-key to a clip-space z (gl_Position.z): onto
// the quad(s) for quad commands, and per-vertex for merged surface meshes.
// Higher sort-key = nearer the camera. Runs before batching so each quad keeps
// its own depth even after tiles are merged into texture batches.
//
// Returns the frame's (minKey, maxKey) so the decal pipeline can normalise its
// GPU-side depth identically (decals own their depth, computed in their
// shader). The window maps the highest key to clip-z -0.9 (near) and the lowest
// to 0.9 (far), leaving NDC headroom for always-on-top/behind layering.
inline std::pair<float, float>
assignClipDepth(std::vector<AnyRenderCommand>& commands)
{
  ZoneScopedN("Render: assignClipDepth");

  float minKey = std::numeric_limits<float>::max();
  float maxKey = std::numeric_limits<float>::lowest();

  const auto includeKey = [&](float key)
  {
    minKey = glm::min(minKey, key);
    maxKey = glm::max(maxKey, key);
  };

  for (const auto& command : commands)
  {
    std::visit(
        [&](const auto& concrete)
        {
          using T = std::decay_t<decltype(concrete)>;

          // Decals own their depth (computed in the decal shader from the range
          // this function returns), and have no quad to stamp -- skip them.
          if constexpr (std::is_same_v<T, DecalDrawCommand>)
            return;
          else
          {
            // UI/text draws with depth disabled, so it must not skew the range.
            if (concrete.order.pass == RenderPass::UI)
              return;

            if constexpr (std::is_same_v<T, SurfaceCommand> ||
                          std::is_same_v<T, GeometryCommand>)
            {
              // A merged surface / geometry mesh carries a per-vertex world
              // sort-key.
              for (const auto& vertex : concrete.vertices)
                includeKey(vertex.z);
            }
            else if constexpr (requires { concrete.quad.quads; })
            {
              // A merged quad batch (terrain shadows) carries a per-quad world
              // sort-key in z.
              for (const auto& quad : concrete.quad.quads)
                includeKey(quad.z);
            }
            else
            {
              const float feetKey = detail::clipOrderKey(concrete.order);
              includeKey(feetKey);

              // A sprite's top edge sits nearer than its feet (vertical
              // billboard), so its key reaches further toward the camera.
              if constexpr (std::is_same_v<T, LitQuadCommand>)
                includeKey(feetKey + concrete.quad.depthSpan);
            }
          }
        },
        command);
  }

  const float range = maxKey - minKey;
  const float invRange = range > 1e-6f ? 1.0f / range : 0.0f;

  // Higher sort-key (clamped -> 1) maps to the near plane (smaller window
  // depth). NDC headroom of [-0.9, 0.9] leaves room for future always-on-top /
  // always-behind layering without clamping against the clip planes.
  const auto toClipZ = [&](float key)
  {
    const float t = invRange > 0.0f ? (key - minKey) * invRange : 0.5f;
    const float clamped = glm::clamp(t, 0.0f, 1.0f);
    return 0.9f - 1.8f * clamped;
  };

  for (auto& command : commands)
  {
    std::visit(
        [&](auto& concrete)
        {
          using T = std::decay_t<decltype(concrete)>;

          // Decals carry no quad to remap; their shader derives depth from the
          // returned range.
          if constexpr (std::is_same_v<T, DecalDrawCommand>)
            return;
          else if constexpr (std::is_same_v<T, SurfaceCommand> ||
                             std::is_same_v<T, GeometryCommand>)
          {
            // Remap each vertex's world sort-key to clip-space z in place.
            for (auto& vertex : concrete.vertices)
              vertex.z = toClipZ(vertex.z);
          }
          else if constexpr (requires { concrete.quad.quads; })
          {
            // Merged quad batch (terrain shadows): remap each quad's own world
            // sort-key so a single batch occludes correctly per tile.
            for (auto& quad : concrete.quad.quads)
              quad.z = toClipZ(quad.z);
          }
          else
          {
            const float feetKey = detail::clipOrderKey(concrete.order);
            concrete.quad.z = toClipZ(feetKey);

            // Give sprites a depth gradient over their height that matches the
            // block-face geometry's elevation weighting, so a billboard sorts
            // per-pixel like the vertical surface it depicts.
            if constexpr (std::is_same_v<T, LitQuadCommand>)
              concrete.quad.zTop = toClipZ(feetKey + concrete.quad.depthSpan);
          }
        },
        command);
  }

  return {minKey, maxKey};
}

} // namespace sfs
