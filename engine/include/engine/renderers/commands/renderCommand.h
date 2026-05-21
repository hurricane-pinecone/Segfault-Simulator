#pragma once

#include "engine/renderers/quads.h"
#include "engine/renderers/renderLayer.h"

namespace sfs
{

template <typename TQuad = Quad>
struct RenderCommand
{
  float sortKey = 0.0f;
  RenderLayer renderLayer = RenderLayer::Object;

  TQuad quad;
};

} // namespace sfs
