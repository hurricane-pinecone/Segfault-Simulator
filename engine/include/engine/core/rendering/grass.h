#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "nlohmann/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs
{

// Grass: the ground it grows on (GrassGround), the maps that say how it
// grows there (GrassMaps), and where it's flattened (GrassTrample). The GPU
// grows the blades themselves each frame, from these (see GrassModule and
// meshGrassGrow.wgsl): none are stored.

/** About how wide a grass grid's texel is, in world units. */
inline constexpr float kGrassTexel = 0.25f;

/** Most texels a grass grid has a side. */
inline constexpr int kGrassMaxTexels = 1024;

/** Least upward a surface faces to grow grass (cos 60 degrees). */
inline constexpr float kGrassMinUpness = 0.5f;

/**
 * A grid over a rectangle of a mesh's own XY: `size` texels from `min` to
 * `max`, rows from min y. Grass's ground and maps are grids over the mesh, so
 * they stay with it however the entity is moved or scaled.
 */
struct GrassGrid
{
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  glm::ivec2 size{0, 0};

  /**
   * A grid over `min`..`max` with square texels about `texel` across (at
   * most `maxTexels` a side, so coarser for big rectangles).
   */
  static GrassGrid over(glm::vec2 min,
                        glm::vec2 max,
                        float texel,
                        int maxTexels = kGrassMaxTexels);

  bool empty() const { return size.x <= 0 || size.y <= 0; }
  std::size_t count() const
  {
    return empty() ? 0 : std::size_t(size.x) * size.y;
  }
  glm::vec2 texelSize() const { return (max - min) / glm::vec2(size); }
  /** The centre of texel (x, y). */
  glm::vec2 center(int x, int y) const
  {
    return min + (glm::vec2(x, y) + 0.5f) * texelSize();
  }
  /** Where a point is, in texels (texel (x, y)'s centre at x + 0.5, y + 0.5).
   */
  glm::vec2 toTexels(glm::vec2 point) const
  {
    return (point - min) / texelSize();
  }

  bool operator==(const GrassGrid&) const = default;
};

/**
 * The ground grass grows on: a mesh seen from above, as a grid. Each texel
 * has the height (the mesh's own Z) and normal of the highest surface there
 * that faces up enough (when placed by the entity's transform), or nothing.
 */
class GrassGround
{
public:
  GrassGround() = default;

  /**
   * The ground of a mesh's triangles (`positions` indexed by `indices`, its
   * own space), placed by `model`, on a grid over its XY with texels about
   * kGrassTexel world units across.
   */
  static GrassGround build(std::span<const glm::vec3> positions,
                           std::span<const uint32_t> indices,
                           const glm::mat4& model);

  const GrassGrid& grid() const { return m_grid; }
  bool empty() const { return m_texels.empty(); }

  /** Whether texel (x, y) has ground. */
  bool has(int x, int y) const
  {
    return texel(x, y).x != 0.0f || texel(x, y).y != 0.0f ||
           texel(x, y).z != 0.0f;
  }
  /** Texel (x, y): its normal (the mesh's own space, unit) and height; zero
   * normal if none. */
  const glm::vec4& texel(int x, int y) const
  {
    return m_texels[std::size_t(y) * m_grid.size.x + x];
  }
  /** Every texel, rows from min y: as the GPU reads them. */
  std::span<const glm::vec4> texels() const { return m_texels; }

  /** How many texels have ground. */
  std::size_t groundTexels() const;

private:
  GrassGrid m_grid;
  std::vector<glm::vec4> m_texels; // xyz normal, w height
};

/**
 * How grass grows over its ground, as grids the game and the editor's brush
 * write: how much grows (Coverage), how long it is (Length), and four Data
 * channels the engine gives no meaning to, for a grass material to read
 * (moisture, say, for a game to colour dry grass yellow).
 *
 * Values are 0 to 1. Unpainted maps grow full, full-length grass (coverage
 * and length 1, data 0).
 */
class GrassMaps
{
public:
  enum Channel : uint8_t
  {
    Coverage,
    Length,
    Data0,
    Data1,
    Data2,
    Data3,
  };
  static constexpr int kChannels = 6;

  /** How stamp() changes values under it. */
  enum class Blend : uint8_t
  {
    Toward, // move toward the value, by the brush's weight
    Add,    // add the value (negative to take away), times the weight
    Max,    // raise to the value (weighted), never lower
    Min,    // lower to the value (weighted), never raise
  };

  /** The default value of a channel. */
  static float defaultValue(Channel channel)
  {
    return channel <= Length ? 1.0f : 0.0f;
  }
  /** A channel's name as saved ("coverage", "length", "data0"...). */
  static std::string_view name(Channel channel);

  GrassMaps() = default;
  /** Maps over `grid`, every channel at its default. */
  explicit GrassMaps(const GrassGrid& grid);

  const GrassGrid& grid() const { return m_grid; }
  bool empty() const { return m_grid.empty(); }

  /** A channel at a point (the mesh's own XY), bilinear between texels; its
   * default off the grid. */
  float sample(Channel channel, glm::vec2 point) const;
  float at(Channel channel, int x, int y) const
  {
    return m_values[channel][index(x, y)] / 255.0f;
  }
  void set(Channel channel, int x, int y, float value);

  /**
   * Brush `value` into a channel over a circle (the mesh's own XY), `radius`
   * across: full `strength` (0 to 1) over the inner part, easing to nothing at
   * the edge (`softness` of the radius: 0 a hard edge, 1 soft all the way in).
   */
  void stamp(Channel channel,
             glm::vec2 center,
             float radius,
             float value,
             Blend blend = Blend::Toward,
             float strength = 1.0f,
             float softness = 0.5f);

  /** Every texel of a channel to `value`. */
  void fill(Channel channel, float value);

  /**
   * The texels changed since the last call, as a rectangle (min inclusive, max
   * exclusive; empty if none), then forgets them.
   */
  std::pair<glm::ivec2, glm::ivec2> takeChanged();
  /** Mark everything changed (e.g. for a fresh upload). */
  void markAllChanged();

  /**
   * The GPU's two RGBA8 images of a rectangle of texels (rows from its min y):
   * coverage, length, 0, 0; then data 0 to 3.
   */
  void pixels(glm::ivec2 from,
              glm::ivec2 to,
              std::vector<uint8_t>& shape,
              std::vector<uint8_t>& data) const;

  /**
   * As saved: {"grid": {"min", "max", "size"}, "coverage": ..., "length": ...,
   * ...}, each channel as base64 run-length pairs (count, value), only those
   * not all at their default. Maps with no grid save as {}.
   */
  nlohmann::json toJson() const;
  /** Maps read from toJson()'s form; none (empty) if it isn't one. */
  static GrassMaps fromJson(const nlohmann::json& json);

  bool operator==(const GrassMaps& other) const
  {
    return m_grid == other.m_grid && m_values == other.m_values;
  }

private:
  std::size_t index(int x, int y) const
  {
    return std::size_t(y) * m_grid.size.x + x;
  }
  void touch(int x, int y);

  GrassGrid m_grid;
  std::array<std::vector<uint8_t>, kChannels> m_values;
  glm::ivec2 m_changedMin{0, 0};
  glm::ivec2 m_changedMax{0, 0};
};

/** What blades grow like (GrassComponent's settings for them). */
struct GrassShape
{
  float density =
      400.0f;           // blades per square world unit, where coverage is full
  float height = 0.35f; // full length, where the Length map is 1
  float heightVariation = 0.4f; // how much lengths vary (0: all the same)
  float width = 0.02f;          // at the root
};

/** Something flattening grass: a sphere, and how it's moving. */
struct GrassBender
{
  glm::vec3 center{0.0f};
  float radius = 0.5f;
  glm::vec3 velocity{0.0f}; // world units a second
};

/**
 * How flattened grass is, and which way: a grid over its ground (world XY),
 * pressed where benders reach into the grass and springing back over time.
 * Its image() is what the GPU bends blades by.
 */
class GrassTrample
{
public:
  /** Texels a world unit, and most a side. */
  static constexpr float kTexelsPerUnit = 8.0f;
  static constexpr int kMaxTexels = 512;

  GrassTrample() = default;

  /** A map over `ground` placed by `model`, its blades up to `bladeHeight`
   * tall. */
  GrassTrample(const GrassGround& ground,
               const glm::mat4& model,
               float bladeHeight);

  /**
   * Spring back (fully over `recovery` seconds), then press down where
   * `benders` reach into the grass: grass under one lies flat, softening to
   * its edge. A moving one lays it down the way it's going (fanning out a
   * little at its sides), all along its path; a still one presses it outward.
   *
   * @return whether the map changed (it needs uploading again)
   */
  bool
  update(float seconds, std::span<const GrassBender> benders, float recovery);

  /** How flattened the grass at a world XY point is, 0 to 1. */
  float flattened(glm::vec2 point) const;
  /** Which way it's pressed there (unit, or zero). */
  glm::vec2 direction(glm::vec2 point) const;

  /**
   * RGBA8, rows from the map's min y: rg the direction (x 0.5 + 0.5), b how
   * flattened, a 255.
   */
  std::vector<uint8_t> image() const;

  glm::ivec2 size() const { return m_size; }
  glm::vec2 min() const { return m_min; }
  glm::vec2 max() const { return m_max; }

private:
  int indexOf(glm::vec2 point) const; // -1 off the map

  glm::vec2 m_min{0.0f};
  glm::vec2 m_max{0.0f};
  glm::ivec2 m_size{0, 0};
  float m_bladeHeight = 0.0f;
  std::vector<float> m_ground; // per texel: the ground's world height, or none
  std::vector<float> m_amount;
  std::vector<glm::vec2> m_direction;
};

} // namespace sfs
