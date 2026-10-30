#pragma once

#include "engine/core/rendering/lighting.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sfs
{

/**
 * A point on a mesh, standing for a share of its surface: where emission is
 * worked out to find how much light a glowing surface gives off. In the
 * mesh's own space.
 */
struct SurfaceSample
{
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f}; // unit
  glm::vec2 uv{0.0f};
  glm::vec3 vertexColor{1.0f};
  // Its triangle's texture directions (see textureDirections()), for its
  // tangent frame once placed.
  glm::vec3 alongU{0.0f};
  glm::vec3 upImage{0.0f};
  float area = 0.0f; // of the surface it stands for
};

/**
 * Spread `count` samples over the triangles `indices` index (a mesh
 * section's), evenly by area, each standing for an equal share of it. The
 * same mesh always gives the same samples. None if the triangles have no
 * area.
 */
std::vector<SurfaceSample> sampleSurface(std::span<const MeshVertex> vertices,
                                         std::span<const uint32_t> indices,
                                         std::size_t count);

/** How many samples a section of this much area gets (more for more). */
std::size_t surfaceSampleCount(float area);

/**
 * Whether a material can give off light: a node graph's whose Emission is
 * anything but black (anything linked to it might glow). The standard surface
 * never does.
 */
bool canGlow(const MeshMaterial& material);

/** A placed sample's glow: where, how much surface, and the light it gives. */
struct GlowPoint
{
  glm::vec3 position{0.0f}; // world space
  float area = 0.0f;        // world units squared
  glm::vec3 emission{0.0f}; // the surface's emitted light there (linear RGB)
};

/**
 * The point lights standing in for one object's glow, appended to `out`.
 *
 * A small object gets one light; a big one (a lava lake, a long neon sign) is
 * split into cells, each lit by its own, so its light comes from all of it.
 * Each light sits where its cell's glow is (weighted by brightness), in the
 * glow's colour. Its intensity is the glow's brightness (the brightest
 * channel, averaged over the glowing surface) x `strength`; its radius is
 * where light from that much glow (brightness x area) would fade to almost
 * nothing, x `range`. Nothing for a cell that doesn't glow. Each casts
 * shadows, except from its own glowing surface (its shadowNear holds it), and
 * is as big as the glow (sourceRadius), so shiny things reflect all of it.
 */
void emissionLights(std::span<const GlowPoint> points,
                    float strength,
                    float range,
                    std::vector<PointLight>& out);

} // namespace sfs
