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
 * A mesh pass's whole shader: the shared declarations (and the sky's
 * helpers), the surface interface, a material's surface, then the pass
 * itself.
 */
inline std::string meshPass(std::string_view surface, std::string_view pass)
{
  std::string source(meshCommonWgsl);
  source += skyCommonWgsl;
  source += meshSurfaceWgsl;
  source += surface;
  source += pass;
  return source;
}

/** A physical sky's reflection passes (sky.wgsl). */
inline std::string skyPasses()
{
  return std::string(skyCommonWgsl) + std::string(skyWgsl);
}

/** A physical sky's background, drawn in the lit pass. */
inline std::string skyBackground()
{
  return std::string(meshCommonWgsl) + std::string(skyCommonWgsl) +
         std::string(skyBackgroundWgsl) + std::string(fogWgsl);
}

/**
 * The lit pass's lighting (meshLit.wgsl, with height fog), then its vertex
 * stage: meshLitVertex.wgsl's for meshes, meshGrass.wgsl's for grass.
 */
inline std::string litPass(std::string_view vertex)
{
  return std::string(meshLitWgsl) + std::string(fogWgsl) + std::string(vertex);
}

/** Grass's lit vertex stage (for litPass()): its blades, and its ground. */
inline std::string grassPass()
{
  return std::string(grassBladeWgsl) + std::string(meshGrassWgsl);
}

/** Grass's blades into the sun's shadow map (no surface: depth only). */
inline std::string grassShadowPass()
{
  return std::string(meshCommonWgsl) + std::string(grassBladeWgsl) +
         std::string(meshGrassShadowWgsl);
}

/**
 * The passes working out ambient occlusion (ambientOcclusion.wgsl): the
 * depth and normals of what's drawn (a mesh pass), then the occlusion and
 * its blur (full-screen passes).
 */
inline std::string occlusionPasses()
{
  return std::string(ambientOcclusionWgsl);
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
