#include "engine/core/rendering/grass.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace sfs
{

namespace
{

// --- small helpers -----------------------------------------------------------

float smoothstep(float edge0, float edge1, float x)
{
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

uint8_t toByte(float value)
{
  return static_cast<uint8_t>(
      std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::vector<uint8_t>& bytes)
{
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3)
  {
    const uint32_t a = bytes[i];
    const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out += kBase64[(triple >> 18) & 63];
    out += kBase64[(triple >> 12) & 63];
    out += i + 1 < bytes.size() ? kBase64[(triple >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? kBase64[triple & 63] : '=';
  }
  return out;
}

std::optional<std::vector<uint8_t>> unbase64(const std::string& text)
{
  if (text.size() % 4 != 0)
    return std::nullopt;
  std::array<int, 256> value;
  value.fill(-1);
  for (int i = 0; i < 64; ++i)
    value[static_cast<unsigned char>(kBase64[i])] = i;
  std::vector<uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4)
  {
    uint32_t triple = 0;
    int padding = 0;
    for (int k = 0; k < 4; ++k)
    {
      const char c = text[i + k];
      int v = 0;
      if (c == '=' && k >= 2 && i + 4 == text.size())
        ++padding;
      else if ((v = value[static_cast<unsigned char>(c)]) < 0 || padding > 0)
        return std::nullopt;
      triple = (triple << 6) | static_cast<uint32_t>(v);
    }
    out.push_back(static_cast<uint8_t>(triple >> 16));
    if (padding < 2)
      out.push_back(static_cast<uint8_t>(triple >> 8));
    if (padding < 1)
      out.push_back(static_cast<uint8_t>(triple));
  }
  return out;
}

// Run-length pairs (count, value), base64: painted maps are mostly long runs.
std::string encodeRuns(const std::vector<uint8_t>& values)
{
  std::vector<uint8_t> runs;
  for (std::size_t i = 0; i < values.size();)
  {
    const uint8_t value = values[i];
    std::size_t count = 1;
    while (i + count < values.size() && values[i + count] == value &&
           count < 255)
      ++count;
    runs.push_back(static_cast<uint8_t>(count));
    runs.push_back(value);
    i += count;
  }
  return base64(runs);
}

std::optional<std::vector<uint8_t>> decodeRuns(const nlohmann::json& text,
                                               std::size_t count)
{
  if (!text.is_string())
    return std::nullopt;
  const std::optional<std::vector<uint8_t>> runs =
      unbase64(text.get<std::string>());
  if (!runs || runs->size() % 2 != 0)
    return std::nullopt;
  std::vector<uint8_t> values;
  values.reserve(count);
  for (std::size_t i = 0; i < runs->size(); i += 2)
  {
    if (values.size() + (*runs)[i] > count)
      return std::nullopt;
    values.insert(values.end(), (*runs)[i], (*runs)[i + 1]);
  }
  if (values.size() != count)
    return std::nullopt;
  return values;
}

// A point's barycentric weights in a 2D triangle; nullopt if degenerate.
std::optional<glm::vec3>
barycentric(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c)
{
  const glm::vec2 v0 = b - a, v1 = c - a, v2 = p - a;
  const float denominator = v0.x * v1.y - v1.x * v0.y;
  if (std::abs(denominator) < 1e-12f)
    return std::nullopt;
  const float u = (v2.x * v1.y - v1.x * v2.y) / denominator;
  const float v = (v0.x * v2.y - v2.x * v0.y) / denominator;
  return glm::vec3(1.0f - u - v, u, v);
}

} // namespace

// --- GrassGrid ---------------------------------------------------------------

GrassGrid
GrassGrid::over(glm::vec2 min, glm::vec2 max, float texel, int maxTexels)
{
  GrassGrid grid;
  const glm::vec2 extent = glm::max(max - min, glm::vec2(1e-3f));
  const float side = std::max(std::max(texel, 1e-4f),
                              std::max(extent.x, extent.y) /
                                  static_cast<float>(std::max(maxTexels, 1)));
  grid.min = min;
  grid.size = glm::clamp(glm::ivec2(glm::ceil(extent / side - 1e-4f)),
                         glm::ivec2(1),
                         glm::ivec2(std::max(maxTexels, 1)));
  grid.max = min + glm::vec2(grid.size) * side;
  return grid;
}

GrassGrid grassWindow(glm::vec2 min,
                      glm::vec2 max,
                      glm::vec2 center,
                      float half,
                      float texel)
{
  GrassGrid grid;
  texel = std::max(texel, 1e-4f);
  const glm::vec2 low = glm::max(center - half, min);
  const glm::vec2 high = glm::min(center + half, max);
  if (!(high.x > low.x) || !(high.y > low.y))
    return grid;
  // Coarser, by twos, until it fits; on the lattice of multiples of `texel`
  // either way.
  float side = texel;
  glm::ivec2 first, last;
  for (;;)
  {
    first = glm::ivec2(glm::floor(low / side));
    last = glm::ivec2(glm::ceil(high / side));
    if (std::max(last.x - first.x, last.y - first.y) <= kGrassMaxTexels)
      break;
    side *= 2.0f;
  }
  grid.min = glm::vec2(first) * side;
  grid.size = glm::max(last - first, glm::ivec2(1));
  grid.max = grid.min + glm::vec2(grid.size) * side;
  return grid;
}

// --- GrassGround -------------------------------------------------------------

GrassGround GrassGround::build(std::span<const glm::vec3> positions,
                               std::span<const uint32_t> indices,
                               const glm::mat4& model,
                               std::span<const glm::vec2> uvs,
                               const GrassGrid* window)
{
  const bool withUvs = !uvs.empty() && uvs.size() == positions.size();
  GrassGround ground;

  // The triangles grass grows on: facing up enough once placed.
  struct Triangle
  {
    glm::vec3 local[3];
    glm::vec3 normal; // its own space's, turned as the placed one faces
    glm::vec2 uv[3]{};
  };
  std::vector<Triangle> triangles;
  // A mirroring placement turns faces over: so their own normals too.
  const float facing = glm::determinant(glm::mat3(model)) < 0.0f ? -1.0f : 1.0f;
  glm::vec2 low(std::numeric_limits<float>::max());
  glm::vec2 high(std::numeric_limits<float>::lowest());
  for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
  {
    if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() ||
        indices[t + 2] >= positions.size())
      continue;
    Triangle triangle;
    glm::vec3 world[3];
    for (int k = 0; k < 3; ++k)
    {
      triangle.local[k] = positions[indices[t + k]];
      if (withUvs)
        triangle.uv[k] = uvs[indices[t + k]];
      world[k] = glm::vec3(model * glm::vec4(triangle.local[k], 1.0f));
    }
    const glm::vec3 up = glm::cross(world[1] - world[0], world[2] - world[0]);
    const float length = glm::length(up);
    if (length <= 1e-12f || up.z / length < kGrassMinUpness)
      continue;
    const glm::vec3 own = glm::cross(triangle.local[1] - triangle.local[0],
                                     triangle.local[2] - triangle.local[0]);
    if (glm::length(own) <= 1e-12f)
      continue;
    triangle.normal = glm::normalize(own) * facing;
    glm::vec2 from(triangle.local[0]), to(triangle.local[0]);
    for (const glm::vec3& corner : triangle.local)
    {
      from = glm::min(from, glm::vec2(corner));
      to = glm::max(to, glm::vec2(corner));
    }
    // Off the window: none of its ground.
    if (window && (to.x < window->min.x || to.y < window->min.y ||
                   from.x > window->max.x || from.y > window->max.y))
      continue;
    low = glm::min(low, from);
    high = glm::max(high, to);
    triangles.push_back(triangle);
  }
  if (triangles.empty() || (window && window->empty()))
    return ground;

  // Texels about kGrassTexel across in the world, by the placement's
  // sideways scale (or the window's).
  const float scale = std::max({glm::length(glm::vec3(model[0])),
                                glm::length(glm::vec3(model[1])),
                                1e-6f});
  ground.m_grid =
      window ? *window : GrassGrid::over(low, high, kGrassTexel / scale);
  const GrassGrid& grid = ground.m_grid;
  ground.m_texels.assign(grid.count(), glm::vec4(0.0f));
  if (withUvs)
    ground.m_uvs.assign(grid.count(), glm::vec2(0.0f));
  std::vector<float> heights(
      grid.count(), std::numeric_limits<float>::lowest());

  const auto put =
      [&](int x, int y, float height, const glm::vec3& normal, glm::vec2 uv)
  {
    const std::size_t i = std::size_t(y) * grid.size.x + x;
    if (height <= heights[i])
      return;
    heights[i] = height;
    ground.m_texels[i] = glm::vec4(normal, height);
    if (withUvs)
      ground.m_uvs[i] = uv;
  };

  // Each triangle onto the texels whose centres it covers (from above), the
  // highest winning; one too small to cover any centre still marks its own.
  const glm::vec2 texel = grid.texelSize();
  for (const Triangle& triangle : triangles)
  {
    const glm::vec2 a(triangle.local[0]), b(triangle.local[1]),
        c(triangle.local[2]);
    const glm::ivec2 from = glm::clamp(
        glm::ivec2(glm::floor((glm::min(glm::min(a, b), c) - grid.min) / texel -
                              0.5f)),
        glm::ivec2(0),
        grid.size - 1);
    const glm::ivec2 to = glm::clamp(
        glm::ivec2(
            glm::ceil((glm::max(glm::max(a, b), c) - grid.min) / texel - 0.5f)),
        glm::ivec2(0),
        grid.size - 1);
    bool covered = false;
    for (int y = from.y; y <= to.y; ++y)
      for (int x = from.x; x <= to.x; ++x)
      {
        const std::optional<glm::vec3> w =
            barycentric(grid.center(x, y), a, b, c);
        if (!w || w->x < -1e-4f || w->y < -1e-4f || w->z < -1e-4f)
          continue;
        covered = true;
        put(x,
            y,
            w->x * triangle.local[0].z + w->y * triangle.local[1].z +
                w->z * triangle.local[2].z,
            triangle.normal,
            w->x * triangle.uv[0] + w->y * triangle.uv[1] +
                w->z * triangle.uv[2]);
      }
    if (!covered)
    {
      const glm::vec3 middle =
          (triangle.local[0] + triangle.local[1] + triangle.local[2]) / 3.0f;
      const glm::ivec2 at =
          glm::clamp(glm::ivec2(glm::floor(grid.toTexels(glm::vec2(middle)))),
                     glm::ivec2(0),
                     grid.size - 1);
      put(at.x,
          at.y,
          middle.z,
          triangle.normal,
          (triangle.uv[0] + triangle.uv[1] + triangle.uv[2]) / 3.0f);
    }
  }

  ground.m_heights = GrassHeights(grid, ground.m_texels);
  return ground;
}

GrassHeights::GrassHeights(const GrassGrid& grid,
                           std::span<const glm::vec4> texels)
    : m_grid(grid), m_blocks((grid.size + kBlock - 1) / kBlock)
{
  m_ranges.assign(grid.count() > 0 ? std::size_t(m_blocks.x) * m_blocks.y : 0,
                  glm::vec2(std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::lowest()));
  if (texels.size() < grid.count())
    return;
  for (int y = 0; y < grid.size.y; ++y)
    for (int x = 0; x < grid.size.x; ++x)
    {
      const glm::vec4& texel = texels[std::size_t(y) * grid.size.x + x];
      if (texel.x == 0.0f && texel.y == 0.0f && texel.z == 0.0f)
        continue;
      glm::vec2& range =
          m_ranges[std::size_t(y / kBlock) * m_blocks.x + x / kBlock];
      range = glm::vec2(std::min(range.x, texel.w), std::max(range.y, texel.w));
    }
}

glm::vec2 GrassHeights::range(glm::ivec2 from, glm::ivec2 to) const
{
  glm::vec2 range(
      std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest());
  from = glm::clamp(from, glm::ivec2(0), m_grid.size);
  to = glm::clamp(to, from, m_grid.size);
  if (to.x <= from.x || to.y <= from.y || m_ranges.empty())
    return range;
  const glm::ivec2 first = from / kBlock;
  const glm::ivec2 last = (to - 1) / kBlock;
  for (int y = first.y; y <= last.y; ++y)
    for (int x = first.x; x <= last.x; ++x)
    {
      const glm::vec2& block = m_ranges[std::size_t(y) * m_blocks.x + x];
      range = glm::vec2(std::min(range.x, block.x), std::max(range.y, block.y));
    }
  return range;
}

std::size_t GrassGround::groundTexels() const
{
  std::size_t count = 0;
  for (int y = 0; y < m_grid.size.y; ++y)
    for (int x = 0; x < m_grid.size.x; ++x)
      count += has(x, y);
  return count;
}

// --- GrassMaps ---------------------------------------------------------------

namespace
{

// Up toward a target at one rate, down at another, over `days`.
float toward(float now, float target, float up, float down, float days)
{
  return now < target ? std::min(now + up * days, target)
                      : std::max(now - down * days, target);
}

// Days it takes to get from `from` to `to` that way (very long if it can't).
float daysToward(float from, float to, float up, float down)
{
  const float change = to - from;
  if (std::abs(change) < 0.5f / 255.0f)
    return 0.0f;
  const float rate = change > 0.0f ? up : down;
  return rate > 1e-6f ? std::abs(change) / rate : 1.0e6f;
}

// Whether tick `a` comes after `b` (the clock wraps).
bool after(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

// The heap of tiles due (GrassMaps::m_due): soonest on top.
bool laterDue(const std::pair<uint32_t, int>& a,
              const std::pair<uint32_t, int>& b)
{
  return after(a.first, b.first);
}

// The most ticks growing is kept apart by (beyond it, a texel waits).
constexpr uint32_t kLongestGrowth = 0x3fffffffu;

} // namespace

std::string_view GrassMaps::name(Channel channel)
{
  static constexpr std::array<std::string_view, kChannels> kNames = {
      "coverage", "length", "data0", "data1", "data2", "data3", "bounds"};
  return kNames[channel];
}

GrassMaps::GrassMaps(const GrassGrid& grid) : m_grid(grid)
{
  for (int c = 0; c < kChannels; ++c)
    m_values[c] = GrassTiles<uint8_t>(
        m_grid.size, toByte(defaultValue(static_cast<Channel>(c))));
  m_anchors = GrassTiles<GrassAnchor>(m_grid.size, GrassAnchor{});
  m_growthScale = GrassTiles<uint8_t>(m_grid.size, 128);
  m_cut = GrassTiles<uint8_t>(m_grid.size, 0);
  m_lean = GrassTiles<uint32_t>(m_grid.size, 0u);
  m_band = GrassTiles<uint8_t>(m_grid.size, 255);
  const std::size_t tiles = m_anchors.tileCount();
  m_growingTile.assign(tiles, 0);
  m_settleAt.assign(tiles, 0);
  m_fastest.assign(tiles, 0.0f);
  m_dueAt.assign(tiles, GrassAnchor::kSettled);
  m_active.assign(tiles, 0);
  m_tileNow.assign(tiles, 0);
  markAllChanged();
}

float GrassMaps::sample(Channel channel, glm::vec2 point) const
{
  if (empty() || point.x < m_grid.min.x || point.y < m_grid.min.y ||
      point.x > m_grid.max.x || point.y > m_grid.max.y)
    return defaultValue(channel);
  // Bilinear between texel centres, held at the edge texels.
  const glm::vec2 at = m_grid.toTexels(point) - 0.5f;
  const glm::ivec2 lowTexel =
      glm::clamp(glm::ivec2(glm::floor(at)), glm::ivec2(0), m_grid.size - 1);
  const glm::ivec2 highTexel = glm::min(lowTexel + 1, m_grid.size - 1);
  const glm::vec2 t =
      glm::clamp(at - glm::vec2(lowTexel), glm::vec2(0.0f), glm::vec2(1.0f));
  const float a = glm::mix(this->at(channel, lowTexel.x, lowTexel.y),
                           this->at(channel, highTexel.x, lowTexel.y),
                           t.x);
  const float b = glm::mix(this->at(channel, lowTexel.x, highTexel.y),
                           this->at(channel, highTexel.x, highTexel.y),
                           t.x);
  return glm::mix(a, b, t.y);
}

float GrassMaps::sampleNow(Channel channel, glm::vec2 point) const
{
  if (channel != Coverage && channel != Length)
    return sample(channel, point);
  if (empty() || point.x < m_grid.min.x || point.y < m_grid.min.y ||
      point.x > m_grid.max.x || point.y > m_grid.max.y)
    return defaultValue(channel);
  const glm::vec2 at = m_grid.toTexels(point) - 0.5f;
  const glm::ivec2 low =
      glm::clamp(glm::ivec2(glm::floor(at)), glm::ivec2(0), m_grid.size - 1);
  const glm::ivec2 high = glm::min(low + 1, m_grid.size - 1);
  const glm::vec2 t =
      glm::clamp(at - glm::vec2(low), glm::vec2(0.0f), glm::vec2(1.0f));
  const float a =
      glm::mix(now(channel, low.x, low.y), now(channel, high.x, low.y), t.x);
  const float b =
      glm::mix(now(channel, low.x, high.y), now(channel, high.x, high.y), t.x);
  return glm::mix(a, b, t.y);
}

float GrassMaps::now(Channel channel, int x, int y) const
{
  if (channel != Coverage && channel != Length)
    return at(channel, x, y);
  const Grown grown = grownAt(x, y);
  return channel == Coverage ? grown.cover : std::min(grown.length, grown.flat);
}

GrassMaps::Grown GrassMaps::grownAt(int x, int y) const
{
  const GrassAnchor anchor = m_anchors.at(x, y);
  const float cover = at(Coverage, x, y);
  const float length = at(Length, x, y);
  if (anchor.settled())
    return {cover, length, 2.0f};
  const uint32_t ticks = m_now - anchor.time;
  // (Before it was changed: the clock set back. As it was then.)
  const float days =
      ticks > 0x80000000u ? 0.0f : float(double(ticks) / kTicksPerDay);
  const float scale = growthScale(x, y);
  const GrassRates& coverRates = m_rates[anchor.classes & 15];
  const GrassRates& lengthRates = m_rates[anchor.classes >> 4];
  Grown grown;
  grown.cover = toward(anchor.cover / 255.0f,
                       cover,
                       coverRates.coverUp * scale,
                       coverRates.coverDown,
                       days);
  grown.length = toward(anchor.length / 255.0f,
                        length,
                        lengthRates.lengthUp * scale,
                        lengthRates.lengthDown,
                        days);
  grown.flat =
      anchor.flat == 255 ? 2.0f : anchor.flat / 255.0f + m_standUp * days;
  return grown;
}

uint32_t GrassMaps::ticksToSettle(const GrassAnchor& anchor,
                                  float cover,
                                  float length,
                                  float scale) const
{
  const GrassRates& coverRates = m_rates[anchor.classes & 15];
  const GrassRates& lengthRates = m_rates[anchor.classes >> 4];
  float days = std::max(daysToward(anchor.cover / 255.0f,
                                   cover,
                                   coverRates.coverUp * scale,
                                   coverRates.coverDown),
                        daysToward(anchor.length / 255.0f,
                                   length,
                                   lengthRates.lengthUp * scale,
                                   lengthRates.lengthDown));
  if (anchor.flat != 255)
    days =
        std::max(days,
                 m_standUp > 1e-6f
                     ? std::max(length - anchor.flat / 255.0f, 0.0f) / m_standUp
                     : 1.0e6f);
  const double ticks = std::ceil(double(days) * kTicksPerDay) + 1.0;
  return uint32_t(std::min(ticks, double(kLongestGrowth)));
}

float GrassMaps::fastestRate(const GrassAnchor& anchor, float scale) const
{
  const GrassRates& coverRates = m_rates[anchor.classes & 15];
  const GrassRates& lengthRates = m_rates[anchor.classes >> 4];
  float fastest = std::max({coverRates.coverUp * scale,
                            coverRates.coverDown,
                            lengthRates.lengthUp * scale,
                            lengthRates.lengthDown});
  if (anchor.flat != 255)
    fastest = std::max(fastest, m_standUp);
  return fastest;
}

void GrassMaps::setClock(double growthDays)
{
  const double ticks = std::floor(growthDays * kTicksPerDay);
  uint32_t now = uint32_t(int64_t(ticks) & 0xffffffffll);
  if (now == GrassAnchor::kSettled)
    --now;
  m_now = now;
}

void GrassMaps::setRates(int rateClass, const GrassRates& rates)
{
  if (rateClass >= 0 && rateClass < kRateClasses)
    m_rates[rateClass] = rates;
}

const GrassRates& GrassMaps::rates(int rateClass) const
{
  return m_rates[std::clamp(rateClass, 0, kRateClasses - 1)];
}

void GrassMaps::setGrowthScale(int x, int y, float scale)
{
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  m_growthScale.set(x,
                    y,
                    static_cast<uint8_t>(std::lround(
                        std::clamp(scale, 0.0f, 255.0f / 128.0f) * 128.0f)));
}

template <class Change>
void GrassMaps::reanchor(int x, int y, Change&& change)
{
  const Grown grown = grownAt(x, y);
  GrassAnchor anchor = m_anchors.at(x, y);
  anchor.time = m_now;
  anchor.cover = toByte(grown.cover);
  anchor.length = toByte(grown.length);
  anchor.flat = grown.flat < grown.length ? toByte(grown.flat) : 255;
  change(anchor);
  // Grown to its maps already: nothing to grow.
  if (anchor.cover == m_values[Coverage].at(x, y) &&
      anchor.length == m_values[Length].at(x, y) && anchor.flat == 255)
    anchor.time = GrassAnchor::kSettled;
  putAnchor(x, y, anchor);
}

void GrassMaps::putAnchor(int x, int y, const GrassAnchor& anchor)
{
  const GrassAnchor old = m_anchors.at(x, y);
  if (old == anchor)
    return;
  m_anchors.set(x, y, anchor);
  const int tile = m_anchors.tileOf(x, y);
  markTileNow(tile);
  if (anchor.settled())
    return;
  const float scale = growthScale(x, y);
  const uint32_t settleAt =
      anchor.time +
      ticksToSettle(anchor, at(Coverage, x, y), at(Length, x, y), scale);
  const float fastest = fastestRate(anchor, scale);
  if (!m_growingTile[tile])
  {
    m_growingTile[tile] = 1;
    ++m_growingTiles;
    m_settleAt[tile] = settleAt;
    m_fastest[tile] = fastest;
    growingChanged(tile);
    schedule(tile);
    return;
  }
  if (after(settleAt, m_settleAt[tile]))
    m_settleAt[tile] = settleAt;
  m_fastest[tile] = std::max(m_fastest[tile], fastest);
  if (m_dueAt[tile] == GrassAnchor::kSettled)
    schedule(tile);
}

void GrassMaps::schedule(int tile)
{
  // When it's next changed by a step (of the GPU's: 1/255) at its fastest,
  // or done growing if sooner.
  const float fastest = std::max(m_fastest[tile], 1e-3f);
  const double step = std::clamp(
      std::ceil(kTicksPerDay / (255.0 * double(fastest))), 1.0, kTicksPerDay);
  uint32_t due = m_now + uint32_t(step);
  if (after(due, m_settleAt[tile]))
    due = m_settleAt[tile];
  if (due == GrassAnchor::kSettled)
    --due;
  m_dueAt[tile] = due;
  m_due.emplace_back(due, tile);
  std::push_heap(m_due.begin(), m_due.end(), laterDue);
}

void GrassMaps::snap(int x, int y)
{
  GrassAnchor anchor = m_anchors.at(x, y);
  if (anchor.settled())
    return;
  anchor.time = GrassAnchor::kSettled;
  anchor.cover = anchor.length = 0;
  anchor.flat = 255;
  m_anchors.set(x, y, anchor);
  markTileNow(m_anchors.tileOf(x, y));
}

void GrassMaps::growingChanged(int tile)
{
  // It and those round it: whether any round each is growing.
  const glm::ivec2 tiles = m_anchors.tiles();
  const glm::ivec2 at(tile % tiles.x, tile / tiles.x);
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
      const glm::ivec2 n = at + glm::ivec2(dx, dy);
      if (n.x < 0 || n.y < 0 || n.x >= tiles.x || n.y >= tiles.y)
        continue;
      uint8_t any = 0;
      for (int ey = -1; ey <= 1 && !any; ++ey)
        for (int ex = -1; ex <= 1 && !any; ++ex)
        {
          const glm::ivec2 m = n + glm::ivec2(ex, ey);
          if (m.x >= 0 && m.y >= 0 && m.x < tiles.x && m.y < tiles.y)
            any = m_growingTile[std::size_t(m.y) * tiles.x + m.x];
        }
      uint8_t& active = m_active[std::size_t(n.y) * tiles.x + n.x];
      if (active != any)
      {
        active = any;
        m_activeChanged = true;
      }
    }
}

bool GrassMaps::settledRound(int tile) const
{
  const glm::ivec2 tiles = m_anchors.tiles();
  const glm::ivec2 at(tile % tiles.x, tile / tiles.x);
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
      const glm::ivec2 n = at + glm::ivec2(dx, dy);
      if (n.x >= 0 && n.y >= 0 && n.x < tiles.x && n.y < tiles.y &&
          m_growingTile[std::size_t(n.y) * tiles.x + n.x])
        return false;
    }
  return true;
}

void GrassMaps::markTileNow(int tile)
{
  if (m_tileNow[tile])
    return;
  m_tileNow[tile] = 1;
  m_tilesNow.push_back(tile);
}

void GrassMaps::settleTile(int tile)
{
  // Every texel of it grown: kept as settled (with its classes).
  if (m_anchors.isUniform(tile))
  {
    GrassAnchor anchor = m_anchors.uniform(tile);
    m_anchors.setTile(
        tile, GrassAnchor{GrassAnchor::kSettled, 0, 0, 255, anchor.classes});
  }
  else
  {
    const glm::ivec2 tiles = m_anchors.tiles();
    const glm::ivec2 from =
        glm::ivec2(tile % tiles.x, tile / tiles.x) * kGrassMapTile;
    const glm::ivec2 to = glm::min(from + kGrassMapTile, m_grid.size);
    for (int y = from.y; y < to.y; ++y)
      for (int x = from.x; x < to.x; ++x)
        m_anchors.set(
            x,
            y,
            GrassAnchor{
                GrassAnchor::kSettled, 0, 0, 255, m_anchors.at(x, y).classes});
    m_anchors.compact(tile);
  }
  m_growingTile[tile] = 0;
  --m_growingTiles;
  m_dueAt[tile] = GrassAnchor::kSettled;
  growingChanged(tile);
  markTileNow(tile);
  // Cuts' edges forgotten once all round them has grown back.
  const glm::ivec2 tiles = m_cut.tiles();
  const glm::ivec2 at(tile % tiles.x, tile / tiles.x);
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
      const glm::ivec2 n = at + glm::ivec2(dx, dy);
      if (n.x < 0 || n.y < 0 || n.x >= tiles.x || n.y >= tiles.y)
        continue;
      const int index = n.y * tiles.x + n.x;
      if ((m_cut.isUniform(index) && m_cut.uniform(index) == 0) ||
          m_growingTile[index] || !settledRound(index))
        continue;
      m_cut.setTile(index, 0);
      m_lean.setTile(index, 0u);
      markTileNow(index);
    }
}

void GrassMaps::setTarget(Channel channel, int x, int y, float value)
{
  if (channel != Coverage && channel != Length)
  {
    set(channel, x, y, value);
    return;
  }
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  const uint8_t next = toByte(value);
  if (m_values[channel].at(x, y) == next)
    return;
  // As it is now, by its old targets; then growing on toward the new.
  const Grown grown = grownAt(x, y);
  m_values[channel].set(x, y, next);
  touch(channel, x, y);
  GrassAnchor anchor = m_anchors.at(x, y);
  anchor.time = m_now;
  anchor.cover = toByte(grown.cover);
  anchor.length = toByte(grown.length);
  anchor.flat = grown.flat < grown.length ? toByte(grown.flat) : 255;
  if (anchor.cover == m_values[Coverage].at(x, y) &&
      anchor.length == m_values[Length].at(x, y) && anchor.flat == 255)
    anchor.time = GrassAnchor::kSettled;
  putAnchor(x, y, anchor);
}

void GrassMaps::setClasses(int x, int y, int coverClass, int lengthClass)
{
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  const uint8_t classes =
      uint8_t((coverClass & 15) | ((lengthClass & 15) << 4));
  if (m_anchors.at(x, y).classes == classes)
    return;
  reanchor(x, y, [&](GrassAnchor& anchor) { anchor.classes = classes; });
}

void GrassMaps::tear(int x, int y, int coverClass)
{
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  reanchor(x,
           y,
           [&](GrassAnchor& anchor)
           {
             anchor.cover = 0;
             anchor.classes =
                 uint8_t((anchor.classes & 0xf0) | (coverClass & 15));
           });
}

void GrassMaps::flatten(glm::vec2 center,
                        float radius,
                        float to,
                        float softness)
{
  if (empty() || radius <= 0.0f)
    return;
  const glm::ivec2 from =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(center - radius))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const glm::ivec2 last =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(center + radius))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const float inner = 1.0f - std::clamp(softness, 0.0f, 1.0f);
  for (int y = from.y; y <= last.y; ++y)
    for (int x = from.x; x <= last.x; ++x)
    {
      const float distance = glm::length(m_grid.center(x, y) - center) / radius;
      if (distance >= 1.0f)
        continue;
      const float weight =
          inner >= 1.0f ? 1.0f : 1.0f - smoothstep(inner, 1.0f, distance);
      const float flat = 1.0f + (std::clamp(to, 0.0f, 1.0f) - 1.0f) * weight;
      const Grown grown = grownAt(x, y);
      // Only where it lays it flatter than it is.
      if (toByte(flat) >= toByte(std::min(grown.length, grown.flat)))
        continue;
      reanchor(x, y, [&](GrassAnchor& anchor) { anchor.flat = toByte(flat); });
    }
}

void GrassMaps::cut(const GrassCut& cut)
{
  if (empty())
    return;
  glm::vec2 along = cut.b - cut.a;
  const float length = glm::length(along);
  if (length < 1e-5f || cut.width <= 0.0f)
    return;
  along /= length;
  const glm::vec2 across(-along.y, along.x);
  const glm::vec2 middle = 0.5f * (cut.a + cut.b);
  const glm::vec2 half(0.5f * length, 0.5f * cut.width);
  const glm::vec2 texelSize = m_grid.texelSize();
  const float texel = std::max(texelSize.x, texelSize.y);
  // (The edge's kept over two texels either side, as bounds are.)
  const float ramp = 2.0f * texel;
  glm::vec2 low(1.0e30f), high(-1.0e30f);
  for (const float u : {-1.0f, 1.0f})
    for (const float v : {-1.0f, 1.0f})
    {
      const glm::vec2 corner = middle + along * (u * (half.x + ramp)) +
                               across * (v * (half.y + ramp));
      low = glm::min(low, corner);
      high = glm::max(high, corner);
    }
  const glm::ivec2 from =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(low))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const glm::ivec2 last =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(high))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const uint8_t lengthClass = uint8_t(cut.lengthClass & 15);
  const float leaves = std::clamp(cut.length, 0.0f, 1.0f);
  for (int y = from.y; y <= last.y; ++y)
    for (int x = from.x; x <= last.x; ++x)
    {
      // How far inside the strip its middle is (below 0: outside).
      const glm::vec2 offset = m_grid.center(x, y) - middle;
      const glm::vec2 out(std::abs(glm::dot(offset, along)) - half.x,
                          std::abs(glm::dot(offset, across)) - half.y);
      const float outside = glm::length(glm::max(out, glm::vec2(0.0f))) +
                            std::min(std::max(out.x, out.y), 0.0f);
      const float inside = -outside;
      if (inside < -ramp)
        continue;
      uint8_t edge = toByte(GrassMaps::boundsValue(inside, texel));
      const uint8_t old = m_cut.at(x, y);
      if (cut.join)
        edge = std::max(edge, old);
      if (edge != old)
      {
        m_cut.set(x, y, edge);
        markTileNow(m_cut.tileOf(x, y));
      }
      if (inside < 0.0f)
        continue;
      // (How long it leaves it, for the GPU: the blades cut level.)
      const uint32_t lean = m_lean.at(x, y);
      m_lean.set(x, y, (lean & 0x00ffffffu) | (uint32_t(toByte(leaves)) << 24));
      reanchor(x,
               y,
               [&](GrassAnchor& anchor)
               {
                 const float now =
                     std::min(anchor.length / 255.0f,
                              anchor.flat == 255 ? 2.0f : anchor.flat / 255.0f);
                 anchor.length = toByte(std::min(now, leaves));
                 anchor.flat = 255;
                 anchor.classes =
                     uint8_t((anchor.classes & 0x0f) | (lengthClass << 4));
               });
    }
}

void GrassMaps::roll(const GrassRoll& roll)
{
  if (empty())
    return;
  glm::vec2 along = roll.b - roll.a;
  const float length = glm::length(along);
  if (length < 1e-5f || roll.width <= 0.0f)
    return;
  along /= length;
  const glm::vec2 across(-along.y, along.x);
  const glm::vec2 middle = 0.5f * (roll.a + roll.b);
  const glm::vec2 half(0.5f * length, 0.5f * roll.width);
  const glm::vec2 texelSize = m_grid.texelSize();
  const float texel = std::max(texelSize.x, texelSize.y);
  glm::vec2 low(1.0e30f), high(-1.0e30f);
  for (const float u : {-1.0f, 1.0f})
    for (const float v : {-1.0f, 1.0f})
    {
      const glm::vec2 corner = middle + along * (u * (half.x + texel)) +
                               across * (v * (half.y + texel));
      low = glm::min(low, corner);
      high = glm::max(high, corner);
    }
  const glm::ivec2 from =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(low))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const glm::ivec2 last =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(high))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const glm::vec2 lean = along * std::clamp(roll.strength, 0.0f, 1.0f);
  for (int y = from.y; y <= last.y; ++y)
    for (int x = from.x; x <= last.x; ++x)
    {
      // Only what's growing keeps it (mown grass).
      if (m_anchors.at(x, y).time == GrassAnchor::kSettled)
        continue;
      const glm::vec2 offset = m_grid.center(x, y) - middle;
      const float inside =
          std::min(half.x - std::abs(glm::dot(offset, along)),
                   half.y - std::abs(glm::dot(offset, across)));
      // How much of the texel's under it (its edge blended over a texel).
      const float under = std::clamp(0.5f + inside / texel, 0.0f, 1.0f);
      if (under <= 0.0f)
        continue;
      const uint32_t old = m_lean.at(x, y);
      const glm::vec2 was(float(int8_t(old & 0xffu)) / 127.0f,
                          float(int8_t((old >> 8) & 0xffu)) / 127.0f);
      const glm::vec2 now = glm::mix(was, lean, under);
      const Grown grown = grownAt(x, y);
      const uint32_t laid =
          under >= 0.5f ? uint32_t(toByte(std::min(grown.length, grown.flat)))
                        : (old >> 16) & 0xffu;
      const auto signedByte = [](float value)
      {
        return uint32_t(uint8_t(
            int8_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f))));
      };
      const uint32_t next = signedByte(now.x) | (signedByte(now.y) << 8) |
                            (laid << 16) | (old & 0xff000000u);
      if (next == old)
        continue;
      m_lean.set(x, y, next);
      markTileNow(m_lean.tileOf(x, y));
    }
}

void GrassMaps::settle()
{
  if (empty())
    return;
  for (int tile = 0; tile < int(m_anchors.tileCount()); ++tile)
  {
    if (m_growingTile[tile])
      settleTile(tile);
    else
      m_anchors.compact(tile);
  }
  m_due.clear();
  m_cut.fill(0);
  m_lean.fill(0u);
  markAllChanged();
}

void GrassMaps::update(int mostTiles)
{
  int done = 0;
  while (!m_due.empty() && done < mostTiles)
  {
    const auto [when, tile] = m_due.front();
    if (after(when, m_now))
      break;
    std::pop_heap(m_due.begin(), m_due.end(), laterDue);
    m_due.pop_back();
    if (m_dueAt[tile] != when || !m_growingTile[tile])
      continue; // (put off, or done)
    ++done;
    m_dueAt[tile] = GrassAnchor::kSettled;
    if (!after(m_settleAt[tile], m_now))
      settleTile(tile);
    else
    {
      markTileNow(tile);
      schedule(tile);
    }
  }
}

std::vector<int> GrassMaps::takeTilesNow()
{
  std::vector<int> tiles;
  tiles.swap(m_tilesNow);
  for (const int tile : tiles)
    m_tileNow[tile] = 0;
  return tiles;
}

bool GrassMaps::takeActiveChanged()
{
  const bool changed = m_activeChanged;
  m_activeChanged = false;
  return changed;
}

void GrassMaps::set(Channel channel, int x, int y, float value)
{
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  const uint8_t next = toByte(value);
  if (channel == Coverage || channel == Length)
    snap(x, y);
  if (m_values[channel].at(x, y) == next)
    return;
  m_values[channel].set(x, y, next);
  touch(channel, x, y);
}

void GrassMaps::stamp(Channel channel,
                      glm::vec2 center,
                      float radius,
                      float value,
                      Blend blend,
                      float strength,
                      float softness)
{
  if (empty() || radius <= 0.0f || strength <= 0.0f)
    return;
  const glm::ivec2 from =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(center - radius))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const glm::ivec2 to =
      glm::clamp(glm::ivec2(glm::floor(m_grid.toTexels(center + radius))),
                 glm::ivec2(0),
                 m_grid.size - 1);
  const float inner = 1.0f - std::clamp(softness, 0.0f, 1.0f);
  GrassTiles<uint8_t>& values = m_values[channel];
  for (int y = from.y; y <= to.y; ++y)
    for (int x = from.x; x <= to.x; ++x)
    {
      const float distance = glm::length(m_grid.center(x, y) - center) / radius;
      if (distance >= 1.0f)
        continue;
      const float weight =
          std::min(strength, 1.0f) *
          (inner >= 1.0f ? 1.0f : 1.0f - smoothstep(inner, 1.0f, distance));
      const float old = at(channel, x, y);
      float next = old;
      switch (blend)
      {
      case Blend::Toward:
        next = old + (value - old) * weight;
        break;
      case Blend::Add:
        next = old + value * weight;
        break;
      case Blend::Max:
        next = old + std::max(value - old, 0.0f) * weight;
        break;
      case Blend::Min:
        next = old + std::min(value - old, 0.0f) * weight;
        break;
      }
      next = std::clamp(next, 0.0f, 1.0f);
      uint8_t stored = toByte(next);
      // A gentle brush still gets there: at least a step toward its value.
      const uint8_t current = values.at(x, y);
      if (stored == current && weight > 0.0f && next != old)
        stored = static_cast<uint8_t>(
            std::clamp(int(current) + (next > old ? 1 : -1), 0, 255));
      if (stored != current)
      {
        if (channel == Coverage || channel == Length)
          snap(x, y);
        values.set(x, y, stored);
        touch(channel, x, y);
      }
    }
}

void GrassMaps::fill(Channel channel, float value)
{
  if (empty())
    return;
  m_values[channel].fill(toByte(value));
  m_changedMin = glm::ivec2(0);
  m_changedMax = m_grid.size;
  if (shapesBlades(channel))
  {
    m_shapeChanged = true;
    markAllGrown();
  }
  if (channel == Coverage || channel == Length)
    settle();
}

void GrassMaps::touch(Channel channel, int x, int y)
{
  if (shapesBlades(channel))
    m_shapeChanged = true;
  if (shapesBlades(channel) && !m_allGrown)
  {
    const int blocks = (m_grid.size.x + kGrownBlock - 1) / kGrownBlock;
    if (m_grownBlocks.empty())
      m_grownBlocks.assign(
          std::size_t(blocks) *
              ((m_grid.size.y + kGrownBlock - 1) / kGrownBlock),
          0);
    m_grownBlocks[std::size_t(y / kGrownBlock) * blocks + x / kGrownBlock] = 1;
    m_anyGrown = true;
  }
  if (m_changedMin.x >= m_changedMax.x || m_changedMin.y >= m_changedMax.y)
  {
    m_changedMin = {x, y};
    m_changedMax = {x + 1, y + 1};
    return;
  }
  m_changedMin = glm::min(m_changedMin, glm::ivec2(x, y));
  m_changedMax = glm::max(m_changedMax, glm::ivec2(x + 1, y + 1));
}

std::pair<glm::ivec2, glm::ivec2> GrassMaps::takeChanged()
{
  const std::pair<glm::ivec2, glm::ivec2> changed{m_changedMin, m_changedMax};
  m_changedMin = m_changedMax = glm::ivec2(0);
  return changed;
}

bool GrassMaps::takeShapeChanged()
{
  const bool changed = m_shapeChanged;
  m_shapeChanged = false;
  return changed;
}

void GrassMaps::markAllChanged()
{
  m_changedMin = glm::ivec2(0);
  m_changedMax = m_grid.size;
  m_shapeChanged = !empty();
  m_activeChanged = true;
  markAllGrown();
}

void GrassMaps::markAllGrown() { m_allGrown = m_anyGrown = !empty(); }

std::vector<glm::ivec4> GrassMaps::takeGrownChanged()
{
  std::vector<glm::ivec4> changed;
  if (m_allGrown)
    changed.emplace_back(0, 0, m_grid.size.x, m_grid.size.y);
  else if (m_anyGrown)
  {
    // Each row's changed blocks, those side by side as one.
    const glm::ivec2 blocks = (m_grid.size + kGrownBlock - 1) / kGrownBlock;
    for (int y = 0; y < blocks.y; ++y)
      for (int x = 0; x < blocks.x; ++x)
      {
        if (!m_grownBlocks[std::size_t(y) * blocks.x + x])
          continue;
        int end = x + 1;
        while (end < blocks.x && m_grownBlocks[std::size_t(y) * blocks.x + end])
          ++end;
        changed.emplace_back(x * kGrownBlock,
                             y * kGrownBlock,
                             std::min(end * kGrownBlock, m_grid.size.x),
                             std::min((y + 1) * kGrownBlock, m_grid.size.y));
        x = end;
      }
  }
  m_allGrown = m_anyGrown = false;
  std::fill(m_grownBlocks.begin(), m_grownBlocks.end(), uint8_t(0));
  return changed;
}

void GrassMaps::pixels(glm::ivec2 from,
                       glm::ivec2 to,
                       std::vector<uint8_t>& shape,
                       std::vector<uint8_t>& data) const
{
  from = glm::clamp(from, glm::ivec2(0), m_grid.size);
  to = glm::clamp(to, from, m_grid.size);
  const std::size_t count = std::size_t(to.x - from.x) * (to.y - from.y);
  shape.resize(count * 4);
  data.resize(count * 4);
  std::size_t i = 0;
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x, i += 4)
    {
      shape[i + 0] = m_values[Coverage].at(x, y);
      shape[i + 1] = m_values[Length].at(x, y);
      shape[i + 2] = 255;
      shape[i + 3] = m_values[Bounds].at(x, y);
      for (int k = 0; k < 4; ++k)
        data[i + k] = m_values[Data0 + k].at(x, y);
    }
}

void GrassMaps::keepBand(glm::ivec2 from,
                         glm::ivec2 to,
                         std::span<const uint8_t> shape)
{
  from = glm::clamp(from, glm::ivec2(0), m_grid.size);
  to = glm::clamp(to, from, m_grid.size);
  const int width = to.x - from.x;
  if (shape.size() < std::size_t(width) * (to.y - from.y) * 4)
    return;
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x)
      m_band.set(
          x,
          y,
          shape[(std::size_t(y - from.y) * width + (x - from.x)) * 4 + 2]);
  if (width <= 0 || to.y <= from.y)
    return;
  for (int ty = from.y / kGrassMapTile; ty <= (to.y - 1) / kGrassMapTile; ++ty)
    for (int tx = from.x / kGrassMapTile; tx <= (to.x - 1) / kGrassMapTile;
         ++tx)
      m_band.compact(ty * m_band.tiles().x + tx);
}

void GrassMaps::nowPixels(glm::ivec2 from,
                          glm::ivec2 to,
                          std::vector<uint8_t>& now) const
{
  from = glm::clamp(from, glm::ivec2(0), m_grid.size);
  to = glm::clamp(to, from, m_grid.size);
  now.resize(std::size_t(to.x - from.x) * (to.y - from.y) * 4);
  std::size_t i = 0;
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x, i += 4)
    {
      const Grown grown = grownAt(x, y);
      now[i + 0] = toByte(grown.cover);
      now[i + 1] = toByte(std::min(grown.length, grown.flat));
      now[i + 2] = m_band.at(x, y);
      now[i + 3] = m_values[Bounds].at(x, y);
    }
}

void GrassMaps::cutPixels(glm::ivec2 from,
                          glm::ivec2 to,
                          std::vector<uint8_t>& cut) const
{
  from = glm::clamp(from, glm::ivec2(0), m_grid.size);
  to = glm::clamp(to, from, m_grid.size);
  cut.resize(std::size_t(to.x - from.x) * (to.y - from.y) * 4);
  std::size_t i = 0;
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x, i += 4)
    {
      cut[i] = m_cut.at(x, y);
      const uint32_t lean = m_lean.at(x, y);
      cut[i + 3] = uint8_t(lean >> 24);
      float stands = 0.0f; // how much of its lean's left, as it's grown back
      if (lean & 0xffffu)
      {
        const Grown grown = grownAt(x, y);
        const float grownBack = std::min(grown.length, grown.flat) -
                                float((lean >> 16) & 0xffu) / 255.0f;
        stands = std::clamp(1.0f - grownBack / m_leanFade, 0.0f, 1.0f);
      }
      const auto centred = [stands](uint32_t byte)
      {
        return uint8_t(std::lround(127.5f + float(int8_t(byte & 0xffu)) *
                                                stands * (127.5f / 127.0f)));
      };
      cut[i + 1] = centred(lean);
      cut[i + 2] = centred(lean >> 8);
    }
}

std::size_t GrassMaps::bytes() const
{
  std::size_t total = m_anchors.bytes() + m_growthScale.bytes() +
                      m_cut.bytes() + m_lean.bytes() + m_band.bytes();
  for (const GrassTiles<uint8_t>& values : m_values)
    total += values.bytes();
  return total;
}

nlohmann::json GrassMaps::toJson() const
{
  if (empty())
    return nlohmann::json::object();
  nlohmann::json json = {{"grid",
                          {{"min", {m_grid.min.x, m_grid.min.y}},
                           {"max", {m_grid.max.x, m_grid.max.y}},
                           {"size", {m_grid.size.x, m_grid.size.y}}}}};
  for (int c = 0; c < kChannels; ++c)
  {
    const auto channel = static_cast<Channel>(c);
    const uint8_t standard = toByte(defaultValue(channel));
    std::vector<uint8_t> values(m_grid.count());
    bool any = false;
    for (int y = 0; y < m_grid.size.y; ++y)
      for (int x = 0; x < m_grid.size.x; ++x)
      {
        const uint8_t value = m_values[c].at(x, y);
        values[std::size_t(y) * m_grid.size.x + x] = value;
        any = any || value != standard;
      }
    if (any)
      json[std::string(name(channel))] = encodeRuns(values);
  }
  return json;
}

GrassMaps GrassMaps::fromJson(const nlohmann::json& json)
{
  if (!json.is_object())
    return {};
  try
  {
    // Maps saved before there were channels: a density grid alone, as
    // coverage.
    const bool legacy = json.contains("cells") && !json.contains("grid");
    const nlohmann::json& grid = legacy ? json : json.at("grid");
    GrassGrid read;
    read.min = {grid.at("min")[0].get<float>(), grid.at("min")[1].get<float>()};
    read.max = {grid.at("max")[0].get<float>(), grid.at("max")[1].get<float>()};
    read.size = {grid.at("size")[0].get<int>(), grid.at("size")[1].get<int>()};
    if (read.size.x <= 0 || read.size.y <= 0 || read.size.x > kGrassMaxTexels ||
        read.size.y > kGrassMaxTexels || !(read.max.x > read.min.x) ||
        !(read.max.y > read.min.y))
      return {};

    GrassMaps maps(read);
    for (int c = 0; c < kChannels; ++c)
    {
      const auto channel = static_cast<Channel>(c);
      const std::string key =
          legacy && channel == Coverage ? "cells" : std::string(name(channel));
      if (!json.contains(key))
        continue;
      std::optional<std::vector<uint8_t>> values =
          decodeRuns(json[key], read.count());
      if (!values)
        return {};
      GrassTiles<uint8_t>& tiles = maps.m_values[c];
      for (int y = 0; y < read.size.y; ++y)
        for (int x = 0; x < read.size.x; ++x)
          tiles.set(x, y, (*values)[std::size_t(y) * read.size.x + x]);
      for (int tile = 0; tile < int(tiles.tileCount()); ++tile)
        tiles.compact(tile);
    }
    return maps;
  }
  catch (const nlohmann::json::exception&)
  {
    return {};
  }
}

bool GrassMaps::operator==(const GrassMaps& other) const
{
  if (!(m_grid == other.m_grid))
    return false;
  for (int c = 0; c < kChannels; ++c)
  {
    const GrassTiles<uint8_t>& a = m_values[c];
    const GrassTiles<uint8_t>& b = other.m_values[c];
    for (int tile = 0; tile < int(a.tileCount()); ++tile)
    {
      if (a.isUniform(tile) && b.isUniform(tile))
      {
        if (a.uniform(tile) != b.uniform(tile))
          return false;
        continue;
      }
      const glm::ivec2 from =
          glm::ivec2(tile % a.tiles().x, tile / a.tiles().x) * kGrassMapTile;
      const glm::ivec2 to = glm::min(from + kGrassMapTile, m_grid.size);
      for (int y = from.y; y < to.y; ++y)
        for (int x = from.x; x < to.x; ++x)
          if (a.at(x, y) != b.at(x, y))
            return false;
    }
  }
  return true;
}

// --- vigour ------------------------------------------------------------------

namespace
{

// meshGrassGrow.wgsl's shapeHash() and shapeNoise() (and meshGrass.wgsl's
// groundHash(), groundNoise()), the same sums.
float fract(float value) { return value - std::floor(value); }

float vigourHash(glm::vec2 p)
{
  glm::vec2 q(fract(p.x * 0.1031f), fract(p.y * 0.1030f));
  const float d = glm::dot(q, glm::vec2(q.y, q.x) + 33.33f);
  q += d;
  return fract((q.x + q.y) * q.x);
}

float vigourNoise(glm::vec2 p)
{
  const glm::vec2 cell = glm::floor(p);
  const glm::vec2 f = p - cell;
  const glm::vec2 u = f * f * (3.0f - 2.0f * f);
  const float a = vigourHash(cell);
  const float b = vigourHash(cell + glm::vec2(1.0f, 0.0f));
  const float c = vigourHash(cell + glm::vec2(0.0f, 1.0f));
  const float d = vigourHash(cell + glm::vec2(1.0f, 1.0f));
  return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}

} // namespace

float GrassVigourField::sample(glm::vec2 point, float outside) const
{
  if (empty() || !(max.x > min.x) || !(max.y > min.y))
    return outside;
  const glm::vec2 uv = (point - min) / (max - min);
  const float edge =
      std::min(std::min(uv.x, uv.y), std::min(1.0f - uv.x, 1.0f - uv.y));
  if (edge <= 0.0f)
    return outside;
  const glm::vec2 at = uv * glm::vec2(size) - 0.5f;
  const glm::ivec2 low =
      glm::clamp(glm::ivec2(glm::floor(at)), glm::ivec2(0), size - 1);
  const glm::ivec2 high = glm::min(low + 1, size - 1);
  const glm::vec2 t =
      glm::clamp(at - glm::vec2(low), glm::vec2(0.0f), glm::vec2(1.0f));
  const auto value = [&](glm::ivec2 texel)
  { return values[std::size_t(texel.y) * size.x + texel.x]; };
  const float field =
      glm::mix(glm::mix(value(low), value({high.x, low.y}), t.x),
               glm::mix(value({low.x, high.y}), value(high), t.x),
               t.y);
  return glm::mix(outside, field, smoothstep(0.0f, 0.03f, edge));
}

float grassVigourDetail(glm::vec2 point, float size)
{
  const float across = std::max(size, 1e-3f);
  return vigourNoise(point / across + 17.3f) * 0.65f +
         vigourNoise(point / (across * 0.37f) - 5.1f) * 0.35f;
}

float grassVigour(const GrassVigourField* field,
                  const GrassVigourSettings& settings,
                  glm::vec2 point)
{
  if (!settings.on)
    return 1.0f;
  const float w =
      (field ? field->sample(point, settings.outside) : settings.outside) +
      (grassVigourDetail(point, settings.detailSize) - 0.5f) * settings.detail;
  return smoothstep(0.0f, 1.0f, w * settings.scale + settings.bias);
}

void grassEdgeBand(std::span<uint8_t> shape, glm::ivec2 size)
{
  if (size.x <= 0 || size.y <= 0 ||
      shape.size() < std::size_t(size.x) * size.y * 4)
    return;
  // A distance transform from the coverage, in two sweeps (forward, then
  // back) taking each texel's least from the four already swept round it, a
  // texel straight across 1 / kGrassEdgeBand and one diagonally that times
  // the square root of 2 (in bytes: 1 is 255).
  constexpr int kStraight = (255 + kGrassEdgeBand / 2) / kGrassEdgeBand;
  constexpr int kDiagonal = (361 + kGrassEdgeBand / 2) / kGrassEdgeBand;
  const auto blue = [&](int x, int y) -> uint8_t&
  { return shape[(std::size_t(y) * size.x + x) * 4 + 2]; };
  for (int y = 0; y < size.y; ++y)
    for (int x = 0; x < size.x; ++x)
      blue(x, y) = shape[(std::size_t(y) * size.x + x) * 4];
  const auto sweep = [&](int step)
  {
    const int firstY = step > 0 ? 0 : size.y - 1;
    const int firstX = step > 0 ? 0 : size.x - 1;
    for (int y = firstY; y >= 0 && y < size.y; y += step)
      for (int x = firstX; x >= 0 && x < size.x; x += step)
      {
        int least = blue(x, y);
        const int behindX = x - step, behindY = y - step;
        const bool hasX = behindX >= 0 && behindX < size.x;
        const bool hasY = behindY >= 0 && behindY < size.y;
        if (hasX)
          least = std::min(least, blue(behindX, y) + kStraight);
        if (hasY)
        {
          least = std::min(least, blue(x, behindY) + kStraight);
          if (hasX)
            least = std::min(least, blue(behindX, behindY) + kDiagonal);
          const int aheadX = x + step;
          if (aheadX >= 0 && aheadX < size.x)
            least = std::min(least, blue(aheadX, behindY) + kDiagonal);
        }
        blue(x, y) = static_cast<uint8_t>(std::min(least, 255));
      }
  };
  sweep(1);
  sweep(-1);
}

// --- GrassTrample ------------------------------------------------------------

GrassTrample::GrassTrample(const GrassGround& ground,
                           const glm::mat4& model,
                           float bladeHeight)
    : m_bladeHeight(bladeHeight)
{
  if (ground.empty())
    return;
  const GrassGrid& grid = ground.grid();

  // The ground in the world: points across each texel with ground, a few to
  // a trample texel, so every texel under grass gets its height. First where
  // they reach (from the texels' corners), then the heights.
  const glm::vec2 texel = grid.texelSize();
  glm::vec2 low(std::numeric_limits<float>::max());
  glm::vec2 high(std::numeric_limits<float>::lowest());
  for (int y = 0; y < grid.size.y; ++y)
    for (int x = 0; x < grid.size.x; ++x)
    {
      if (!ground.has(x, y))
        continue;
      for (int corner = 0; corner < 4; ++corner)
      {
        const glm::vec2 local =
            grid.min + (glm::vec2(x, y) +
                        glm::vec2(float(corner & 1), float(corner >> 1))) *
                           texel;
        const glm::vec2 world(model *
                              glm::vec4(local, ground.texel(x, y).w, 1.0f));
        low = glm::min(low, world);
        high = glm::max(high, world);
      }
    }
  if (!(high.x >= low.x))
    return;

  const glm::vec2 extent =
      glm::max(high - low, glm::vec2(1.0f / kTexelsPerUnit));
  const float side = std::max(
      1.0f / kTexelsPerUnit, std::max(extent.x, extent.y) / kMaxTexels);
  // A texel's margin all round, so grass at the edge samples inside.
  m_min = low - side;
  m_size = glm::clamp(glm::ivec2(glm::ceil((extent + 2.0f * side) / side)),
                      glm::ivec2(1),
                      glm::ivec2(kMaxTexels));
  m_max = m_min + glm::vec2(m_size) * side;

  const std::size_t count = std::size_t(m_size.x) * m_size.y;
  m_ground.assign(count, -std::numeric_limits<float>::infinity());
  m_amount.assign(count, 0.0f);
  m_direction.assign(count, glm::vec2(0.0f));
  const float worldTexel = std::max(glm::length(glm::vec3(model[0])) * texel.x,
                                    glm::length(glm::vec3(model[1])) * texel.y);
  const int steps = std::clamp(int(std::ceil(worldTexel / side)) + 1, 1, 8);
  for (int y = 0; y < grid.size.y; ++y)
    for (int x = 0; x < grid.size.x; ++x)
    {
      if (!ground.has(x, y))
        continue;
      const float height = ground.texel(x, y).w;
      for (int sy = 0; sy < steps; ++sy)
        for (int sx = 0; sx < steps; ++sx)
        {
          const glm::vec2 local =
              grid.min +
              (glm::vec2(x, y) + (glm::vec2(sx, sy) + 0.5f) / float(steps)) *
                  texel;
          const glm::vec3 point(model * glm::vec4(local, height, 1.0f));
          if (const int i = indexOf(glm::vec2(point)); i >= 0)
            m_ground[i] = std::max(m_ground[i], point.z);
        }
    }

  // Fill gaps a texel or two wide from their neighbours.
  for (int pass = 0; pass < 2; ++pass)
  {
    const std::vector<float> known = m_ground;
    for (int y = 0; y < m_size.y; ++y)
      for (int x = 0; x < m_size.x; ++x)
      {
        float& height = m_ground[std::size_t(y) * m_size.x + x];
        if (std::isfinite(height))
          continue;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            const int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < m_size.x && ny < m_size.y)
              height = std::max(height, known[std::size_t(ny) * m_size.x + nx]);
          }
      }
  }

  // Nothing pressed yet.
  m_pixels.resize(count * 4);
  for (std::size_t i = 0; i < count; ++i)
  {
    m_pixels[i * 4 + 0] = m_pixels[i * 4 + 1] = toByte(0.5f);
    m_pixels[i * 4 + 2] = 0;
    m_pixels[i * 4 + 3] = 255;
  }
  m_tiles = (m_size + kTile - 1) / kTile;
  m_tileFlags.assign(std::size_t(m_tiles.x) * m_tiles.y, 0);
  markAllChanged();
}

int GrassTrample::indexOf(glm::vec2 point) const
{
  if (m_size.x <= 0)
    return -1;
  const glm::ivec2 texel(
      glm::floor((point - m_min) / (m_max - m_min) * glm::vec2(m_size)));
  if (texel.x < 0 || texel.y < 0 || texel.x >= m_size.x || texel.y >= m_size.y)
    return -1;
  return texel.y * m_size.x + texel.x;
}

bool GrassTrample::update(float seconds,
                          std::span<const GrassBender> benders,
                          float recovery)
{
  if (m_size.x <= 0)
    return false;

  // Standing back up, where it's down.
  const float rise = recovery > 0.0f ? seconds / recovery : 1.0f;
  for (int ty = 0; ty < m_tiles.y; ++ty)
    for (int tx = 0; tx < m_tiles.x; ++tx)
    {
      uint8_t& flags = m_tileFlags[std::size_t(ty) * m_tiles.x + tx];
      if (!(flags & kDown) || rise <= 0.0f)
        continue;
      flags |= kTouched;
      bool down = false;
      const glm::ivec2 to =
          glm::min(glm::ivec2(tx + 1, ty + 1) * kTile, m_size);
      for (int y = ty * kTile; y < to.y; ++y)
        for (int x = tx * kTile; x < to.x; ++x)
        {
          float& amount = m_amount[std::size_t(y) * m_size.x + x];
          if (amount > 0.0f)
            amount = std::max(amount - rise, 0.0f);
          down = down || amount > 0.0f;
        }
      if (!down)
        flags &= ~kDown;
    }

  const glm::vec2 texel = (m_max - m_min) / glm::vec2(m_size);
  for (const GrassBender& bender : benders)
  {
    if (bender.radius <= 0.0f)
      continue;
    const float reach =
        bender.radius * 1.15f; // grass is pushed a little beyond
    if (glm::vec2(bender.center).x + reach < m_min.x ||
        glm::vec2(bender.center).y + reach < m_min.y ||
        glm::vec2(bender.center).x - reach > m_max.x ||
        glm::vec2(bender.center).y - reach > m_max.y)
      continue;
    const glm::ivec2 low = glm::clamp(
        glm::ivec2(
            glm::floor((glm::vec2(bender.center) - reach - m_min) / texel)),
        glm::ivec2(0),
        m_size - 1);
    const glm::ivec2 high = glm::clamp(
        glm::ivec2(
            glm::floor((glm::vec2(bender.center) + reach - m_min) / texel)),
        glm::ivec2(0),
        m_size - 1);
    // Moving, it lays the grass down the way it's going (fanning out a little
    // at its sides), all along its path; standing still, it presses it outward.
    const glm::vec2 velocity(bender.velocity);
    const float speed = glm::length(velocity);
    const glm::vec2 heading =
        speed > 1e-4f ? velocity / speed : glm::vec2(0.0f);
    const float moving = smoothstep(0.2f, 1.2f, speed);
    for (int y = low.y; y <= high.y; ++y)
      for (int x = low.x; x <= high.x; ++x)
      {
        const int i = y * m_size.x + x;
        const float ground = m_ground[i];
        if (!std::isfinite(ground))
          continue; // no grass here
        // Does it reach into the grass here: below its tips, above its roots?
        const float top = ground + m_bladeHeight;
        if (bender.center.z - bender.radius > top ||
            bender.center.z + bender.radius < ground)
          continue;
        // Its width where it meets the grass: all of it below the tips, less
        // as just its underside dips in.
        const float above = bender.center.z - top;
        const float footprint =
            above <= 0.0f
                ? reach
                : std::sqrt(std::max(
                      bender.radius * bender.radius - above * above, 0.0f)) *
                      1.15f;
        const glm::vec2 middle = m_min + (glm::vec2(x, y) + 0.5f) * texel;
        const glm::vec2 offset = middle - glm::vec2(bender.center);
        const float distance = glm::length(offset);
        if (footprint <= 0.0f || distance >= footprint)
          continue;
        const float pressed =
            1.0f - smoothstep(0.6f, 1.0f, distance / footprint);
        // Moving over grass already down, it lays it its own way; still, it
        // only presses harder.
        if (pressed <= m_amount[i] && moving < 0.5f)
          continue;
        const glm::vec2 outward =
            distance > 1e-4f ? offset / distance : glm::vec2(0.0f);
        const glm::vec2 sideways =
            outward - heading * glm::dot(outward, heading);
        const glm::vec2 laid = heading + sideways * 0.3f;
        glm::vec2 direction = outward * (1.0f - moving) + laid * moving;
        const float length = glm::length(direction);
        m_direction[i] =
            length > 1e-4f ? direction / length : glm::vec2(1.0f, 0.0f);
        m_amount[i] = std::max(m_amount[i], pressed);
        m_tileFlags[tileOf(x, y)] |= kDown | kTouched;
      }
  }

  // What that did to its image (pressed back just as it was: nothing).
  bool changed = false;
  for (int ty = 0; ty < m_tiles.y; ++ty)
    for (int tx = 0; tx < m_tiles.x; ++tx)
    {
      uint8_t& flags = m_tileFlags[std::size_t(ty) * m_tiles.x + tx];
      if (!(flags & kTouched))
        continue;
      flags &= ~kTouched;
      const glm::ivec2 to =
          glm::min(glm::ivec2(tx + 1, ty + 1) * kTile, m_size);
      for (int y = ty * kTile; y < to.y; ++y)
        for (int x = tx * kTile; x < to.x; ++x)
        {
          const std::size_t i = std::size_t(y) * m_size.x + x;
          const glm::vec2 direction = m_direction[i] * 0.5f + 0.5f;
          const uint8_t pixel[3] = {
              toByte(direction.x), toByte(direction.y), toByte(m_amount[i])};
          uint8_t* at = &m_pixels[i * 4];
          if (at[0] == pixel[0] && at[1] == pixel[1] && at[2] == pixel[2])
            continue;
          at[0] = pixel[0];
          at[1] = pixel[1];
          at[2] = pixel[2];
          if (m_changedMin.x >= m_changedMax.x ||
              m_changedMin.y >= m_changedMax.y)
          {
            m_changedMin = {x, y};
            m_changedMax = {x + 1, y + 1};
          }
          else
          {
            m_changedMin = glm::min(m_changedMin, glm::ivec2(x, y));
            m_changedMax = glm::max(m_changedMax, glm::ivec2(x + 1, y + 1));
          }
          changed = true;
        }
    }
  return changed;
}

float GrassTrample::flattened(glm::vec2 point) const
{
  const int i = indexOf(point);
  return i >= 0 ? m_amount[i] : 0.0f;
}

glm::vec2 GrassTrample::direction(glm::vec2 point) const
{
  const int i = indexOf(point);
  return i >= 0 && m_amount[i] > 0.0f ? m_direction[i] : glm::vec2(0.0f);
}

void GrassTrample::pixels(glm::ivec2 from,
                          glm::ivec2 to,
                          std::vector<uint8_t>& out) const
{
  from = glm::clamp(from, glm::ivec2(0), m_size);
  to = glm::clamp(to, from, m_size);
  const std::size_t row = std::size_t(to.x - from.x) * 4;
  out.resize(row * (to.y - from.y));
  for (int y = from.y; y < to.y; ++y)
    std::copy_n(m_pixels.begin() + (std::size_t(y) * m_size.x + from.x) * 4,
                row,
                out.begin() + std::size_t(y - from.y) * row);
}

std::pair<glm::ivec2, glm::ivec2> GrassTrample::takeChanged()
{
  const std::pair<glm::ivec2, glm::ivec2> changed{m_changedMin, m_changedMax};
  m_changedMin = m_changedMax = glm::ivec2(0);
  return changed;
}

void GrassTrample::markAllChanged()
{
  m_changedMin = glm::ivec2(0);
  m_changedMax = m_size;
}

} // namespace sfs
