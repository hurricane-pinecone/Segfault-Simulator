#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace sfs
{

class GroundSurface;
struct TerrainMaterial;
struct TextureData;

/**
 * Where rain water collects on a terrain (TerrainMaterial::puddles), worked
 * out on the GPU in a window of the world's XY around what the camera looks
 * at (see PuddleModule, which places it):
 *
 * 1. The ground's height at each texel: the mesh's surface (laid onto a
 *    coarser grid on the CPU, puddleGround(), once each time the window
 *    moves), less the terrain's micro relief (shallow dips on flat ground)
 *    where its layers' `puddles` let it, plus how far the ground's pressed
 *    in there (its deformation's window, the lowest under each texel, so a
 *    rut narrower than a texel still holds water).
 * 2. Its hollows are found by a morphological closing: the height's highest
 *    within kPuddleRadius (dilated), then that's lowest within it again
 *    (eroded) is the brim water would fill each hollow narrower than that
 *    to. Its lowest within kPuddleRadius is the hollow's bottom.
 * 3. Each holds up to `puddleDepth` above its bottom (or to its brim, if
 *    shallower), less where the brim slopes (water runs off), none in a
 *    hollow shallower than a few millimetres.
 *
 * What the shader reads is a texture of it (the puddle map, RGBA8): red how
 * far up its hollow's water line a texel is (0 its bottom, 254/255 the line,
 * 1 where water never stands), green the hollow's whole depth of water over
 * kPuddleDepthRange. As the weather's puddles go from 0 to 1 the water
 * spreads out from each hollow's bottom to its line.
 *
 * It's worked out again when the window moves, the ground's pressed in (at
 * most a couple of times a second), its deformation's window moves, or the
 * terrain's puddle settings change: not every frame. Beyond the window
 * water stands nowhere (the ground's only wet).
 */

/** The deepest water a puddle map can say stands at its line (metres). */
inline constexpr float kPuddleDepthRange = 0.25f;
/** A puddle map's texel (world units), and its texels across. */
inline constexpr float kPuddleTexel = 0.125f;
inline constexpr int kPuddleTexels = 1024;
inline constexpr float kPuddleWindow = kPuddleTexel * kPuddleTexels;
/** The ground under it, as the GPU reads it: a texel, and texels across. */
inline constexpr float kPuddleGroundTexel = 0.5f;
inline constexpr int kPuddleGroundTexels =
    static_cast<int>(kPuddleWindow / kPuddleGroundTexel);
/** The widest a hollow is looked for (metres either side of a texel). */
inline constexpr float kPuddleRadius = 2.5f;
/** How far ahead of the camera the window's centre goes, at most. */
inline constexpr float kPuddleReach = 40.0f;
/** How far the window's wanted centre wanders before it moves. */
inline constexpr float kPuddleMove = kPuddleWindow / 8.0f;

/**
 * Where a puddle window's centre wants to be: where the view meets the
 * ground (at `ground`, world Z), at most kPuddleReach out from the eye (its
 * XY), so what's looked at and the ground between it and the camera are in
 * it.
 */
glm::vec2 puddleWindowCenter(glm::vec3 eye, glm::vec3 view, float ground);

/**
 * The corner (world XY) of a window centred near `center`, on whole ground
 * texels, so its ground and its puddle map's texels line up with the next's.
 */
glm::vec2 puddleWindowOrigin(glm::vec2 center);

/**
 * Whether a window with its corner at `origin` should move for a wanted
 * centre `center`: once it's more than kPuddleMove off.
 */
bool puddleWindowMoves(glm::vec2 origin, glm::vec2 center);

/**
 * A mesh's surface (`surface`, as placed) under a window from `origin`:
 * kPuddleGroundTexels square, kPuddleGroundTexel across, row-major from the
 * lowest y; each texel (height, u, v, pooling), pooling how readily water
 * pools there (`terrain`'s layers' `puddles` at its UV, 0 to 1), -1 off the
 * mesh.
 */
std::vector<glm::vec4> puddleGround(const GroundSurface& surface,
                                    const TerrainMaterial& terrain,
                                    glm::vec2 origin);

/**
 * A terrain material's puddles, as the renderer works them out
 * (IRenderer::setTerrainPuddles()): its window, the ground under it
 * (puddleGround()), its puddle settings, how fast they fill, and a count the
 * module bumps to have them found again (the ground's been pressed in).
 *
 * `fillRate` (PuddleModule::setFillRate()) is over the mesh's UVs, read as
 * data: its red how fast water rises in the puddles there, 0 (none stands:
 * loose soil soaks it all up) to 1 (as fast as the weather's puddles; the
 * default). It scales how far up its hollow the water line has risen, so
 * where it's low, water only stands in the deepest part of a hollow, if at
 * all, until the weather's puddles are nearly full. The puddle map keeps it
 * in its blue for the terrain.
 */
struct TerrainPuddles
{
  // The window: its corner (world XY), a texel, texels across.
  glm::vec2 origin{0.0f};
  float texel = kPuddleTexel;
  int size = 0;
  std::shared_ptr<const std::vector<glm::vec4>> ground;
  float groundTexel = kPuddleGroundTexel;
  int groundSize = 0;
  float depth = 0.05f;      // the most a hollow holds (puddleDepth)
  float microRelief = 0.0f; // metres deep
  float microReliefSize = 3.0f;
  float radius = kPuddleRadius;
  std::shared_ptr<const TextureData> fillRate; // null: 1 everywhere
  uint32_t version = 0;

  bool empty() const { return size <= 0 || !ground; }
  glm::vec2 extent() const { return glm::vec2(texel * size); }

  bool operator==(const TerrainPuddles&) const = default;
};

} // namespace sfs
