#pragma once

#include "engine/core/rendering/quads.h"
#include "engine/core/rendering/renderPass.h"

namespace sfs
{

struct RenderOrder
{
  RenderPass pass = RenderPass::Objects;

  // Main painter/depth ordering.
  float depth = 0.0f;

  // Small deterministic ordering inside same depth.
  int subpass = 0;

  bool operator<(const RenderOrder& other) const
  {
    if (pass != other.pass)
      return pass < other.pass;

    if (depth != other.depth)
      return depth < other.depth;

    return subpass < other.subpass;
  }
};

template <typename TQuad = Quad>
struct RenderCommand
{
  RenderOrder order;
  TQuad quad;
};

} // namespace sfs
