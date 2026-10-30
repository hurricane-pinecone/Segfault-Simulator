#pragma once

// How the engine's mesh shaders are put together, for every GPU backend: the
// WGSL each pass compiles, whichever API then runs it.

#include "engine/core/rendering/material.h"
#include "engine/core/rendering/materialGraphCompiler.h"

#include <engine/generated/embeddedWgsl.h>

#include <string>
#include <string_view>

namespace sfs::shaders
{

/**
 * A mesh pass's whole shader: the shared declarations, the surface interface,
 * a material's surface, then the pass itself.
 */
inline std::string meshPass(std::string_view surface, std::string_view pass)
{
  std::string source(meshCommonWgsl);
  source += meshSurfaceWgsl;
  source += surface;
  source += pass;
  return source;
}

/** The standard surface's WGSL (for materials without a graph). */
inline std::string standardSurface()
{
  return std::string(meshStandardSurfaceWgsl);
}

/**
 * A material graph's surface for an alpha mode. The mode is baked in, so the
 * passes' branches on it fold away.
 */
inline std::string graphSurface(const materialGraph::MaterialShader& shader,
                                AlphaMode mode)
{
  const char* constant = mode == AlphaMode::Mask       ? "ALPHA_MASK"
                         : mode == AlphaMode::Blend    ? "ALPHA_BLEND"
                         : mode == AlphaMode::Additive ? "ALPHA_ADDITIVE"
                                                       : "ALPHA_OPAQUE";
  return shader.wgsl() + "\nfn surfaceAlphaMode() -> u32 {\n  return " +
         constant + ";\n}\n";
}

} // namespace sfs::shaders
