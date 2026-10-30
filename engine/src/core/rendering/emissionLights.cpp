#include "engine/core/rendering/emissionLights.h"

#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/core/rendering/materialNodes.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sfs
{

namespace
{

// Samples: one per this much area, within these bounds.
constexpr float kAreaPerSample = 0.02f;
constexpr std::size_t kMinSamples = 16;
constexpr std::size_t kMaxSamples = 128;

// Cells: at least this big, and big enough that an object has at most about
// kMaxCells of them by area.
constexpr float kMinCell = 2.0f;
constexpr float kMaxCells = 32.0f;

// How faint a light's reach ends: the distance where light from its glow
// (brightness x area / (pi x distance^2), as from a small glowing patch) falls
// to this.
constexpr float kFaintest = 0.02f;
constexpr float kPi = 3.14159265f;

// Lights no stronger or wider than this.
constexpr float kMaxIntensity = 10.0f;
constexpr float kMaxRadius = 60.0f;

float brightest(const glm::vec3& c)
{
  return std::max(std::max(c.r, c.g), c.b);
}

} // namespace

bool canGlow(const MeshMaterial& material)
{
  if (!material.shader)
    return false;
  // Anything depending on the point (a texture, noise, time) might glow
  // somewhere; otherwise it's one colour everywhere, worked out once.
  if (any(material.shader->reads(materialGraph::kEmission)))
    return true;
  const materialGraph::SurfaceValues surface = material.shader->evaluate(
      {}, material.shaderParameters, material.shaderTextures);
  return brightest(surface.emission) > 0.0f;
}

std::size_t surfaceSampleCount(float area)
{
  const float wanted = std::ceil(std::max(area, 0.0f) / kAreaPerSample);
  return static_cast<std::size_t>(std::clamp(wanted,
                                             static_cast<float>(kMinSamples),
                                             static_cast<float>(kMaxSamples)));
}

std::vector<SurfaceSample> sampleSurface(std::span<const MeshVertex> vertices,
                                         std::span<const uint32_t> indices,
                                         std::size_t count)
{
  // Each triangle's area, running total.
  std::vector<float> running;
  running.reserve(indices.size() / 3);
  float total = 0.0f;
  for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
  {
    const auto& a = vertices[indices[t]].position;
    const auto& b = vertices[indices[t + 1]].position;
    const auto& c = vertices[indices[t + 2]].position;
    total += 0.5f * glm::length(glm::cross(b - a, c - a));
    running.push_back(total);
  }
  std::vector<SurfaceSample> samples;
  if (total <= 0.0f || count == 0)
    return samples;

  // Evenly along the running area (each sample an equal share), and within
  // its triangle at the next point of a low-discrepancy sequence (R2), so
  // samples spread evenly and the same mesh always gives the same ones.
  samples.reserve(count);
  const float share = total / static_cast<float>(count);
  std::size_t triangle = 0;
  for (std::size_t k = 0; k < count; ++k)
  {
    const float at = (static_cast<float>(k) + 0.5f) * share;
    while (triangle + 1 < running.size() && running[triangle] < at)
      ++triangle;

    float u = std::fmod(0.5f + 0.7548776662f * static_cast<float>(k), 1.0f);
    float v = std::fmod(0.5f + 0.5698402910f * static_cast<float>(k), 1.0f);
    if (u + v > 1.0f)
    {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    const float w = 1.0f - u - v;

    const MeshVertex& a = vertices[indices[triangle * 3]];
    const MeshVertex& b = vertices[indices[triangle * 3 + 1]];
    const MeshVertex& c = vertices[indices[triangle * 3 + 2]];
    SurfaceSample sample;
    sample.position = a.position * w + b.position * u + c.position * v;
    const glm::vec3 normal = a.normal * w + b.normal * u + c.normal * v;
    const float length = glm::length(normal);
    sample.normal = length > 1e-6f
                        ? normal / length
                        : glm::normalize(glm::cross(b.position - a.position,
                                                    c.position - a.position));
    sample.uv = a.uv * w + b.uv * u + c.uv * v;
    sample.vertexColor = a.color * w + b.color * u + c.color * v;
    const glm::vec3 positions[3] = {a.position, b.position, c.position};
    const glm::vec2 uvs[3] = {a.uv, b.uv, c.uv};
    materialGraph::textureDirections(
        positions, uvs, sample.alongU, sample.upImage);
    sample.area = share;
    samples.push_back(sample);
  }
  return samples;
}

void emissionLights(std::span<const GlowPoint> points,
                    float strength,
                    float range,
                    std::vector<PointLight>& out)
{
  if (points.empty() || strength <= 0.0f || range <= 0.0f)
    return;

  glm::vec3 low(1e30f), high(-1e30f);
  float area = 0.0f;
  for (const GlowPoint& point : points)
  {
    low = glm::min(low, point.position);
    high = glm::max(high, point.position);
    area += point.area;
  }

  // One cell for a small object; cells from its low corner for a big one.
  const float cell = std::max(kMinCell, std::sqrt(area / kMaxCells));
  const glm::vec3 extent = high - low;
  const bool single =
      std::max(std::max(extent.x, extent.y), extent.z) <= cell * 1.5f;

  struct Cell
  {
    glm::vec3 light{0.0f};    // emission x area, summed
    float glowingArea = 0.0f; // of the points that glow
    glm::vec3 where{0.0f};    // positions weighted by brightness x area
    float weight = 0.0f;
    glm::vec3 low{1e30f}, high{-1e30f}; // around its glowing points
  };
  std::unordered_map<uint64_t, Cell> cells;
  for (const GlowPoint& point : points)
  {
    const float glow = brightest(point.emission);
    if (glow <= 0.0f || point.area <= 0.0f)
      continue;
    uint64_t key = 0;
    if (!single)
    {
      const glm::uvec3 index((point.position - low) / cell);
      key = (uint64_t(index.x) << 42) | (uint64_t(index.y) << 21) | index.z;
    }
    Cell& c = cells[key];
    c.light += point.emission * point.area;
    c.glowingArea += point.area;
    c.where += point.position * (glow * point.area);
    c.weight += glow * point.area;
    c.low = glm::min(c.low, point.position);
    c.high = glm::max(c.high, point.position);
  }

  for (const auto& [key, c] : cells)
  {
    const float amount = brightest(c.light); // brightness x area
    if (amount <= 0.0f || c.weight <= 0.0f)
      continue;
    PointLight light;
    light.position = c.where / c.weight;
    light.color = c.light / amount;
    light.intensity =
        std::min(amount / c.glowingArea * strength, kMaxIntensity);
    light.radius =
        std::min(std::sqrt(amount / (kPi * kFaintest)) * range, kMaxRadius);
    if (light.intensity < 0.01f || light.radius < 0.05f)
      continue;
    // It comes from the glowing surface, so it's as big as that (for
    // highlights), and that surface mustn't block it: shadows start beyond a
    // box around the light holding all of it (with a little room, as the
    // samples only nearly reach the surface's edges).
    const glm::vec3 reach =
        glm::max(c.high - light.position, light.position - c.low);
    light.sourceRadius = std::max(std::max(reach.x, reach.y), reach.z);
    light.shadowNear = light.sourceRadius * 1.1f + 0.02f;
    light.castsShadow = true;
    out.push_back(light);
  }
}

} // namespace sfs
