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
  return ground;
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

std::string_view GrassMaps::name(Channel channel)
{
  static constexpr std::array<std::string_view, kChannels> kNames = {
      "coverage", "length", "data0", "data1", "data2", "data3"};
  return kNames[channel];
}

GrassMaps::GrassMaps(const GrassGrid& grid) : m_grid(grid)
{
  for (int c = 0; c < kChannels; ++c)
    m_values[c].assign(
        m_grid.count(), toByte(defaultValue(static_cast<Channel>(c))));
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

void GrassMaps::set(Channel channel, int x, int y, float value)
{
  if (x < 0 || y < 0 || x >= m_grid.size.x || y >= m_grid.size.y)
    return;
  uint8_t& stored = m_values[channel][index(x, y)];
  const uint8_t next = toByte(value);
  if (stored == next)
    return;
  stored = next;
  touch(x, y);
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
      const uint8_t current = m_values[channel][index(x, y)];
      if (stored == current && weight > 0.0f && next != old)
        stored = static_cast<uint8_t>(
            std::clamp(int(current) + (next > old ? 1 : -1), 0, 255));
      if (stored != current)
      {
        m_values[channel][index(x, y)] = stored;
        touch(x, y);
      }
    }
}

void GrassMaps::fill(Channel channel, float value)
{
  if (empty())
    return;
  std::fill(m_values[channel].begin(), m_values[channel].end(), toByte(value));
  markAllChanged();
}

void GrassMaps::touch(int x, int y)
{
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

void GrassMaps::markAllChanged()
{
  m_changedMin = glm::ivec2(0);
  m_changedMax = m_grid.size;
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
      const std::size_t at = index(x, y);
      shape[i + 0] = m_values[Coverage][at];
      shape[i + 1] = m_values[Length][at];
      shape[i + 2] = 0;
      shape[i + 3] = 0;
      for (int k = 0; k < 4; ++k)
        data[i + k] = m_values[Data0 + k][at];
    }
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
    const std::vector<uint8_t>& values = m_values[c];
    if (std::any_of(values.begin(),
                    values.end(),
                    [&](uint8_t value) { return value != standard; }))
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
      maps.m_values[c] = std::move(*values);
    }
    return maps;
  }
  catch (const nlohmann::json::exception&)
  {
    return {};
  }
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
  bool changed = false;

  // Standing back up.
  const float rise = recovery > 0.0f ? seconds / recovery : 1.0f;
  if (rise > 0.0f)
    for (float& amount : m_amount)
      if (amount > 0.0f)
      {
        amount = std::max(amount - rise, 0.0f);
        changed = true;
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

std::vector<uint8_t> GrassTrample::image() const
{
  std::vector<uint8_t> pixels(m_amount.size() * 4);
  for (std::size_t i = 0; i < m_amount.size(); ++i)
  {
    const glm::vec2 direction = m_direction[i] * 0.5f + 0.5f;
    pixels[i * 4 + 0] = toByte(direction.x);
    pixels[i * 4 + 1] = toByte(direction.y);
    pixels[i * 4 + 2] = toByte(m_amount[i]);
    pixels[i * 4 + 3] = 255;
  }
  return pixels;
}

} // namespace sfs
