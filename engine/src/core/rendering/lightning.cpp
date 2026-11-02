#include "engine/core/rendering/lightning.h"

#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sfs
{

namespace
{

// A strike's own random numbers: a small counter-based generator (the same
// seed gives the same strike everywhere).
class Random
{
public:
  explicit Random(uint32_t seed) : m_state(seed * 0x9e3779b9u + 0x7f4a7c15u) {}

  // 0..1
  float next()
  {
    uint32_t h = (m_state += 0x6d2b79f5u);
    h = (h ^ (h >> 15)) * (h | 1u);
    h ^= h + (h ^ (h >> 7)) * (h | 61u);
    h ^= h >> 14;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
  }
  float range(float low, float high) { return low + (high - low) * next(); }

private:
  uint32_t m_state;
};

// One stroke: when it flashes, how bright, how fast it dies off, and how
// long a continuing current holds it at a share of that after (0: none).
struct Stroke
{
  float start = 0.0f;
  float amplitude = 1.0f;
  float decay = 0.025f;
  float hold = 0.0f;
};

constexpr std::size_t kMostStrokes = 7;
// How bright the afterimage starts (of the stroke's), and how fast it fades.
constexpr float kAfterimage = 0.18f;
constexpr float kAfterimageFade = 0.2f;

struct Strokes
{
  std::array<Stroke, kMostStrokes> strokes;
  std::size_t count = 0;
};

// A cloud-to-ground strike's 2 to 5 return strokes, the first the brightest,
// 40 to 120 ms apart, and maybe one held by a continuing current; an
// in-cloud strike's 3 to 7 softer pulses, closer together.
Strokes strokesOf(const LightningStrike& strike)
{
  Random random(strike.seed ^ 0x5bd1e995u);
  Strokes out;
  const bool cloud = strike.inCloud;
  out.count = cloud ? 3 + static_cast<std::size_t>(random.next() * 4.99f)
                    : 2 + static_cast<std::size_t>(random.next() * 3.99f);
  out.count = std::min(out.count, kMostStrokes);
  const std::size_t held =
      random.next() < 0.5f ? static_cast<std::size_t>(
                                 random.next() * static_cast<float>(out.count))
                           : kMostStrokes;
  float at = 0.0f;
  for (std::size_t i = 0; i < out.count; ++i)
  {
    Stroke& stroke = out.strokes[i];
    stroke.start = at;
    stroke.amplitude =
        i == 0 ? 1.0f
               : (cloud ? random.range(0.3f, 1.0f) : random.range(0.45f, 0.9f));
    stroke.decay =
        cloud ? random.range(0.035f, 0.07f) : random.range(0.018f, 0.032f);
    if (i == held && !cloud)
      stroke.hold = random.range(0.06f, 0.14f);
    at += stroke.hold +
          (cloud ? random.range(0.03f, 0.14f) : random.range(0.04f, 0.12f));
  }
  return out;
}

// One stroke's light `t` seconds after it began: a flash (at once: it
// rises in microseconds), dying off; held at a third (fading slowly) while a
// continuing current flows.
float strokeLight(const Stroke& stroke, float t)
{
  if (t < 0.0f)
    return 0.0f;
  float light = std::exp(-t / stroke.decay);
  if (stroke.hold > 0.0f)
  {
    const float held =
        0.35f *
        (t < stroke.hold ? 1.0f : std::exp(-(t - stroke.hold) / stroke.decay));
    light =
        std::max(light, held * (1.0f - 0.3f * std::min(t / stroke.hold, 1.0f)));
  }
  return stroke.amplitude * light;
}

// A straight run displaced again and again, finer each time: the jagged
// path from `from` to `to`, `depth` halvings of it (2^depth pieces), each
// midpoint moved across the run by up to `rough` x its piece's length.
std::vector<glm::vec3> jagged(const glm::vec3& from,
                              const glm::vec3& to,
                              int depth,
                              float rough,
                              Random& random)
{
  std::vector<glm::vec3> points{from, to};
  const glm::vec3 along = glm::normalize(to - from);
  const glm::vec3 helper =
      std::abs(along.z) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
  const glm::vec3 side = glm::normalize(glm::cross(along, helper));
  const glm::vec3 other = glm::cross(along, side);
  float scale = rough;
  for (int level = 0; level < depth; ++level)
  {
    std::vector<glm::vec3> finer;
    finer.reserve(points.size() * 2);
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
    {
      const glm::vec3 a = points[i];
      const glm::vec3 b = points[i + 1];
      const float length = glm::distance(a, b);
      const glm::vec3 shift =
          (side * random.range(-1.0f, 1.0f) +
           other * random.range(-1.0f, 1.0f)) *
              (length * scale) +
          along * (length * 0.15f * random.range(-1.0f, 1.0f));
      finer.push_back(a);
      finer.push_back((a + b) * 0.5f + shift);
    }
    finer.push_back(points.back());
    points = std::move(finer);
    // Rougher at the finer scales (as channels are: tortuous up close).
    scale = std::min(scale * 1.1f, 0.45f);
  }
  return points;
}

void addPath(std::vector<LightningSegment>& out,
             const std::vector<glm::vec3>& points,
             float width,
             float brightness,
             bool branch,
             float fadeTo)
{
  const std::size_t n = points.size();
  for (std::size_t i = 0; i + 1 < n; ++i)
  {
    // A branch fades (and thins) toward its tip.
    const float along = static_cast<float>(i) / static_cast<float>(n - 1);
    const float fade = 1.0f + (fadeTo - 1.0f) * along;
    out.push_back({points[i],
                   points[i + 1],
                   width * (0.5f + 0.5f * fade),
                   brightness * fade,
                   branch});
  }
}

void addBranches(std::vector<LightningSegment>& out,
                 const std::vector<glm::vec3>& path,
                 float length,
                 float width,
                 float brightness,
                 int generation,
                 Random& random)
{
  const int count = generation == 0 ? 3 + static_cast<int>(random.next() * 4)
                                    : static_cast<int>(random.next() * 2.5f);
  const glm::vec3 down = glm::normalize(path.back() - path.front());
  for (int k = 0; k < count; ++k)
  {
    // From somewhere along the upper part of the path, down and out at
    // 25 to 60 degrees from it.
    const float at = random.range(0.05f, generation == 0 ? 0.7f : 0.6f);
    const std::size_t index = std::min(
        path.size() - 2,
        static_cast<std::size_t>(at * static_cast<float>(path.size() - 1)));
    const glm::vec3 start = path[index];
    const float turn = random.range(0.0f, 6.2831853f);
    const glm::vec3 helper =
        std::abs(down.z) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
    const glm::vec3 side = glm::normalize(glm::cross(down, helper));
    const glm::vec3 other = glm::cross(down, side);
    const glm::vec3 out1 = side * std::cos(turn) + other * std::sin(turn);
    const float angle = random.range(0.45f, 1.05f);
    const glm::vec3 direction =
        glm::normalize(down * std::cos(angle) + out1 * std::sin(angle));
    const float reach = length * random.range(0.12f, 0.35f);
    const std::vector<glm::vec3> branch =
        jagged(start, start + direction * reach, 5, 0.18f, random);
    const float lit = brightness * random.range(0.35f, 0.6f);
    addPath(out, branch, width * 0.6f, lit, true, 0.15f);
    if (generation < 1)
      addBranches(
          out, branch, reach, width * 0.6f, lit, generation + 1, random);
  }
}

} // namespace

float lightningBrightness(const LightningStrike& strike, float seconds)
{
  const Strokes strokes = strokesOf(strike);
  float light = 0.0f;
  for (std::size_t i = 0; i < strokes.count; ++i)
    light = std::max(
        light,
        strokeLight(strokes.strokes[i], seconds - strokes.strokes[i].start));
  return light;
}

float lightningChannelBrightness(const LightningStrike& strike, float seconds)
{
  const Strokes strokes = strokesOf(strike);
  float light = 0.0f;
  for (std::size_t i = 0; i < strokes.count; ++i)
  {
    const Stroke& stroke = strokes.strokes[i];
    const float t = seconds - stroke.start;
    if (t < 0.0f)
      continue;
    light = std::max(
        {light,
         strokeLight(stroke, t),
         stroke.amplitude * kAfterimage * std::exp(-t / kAfterimageFade)});
  }
  return light;
}

float lightningDuration(const LightningStrike& strike)
{
  const Strokes strokes = strokesOf(strike);
  const Stroke& last = strokes.strokes[strokes.count - 1];
  // Until the last afterimage has faded below a hundredth.
  return last.start + last.hold +
         std::max(kAfterimageFade * std::log(100.0f * kAfterimage),
                  last.decay * std::log(100.0f));
}

glm::vec3 lightningTop(const LightningStrike& strike, float cloudBase)
{
  Random random(strike.seed ^ 0x27d4eb2fu);
  const float height = std::max(cloudBase - strike.position.z, 50.0f);
  const float angle = random.range(0.0f, 6.2831853f);
  const float lean = height * random.range(0.05f, 0.3f);
  return strike.position +
         glm::vec3(std::cos(angle) * lean, std::sin(angle) * lean, height);
}

std::vector<LightningSegment> lightningChannel(const LightningStrike& strike,
                                               const glm::vec3& top)
{
  std::vector<LightningSegment> out;
  if (strike.inCloud)
    return out;
  Random random(strike.seed);
  const glm::vec3 ground = strike.position;
  const float length = glm::distance(top, ground);
  if (length < 1.0f)
    return out;
  // The main channel: 128 pieces (about 12 m each from a cloud 1.5 km up),
  // fading out into the cloud over its top fifth.
  const std::vector<glm::vec3> path = jagged(top, ground, 7, 0.16f, random);
  const std::size_t n = path.size();
  for (std::size_t i = 0; i + 1 < n; ++i)
  {
    const float along = static_cast<float>(i) / static_cast<float>(n - 1);
    const float emerging = std::min(along / 0.2f, 1.0f);
    out.push_back({path[i], path[i + 1], 1.0f, 0.2f + 0.8f * emerging, false});
  }
  addBranches(out, path, length, 1.0f, 1.0f, 0, random);
  return out;
}

MeshData lightningMesh(const std::vector<LightningSegment>& segments,
                       const glm::vec3& eye,
                       float width,
                       bool branches)
{
  MeshData mesh;
  for (const LightningSegment& segment : segments)
  {
    if (segment.branch != branches)
      continue;
    const glm::vec3 along = segment.to - segment.from;
    const glm::vec3 middle = (segment.from + segment.to) * 0.5f;
    glm::vec3 toEye = eye - middle;
    glm::vec3 across = glm::cross(along, toEye);
    if (glm::dot(across, across) < 1e-12f)
      continue;
    across = glm::normalize(across) * (0.5f * width * segment.width);
    toEye = glm::normalize(toEye);
    // Two quads side by side: bright down the middle, nothing at the edges.
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 light(segment.brightness);
    for (const glm::vec3& end : {segment.from, segment.to})
    {
      mesh.vertices.push_back({end - across, toEye, glm::vec3(0.0f), {0, 0}});
      mesh.vertices.push_back({end, toEye, light, {0.5f, 0}});
      mesh.vertices.push_back({end + across, toEye, glm::vec3(0.0f), {1, 0}});
    }
    // Wound counter-clockwise as the eye sees it (its front face).
    const glm::vec3 face = glm::cross(segment.to - segment.from, across);
    const bool flip = glm::dot(face, toEye) < 0.0f;
    for (uint32_t q = 0; q < 2; ++q)
    {
      const uint32_t a = base + q;
      const uint32_t b = base + 3 + q;
      if (flip)
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
      else
        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  if (!mesh.indices.empty())
  {
    MeshSection section;
    section.indexCount = static_cast<uint32_t>(mesh.indices.size());
    section.material.baseColor = glm::vec4(0.82f, 0.86f, 1.0f, 1.0f);
    section.material.unlit = true;
    section.material.alphaMode = AlphaMode::Additive;
    section.name = "lightning";
    mesh.sections.push_back(std::move(section));
  }
  return mesh;
}

float lightningLineLight(const glm::vec3& low,
                         const glm::vec3& high,
                         const glm::vec3& point)
{
  // Along the line (s) and away from it (h): the integral of 1 / (h^2 + x^2)
  // over the channel's length, x from the nearest point on its line.
  const glm::vec3 line = high - low;
  const float length = glm::length(line);
  if (length < 1e-4f)
  {
    const glm::vec3 to = point - low;
    return 1.0f / std::max(glm::dot(to, to), 1.0f);
  }
  const glm::vec3 up = line / length;
  const glm::vec3 to = point - low;
  const float s = glm::dot(to, up);
  const float h = std::max(glm::length(to - up * s), 1.0f);
  return (std::atan((length - s) / h) - std::atan(-s / h)) / h;
}

glm::vec3 lightningLightCentre(const glm::vec3& low,
                               const glm::vec3& high,
                               const glm::vec3& point)
{
  const glm::vec3 line = high - low;
  const float length = glm::length(line);
  if (length < 1e-4f)
    return low;
  const glm::vec3 up = line / length;
  const glm::vec3 to = point - low;
  const float s = glm::dot(to, up);
  const float h = std::max(glm::length(to - up * s), 1.0f);
  // The light-weighted mean of x along it: the integral of x / (h^2 + x^2)
  // over that of 1 / (h^2 + x^2).
  const float a = -s;
  const float b = length - s;
  const float weight = (std::atan(b / h) - std::atan(a / h)) / h;
  const float moment = 0.5f * std::log((h * h + b * b) / (h * h + a * a));
  const float x = weight > 1e-12f ? moment / weight : 0.0f;
  return low + up * std::clamp(s + x, 0.0f, length);
}

} // namespace sfs
