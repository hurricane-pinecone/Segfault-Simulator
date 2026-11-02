#include "engine/core/rendering/deformation.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/packing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sfs
{

namespace
{

constexpr float kNone = std::numeric_limits<float>::quiet_NaN();
constexpr int kTileTexels = DeformationMap::kTile * DeformationMap::kTile;
// Offsets nearer 0 than this (metres) are flat ground.
constexpr float kFlat = 0.0005f;
// A texel the deformer's underside is within this of (metres) is under it:
// it neither rises in a ridge nor counts as clear ground.
constexpr float kUnder = 0.01f;
// A texel this far down (metres) is a rut already: soil isn't pushed up
// into it.
constexpr float kInRut = 0.005f;
// An underside's samples across a texel (each way).
constexpr int kSamples = 4;

int floorDiv(int value, int by)
{
  return value >= 0 ? value / by : -((-value + by - 1) / by);
}

// A texel's index in its tile.
int indexInTile(glm::ivec2 texel)
{
  const glm::ivec2 tile(floorDiv(texel.x, DeformationMap::kTile),
                        floorDiv(texel.y, DeformationMap::kTile));
  const glm::ivec2 local = texel - tile * DeformationMap::kTile;
  return local.y * DeformationMap::kTile + local.x;
}

} // namespace

float redAt(const TextureData& image, glm::vec2 uv)
{
  if (image.empty() ||
      image.rgba.size() < std::size_t(image.width) * image.height * 4)
    return 0.0f;
  const glm::vec2 at = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f)) *
                           glm::vec2(image.width, image.height) -
                       0.5f;
  const glm::ivec2 low(glm::floor(at));
  const glm::vec2 f = at - glm::vec2(low);
  const auto texel = [&](int x, int y)
  {
    x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
    return image.rgba[(std::size_t(y) * image.width + x) * 4] / 255.0f;
  };
  return glm::mix(
      glm::mix(texel(low.x, low.y), texel(low.x + 1, low.y), f.x),
      glm::mix(texel(low.x, low.y + 1), texel(low.x + 1, low.y + 1), f.x),
      f.y);
}

namespace
{

// Bytes for save(): unsigned LEB128, and signed values zigzagged into it.
void putVarint(std::vector<uint8_t>& out, uint64_t value)
{
  while (value >= 0x80)
  {
    out.push_back(static_cast<uint8_t>(value | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

void putSigned(std::vector<uint8_t>& out, int64_t value)
{
  putVarint(
      out,
      (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63));
}

void putFloat(std::vector<uint8_t>& out, float value)
{
  uint8_t bytes[4];
  std::memcpy(bytes, &value, 4);
  out.insert(out.end(), bytes, bytes + 4);
}

struct Reader
{
  std::span<const uint8_t> bytes;
  std::size_t at = 0;
  bool ok = true;

  uint64_t varint()
  {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7)
    {
      if (at >= bytes.size())
        break;
      const uint8_t byte = bytes[at++];
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (!(byte & 0x80))
        return value;
    }
    ok = false;
    return 0;
  }
  int64_t signedValue()
  {
    const uint64_t raw = varint();
    return static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
  }
  float number()
  {
    if (at + 4 > bytes.size())
    {
      ok = false;
      return 0.0f;
    }
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + at, 4);
    at += 4;
    return value;
  }
};

constexpr uint8_t kMagic[4] = {'S', 'F', 'D', 'M'};
constexpr uint8_t kVersion = 1;

} // namespace

DeformationMap::DeformationMap(float texel) : m_texel(std::max(texel, 1e-3f)) {}

glm::ivec2 DeformationMap::texelOf(glm::vec2 point) const
{
  return glm::ivec2(glm::floor(point / m_texel));
}

glm::vec2 DeformationMap::centerOf(glm::ivec2 texel) const
{
  return (glm::vec2(texel) + 0.5f) * m_texel;
}

glm::ivec2 DeformationMap::tileOf(glm::ivec2 texel)
{
  return {floorDiv(texel.x, kTile), floorDiv(texel.y, kTile)};
}

int64_t DeformationMap::keyOf(glm::ivec2 tile)
{
  return (static_cast<int64_t>(tile.y) << 32) ^
         static_cast<int64_t>(static_cast<uint32_t>(tile.x));
}

namespace
{
glm::ivec2 tileOfKey(int64_t key)
{
  return {static_cast<int32_t>(static_cast<uint32_t>(key)),
          static_cast<int32_t>(key >> 32)};
}
} // namespace

DeformationMap::Tile* DeformationMap::tileAt(glm::ivec2 tile)
{
  const auto it = m_tiles.find(keyOf(tile));
  return it != m_tiles.end() ? &it->second : nullptr;
}

const DeformationMap::Tile* DeformationMap::tileAt(glm::ivec2 tile) const
{
  const auto it = m_tiles.find(keyOf(tile));
  return it != m_tiles.end() ? &it->second : nullptr;
}

DeformationMap::Tile& DeformationMap::makeTile(glm::ivec2 tile)
{
  Tile& made = m_tiles[keyOf(tile)];
  if (made.empty())
    made.assign(kTileTexels, 0.0f);
  return made;
}

float DeformationMap::at(glm::ivec2 texel) const
{
  const Tile* tile = tileAt(tileOf(texel));
  return tile ? (*tile)[indexInTile(texel)] : 0.0f;
}

void DeformationMap::set(glm::ivec2 texel, float offset)
{
  const glm::ivec2 tile = tileOf(texel);
  Tile* existing = tileAt(tile);
  if (!existing && std::abs(offset) < kFlat)
    return;
  Tile& into = existing ? *existing : makeTile(tile);
  into[indexInTile(texel)] = offset;
  m_deepest = std::min(m_deepest, offset);
  m_changed.insert(keyOf(tile));
}

float DeformationMap::sample(glm::vec2 point) const
{
  const glm::vec2 at = point / m_texel - 0.5f;
  const glm::ivec2 low(glm::floor(at));
  const glm::vec2 f = at - glm::vec2(low);
  const float a = this->at(low);
  const float b = this->at(low + glm::ivec2(1, 0));
  const float c = this->at(low + glm::ivec2(0, 1));
  const float d = this->at(low + glm::ivec2(1, 1));
  return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}

float DeformationMap::press(const Footprint& footprint,
                            const PressSettings& settings)
{
  const std::size_t count = std::size_t(std::max(footprint.size.x, 0)) *
                            std::max(footprint.size.y, 0);
  if (footprint.empty() || footprint.target.size() != count)
    return 0.0f;

  // Down to the targets: what's under the deformer, and what it pressed.
  std::vector<uint8_t> under(count, 0);
  std::vector<uint8_t> pressed(count, 0);
  float pressedOut = 0.0f; // summed heights
  for (int y = 0; y < footprint.size.y; ++y)
    for (int x = 0; x < footprint.size.x; ++x)
    {
      const std::size_t i = std::size_t(y) * footprint.size.x + x;
      float target = footprint.target[i];
      if (std::isnan(target))
        continue;
      const glm::ivec2 texel = footprint.from + glm::ivec2(x, y);
      const float now = at(texel);
      const float cover = footprint.coverage.size() == count
                              ? std::clamp(footprint.coverage[i], 0.0f, 1.0f)
                              : 1.0f;
      if (target > now + kUnder)
        continue;
      if (cover > 0.0f)
        under[i] = 1;
      // Part of it pressed: part way down.
      target = std::max(now + (std::min(target, now) - now) * cover, -kDeepest);
      if (target >= now)
        continue;
      set(texel, target);
      pressedOut += now - target;
      pressed[i] = 1;
    }
  const float area = m_texel * m_texel;
  if (pressedOut > 0.0f && settings.ridge > 0.0f)
    pushUp(footprint, under, pressed, pressedOut, settings);
  if (pressedOut > 0.0f)
    slump(footprint, under, settings);
  return pressedOut * area;
}

void DeformationMap::pushUp(const Footprint& footprint,
                            const std::vector<uint8_t>& under,
                            const std::vector<uint8_t>& pressed,
                            float pressedOut,
                            const PressSettings& settings)
{
  // The soil pushed out rises around what was pressed, within its reach:
  // most near the edge and to the sides of the way it's going, not into
  // ruts or under the deformer itself.
  const int reach =
      std::max(1, static_cast<int>(std::lround(settings.ridgeWidth / m_texel)));
  const float moving = glm::length(settings.motion);
  const glm::vec2 heading =
      moving > 1e-6f ? settings.motion / moving : glm::vec2(0.0f);
  struct Rise
  {
    glm::ivec2 texel;
    float weight;
  };
  std::vector<Rise> rises;
  float total = 0.0f;
  // Each texel's nearest pressed one (two sweeps of a distance transform
  // over the footprint and its reach).
  const glm::ivec2 size = footprint.size + 2 * reach;
  constexpr int kFar = std::numeric_limits<int>::max() / 4;
  std::vector<glm::ivec2> source(
      std::size_t(size.x) * size.y, glm::ivec2(kFar));
  const auto distance2 = [](glm::ivec2 a, glm::ivec2 b)
  {
    const glm::ivec2 d = a - b;
    return d.x * d.x + d.y * d.y;
  };
  for (int y = 0; y < footprint.size.y; ++y)
    for (int x = 0; x < footprint.size.x; ++x)
      if (pressed[std::size_t(y) * footprint.size.x + x])
        source[std::size_t(y + reach) * size.x + x + reach] =
            glm::ivec2(x + reach, y + reach);
  const auto sweep = [&](int step)
  {
    const glm::ivec2 around[4] = {
        {-step, 0}, {-step, -step}, {0, -step}, {step, -step}};
    const int y0 = step > 0 ? 0 : size.y - 1;
    const int x0 = step > 0 ? 0 : size.x - 1;
    for (int y = y0; y >= 0 && y < size.y; y += step)
      for (int x = x0; x >= 0 && x < size.x; x += step)
      {
        const glm::ivec2 here(x, y);
        glm::ivec2& best = source[std::size_t(y) * size.x + x];
        for (const glm::ivec2 d : around)
        {
          const glm::ivec2 n = here + d;
          if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y)
            continue;
          const glm::ivec2 theirs = source[std::size_t(n.y) * size.x + n.x];
          if (theirs.x != kFar && (best.x == kFar || distance2(here, theirs) <
                                                         distance2(here, best)))
            best = theirs;
        }
      }
  };
  sweep(1);
  sweep(-1);
  for (int y = -reach; y < footprint.size.y + reach; ++y)
    for (int x = -reach; x < footprint.size.x + reach; ++x)
    {
      const bool inside =
          x >= 0 && y >= 0 && x < footprint.size.x && y < footprint.size.y;
      if (inside && under[std::size_t(y) * footprint.size.x + x])
        continue;
      const glm::ivec2 texel = footprint.from + glm::ivec2(x, y);
      const float now = at(texel);
      if (now < -kInRut || now >= settings.ridgeMost)
        continue;
      // The nearest texel pressed.
      const glm::ivec2 here(x + reach, y + reach);
      const glm::ivec2 nearestAt =
          source[std::size_t(here.y) * size.x + here.x];
      if (nearestAt.x == kFar)
        continue;
      const glm::ivec2 from = nearestAt - here;
      const int nearest = distance2(here, nearestAt);
      const float distance = std::sqrt(static_cast<float>(nearest));
      if (distance > static_cast<float>(reach))
        continue;
      float weight = 1.0f - (distance - 1.0f) / static_cast<float>(reach);
      if (moving > 1e-6f)
      {
        const glm::vec2 out = -glm::vec2(from) / std::max(distance, 1e-6f);
        // None ahead (it would only be pressed again next frame, piling up
        // in waves) or behind.
        weight *= std::clamp(
            1.0f - 1.25f * std::abs(glm::dot(out, heading)), 0.0f, 1.0f);
      }
      if (weight <= 0.0f)
        continue;
      rises.push_back({texel, weight});
      total += weight;
    }
  if (total <= 0.0f)
    return;
  const float share = pressedOut * std::clamp(settings.ridge, 0.0f, 1.0f);
  for (const Rise& rise : rises)
  {
    const float now = at(rise.texel);
    set(rise.texel,
        std::min(now + share * rise.weight / total, settings.ridgeMost));
  }
}

void DeformationMap::slump(const Footprint& footprint,
                           const std::vector<uint8_t>& under,
                           const PressSettings& settings)
{
  // Around what was pressed (and its ridge), each pair of neighbours
  // steeper than `steepest` evens out by moving soil from the higher to the
  // lower, a few times over: walls a texel apart a rut's depth high would
  // stand in steps, and soil doesn't. What's under the deformer is held.
  const int margin =
      std::max(
          1, static_cast<int>(std::lround(settings.ridgeWidth / m_texel))) +
      2;
  const glm::ivec2 from = footprint.from - margin;
  const glm::ivec2 size = footprint.size + 2 * margin;
  std::vector<float> height(std::size_t(size.x) * size.y);
  std::vector<uint8_t> held(height.size(), 0);
  for (int y = 0; y < size.y; ++y)
    for (int x = 0; x < size.x; ++x)
    {
      const std::size_t i = std::size_t(y) * size.x + x;
      height[i] = at(from + glm::ivec2(x, y));
      const glm::ivec2 in = glm::ivec2(x, y) - margin;
      held[i] = in.x >= 0 && in.y >= 0 && in.x < footprint.size.x &&
                in.y < footprint.size.y &&
                under[std::size_t(in.y) * footprint.size.x + in.x];
    }
  const std::vector<float> was = height;
  const float most = std::max(settings.steepest, 0.1f) * m_texel;
  for (int pass = 0; pass < 3; ++pass)
    for (int y = 0; y < size.y; ++y)
      for (int x = 0; x < size.x; ++x)
        for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(0, 1)})
        {
          if (x + step.x >= size.x || y + step.y >= size.y)
            continue;
          const std::size_t a = std::size_t(y) * size.x + x;
          const std::size_t b = std::size_t(y + step.y) * size.x + x + step.x;
          const float drop = height[a] - height[b];
          if (std::abs(drop) <= most || (held[a] && held[b]))
            continue;
          // Half the excess each way (all of it into or out of the free one
          // beside a held one), and a little less, so it settles.
          float move =
              (std::abs(drop) - most) * 0.4f * (drop > 0 ? 1.0f : -1.0f);
          if (held[a] || held[b])
            move *= 2.0f;
          if (!held[a])
            height[a] -= move;
          if (!held[b])
            height[b] += move;
        }
  for (int y = 0; y < size.y; ++y)
    for (int x = 0; x < size.x; ++x)
    {
      const std::size_t i = std::size_t(y) * size.x + x;
      if (std::abs(height[i] - was[i]) > 1e-5f)
        set(from + glm::ivec2(x, y), std::max(height[i], -kDeepest));
    }
}

void DeformationMap::stamp(glm::vec2 center,
                           float heading,
                           glm::vec2 size,
                           float depth,
                           const TextureData* shape,
                           const PressSettings& settings)
{
  size = glm::max(size, glm::vec2(m_texel));
  if (depth <= 0.0f)
    return;
  const glm::vec2 along(std::cos(heading), std::sin(heading));
  const glm::vec2 across(-along.y, along.x);
  const glm::vec2 half =
      glm::abs(along) * size.x * 0.5f + glm::abs(across) * size.y * 0.5f;
  Footprint footprint;
  footprint.from = texelOf(center - half);
  footprint.size = texelOf(center + half) - footprint.from + 1;
  footprint.target.assign(
      std::size_t(footprint.size.x) * footprint.size.y, kNone);
  const float surface = sample(center);
  for (int y = 0; y < footprint.size.y; ++y)
    for (int x = 0; x < footprint.size.x; ++x)
    {
      const glm::vec2 offset =
          centerOf(footprint.from + glm::ivec2(x, y)) - center;
      // Across its length and width, -0.5 to 0.5.
      const glm::vec2 local(
          glm::dot(offset, along) / size.x, glm::dot(offset, across) / size.y);
      if (std::abs(local.x) > 0.5f || std::abs(local.y) > 0.5f)
        continue;
      float deep = 0.0f;
      if (shape)
        // Its top ahead, its right to the heading's right.
        deep = redAt(*shape, glm::vec2(0.5f - local.y, 0.5f - local.x));
      else
      {
        const float r = 4.0f * glm::dot(local, local);
        deep = r < 1.0f ? std::sqrt(1.0f - r) : 0.0f;
      }
      if (deep > 0.02f)
        footprint.target[std::size_t(y) * footprint.size.x + x] =
            surface - depth * deep;
    }
  press(footprint, settings);
}

void DeformationMap::relax(float smooth, float fade)
{
  smooth = std::clamp(smooth, 0.0f, 1.0f);
  fade = std::clamp(fade, 0.0f, 1.0f);
  if ((smooth <= 0.0f && fade <= 0.0f) || m_tiles.empty())
    return;
  std::unordered_map<int64_t, Tile> next;
  next.reserve(m_tiles.size());
  for (const auto& [key, tile] : m_tiles)
  {
    const glm::ivec2 corner = tileOfKey(key) * kTile;
    Tile out(kTileTexels);
    for (int y = 0; y < kTile; ++y)
      for (int x = 0; x < kTile; ++x)
      {
        const bool inner = x > 0 && y > 0 && x < kTile - 1 && y < kTile - 1;
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            sum += inner ? tile[(y + dy) * kTile + x + dx]
                         : at(corner + glm::ivec2(x + dx, y + dy));
        const float here = tile[y * kTile + x];
        out[y * kTile + x] = glm::mix(here, sum / 9.0f, smooth) * (1.0f - fade);
      }
    next.emplace(key, std::move(out));
  }
  m_tiles = std::move(next);
  std::vector<glm::ivec2> all;
  for (const auto& [key, tile] : m_tiles)
  {
    m_changed.insert(key);
    all.push_back(tileOfKey(key));
  }
  dropFlat(all);
}

void DeformationMap::clear(glm::vec2 low, glm::vec2 high)
{
  const glm::vec2 corners[4] = {low, {high.x, low.y}, high, {low.x, high.y}};
  clear(corners);
}

void DeformationMap::clear(std::span<const glm::vec2> polygon)
{
  if (polygon.size() < 3)
    return;
  glm::vec2 low = polygon[0];
  glm::vec2 high = polygon[0];
  for (const glm::vec2& p : polygon)
  {
    low = glm::min(low, p);
    high = glm::max(high, p);
  }
  const auto inside = [&](glm::vec2 p)
  {
    bool in = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    {
      const glm::vec2 a = polygon[i];
      const glm::vec2 b = polygon[j];
      if ((a.y > p.y) != (b.y > p.y) &&
          p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
        in = !in;
    }
    return in;
  };
  const glm::ivec2 from = tileOf(texelOf(low));
  const glm::ivec2 to = tileOf(texelOf(high));
  std::vector<glm::ivec2> touched;
  for (int ty = from.y; ty <= to.y; ++ty)
    for (int tx = from.x; tx <= to.x; ++tx)
    {
      Tile* tile = tileAt({tx, ty});
      if (!tile)
        continue;
      bool any = false;
      for (int y = 0; y < kTile; ++y)
        for (int x = 0; x < kTile; ++x)
        {
          float& value = (*tile)[y * kTile + x];
          if (value != 0.0f &&
              inside(centerOf(glm::ivec2(tx, ty) * kTile + glm::ivec2(x, y))))
          {
            value = 0.0f;
            any = true;
          }
        }
      if (any)
      {
        m_changed.insert(keyOf({tx, ty}));
        touched.push_back({tx, ty});
      }
    }
  dropFlat(touched);
}

void DeformationMap::clearAll()
{
  for (const auto& [key, tile] : m_tiles)
    m_changed.insert(key);
  m_tiles.clear();
  m_deepest = 0.0f;
}

void DeformationMap::dropFlat(std::span<const glm::ivec2> tiles)
{
  for (const glm::ivec2 tile : tiles)
  {
    const auto it = m_tiles.find(keyOf(tile));
    if (it == m_tiles.end())
      continue;
    if (std::all_of(it->second.begin(),
                    it->second.end(),
                    [](float v) { return std::abs(v) < kFlat; }))
      m_tiles.erase(it);
  }
  if (m_tiles.empty())
    m_deepest = 0.0f;
}

std::vector<glm::ivec2> DeformationMap::takeChanged()
{
  std::vector<glm::ivec2> out;
  out.reserve(m_changed.size());
  for (const int64_t key : m_changed)
    out.push_back(tileOfKey(key));
  m_changed.clear();
  return out;
}

std::vector<glm::ivec2> DeformationMap::tiles() const
{
  std::vector<glm::ivec2> out;
  out.reserve(m_tiles.size());
  for (const auto& [key, tile] : m_tiles)
    out.push_back(tileOfKey(key));
  return out;
}

void DeformationMap::halves(glm::ivec2 tile, std::span<uint16_t> out) const
{
  const Tile* from = tileAt(tile);
  const std::size_t count = std::min<std::size_t>(out.size(), kTileTexels);
  if (!from)
  {
    std::fill(out.begin(), out.begin() + count, uint16_t(0));
    return;
  }
  for (std::size_t i = 0; i < count; ++i)
    out[i] = toHalf((*from)[i]);
}

std::vector<float>
DeformationMap::lowestOver(glm::vec2 origin, float texel, glm::ivec2 size) const
{
  std::vector<float> out(
      std::size_t(std::max(size.x, 0)) * std::max(size.y, 0), 0.0f);
  if (out.empty() || texel <= 0.0f)
    return out;
  for (const auto& [key, tile] : m_tiles)
  {
    const glm::ivec2 corner = tileOfKey(key) * kTile;
    for (int y = 0; y < kTile; ++y)
      for (int x = 0; x < kTile; ++x)
      {
        const float value = tile[y * kTile + x];
        if (value >= 0.0f)
          continue;
        const glm::ivec2 at(
            glm::floor((centerOf(corner + glm::ivec2(x, y)) - origin) / texel));
        if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y)
          continue;
        float& lowest = out[std::size_t(at.y) * size.x + at.x];
        lowest = std::min(lowest, value);
      }
  }
  return out;
}

std::vector<uint8_t> DeformationMap::save() const
{
  std::vector<uint8_t> out(std::begin(kMagic), std::end(kMagic));
  out.push_back(kVersion);
  putFloat(out, m_texel);
  putFloat(out, m_deepest);
  putVarint(out, m_tiles.size());
  for (const auto& [key, tile] : m_tiles)
  {
    const glm::ivec2 at = tileOfKey(key);
    putSigned(out, at.x);
    putSigned(out, at.y);
    // Millimetres, as runs: how many flat, then how many not, each of those
    // as its step from the one before.
    int i = 0;
    int64_t last = 0;
    while (i < kTileTexels)
    {
      const auto mm = [&](int k)
      { return static_cast<int64_t>(std::lround(tile[k] * 1000.0f)); };
      int flat = 0;
      while (i + flat < kTileTexels && mm(i + flat) == 0)
        ++flat;
      int values = 0;
      while (i + flat + values < kTileTexels && mm(i + flat + values) != 0)
        ++values;
      putVarint(out, static_cast<uint64_t>(flat));
      putVarint(out, static_cast<uint64_t>(values));
      for (int k = i + flat; k < i + flat + values; ++k)
      {
        putSigned(out, mm(k) - last);
        last = mm(k);
      }
      i += flat + values;
    }
  }
  return out;
}

bool DeformationMap::load(std::span<const uint8_t> bytes)
{
  m_tiles.clear();
  m_deepest = 0.0f;
  m_changed.clear();
  if (bytes.size() < 5 ||
      !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin()) ||
      bytes[4] != kVersion)
    return false;
  Reader in{bytes, 5};
  const float texel = in.number();
  const float deepest = in.number();
  const uint64_t tiles = in.varint();
  if (!in.ok || texel <= 0.0f || tiles > (uint64_t(1) << 24))
    return false;
  std::unordered_map<int64_t, Tile> read;
  for (uint64_t t = 0; t < tiles && in.ok; ++t)
  {
    glm::ivec2 at;
    at.x = static_cast<int>(in.signedValue());
    at.y = static_cast<int>(in.signedValue());
    Tile tile(kTileTexels, 0.0f);
    int i = 0;
    int64_t last = 0;
    while (i < kTileTexels && in.ok)
    {
      const uint64_t flat = in.varint();
      const uint64_t values = in.varint();
      if (flat + values == 0 || i + flat + values > uint64_t(kTileTexels))
      {
        in.ok = false;
        break;
      }
      i += static_cast<int>(flat);
      for (uint64_t k = 0; k < values; ++k)
      {
        last += in.signedValue();
        tile[i++] = static_cast<float>(last) / 1000.0f;
      }
    }
    read.emplace(keyOf(at), std::move(tile));
  }
  if (!in.ok)
    return false;
  m_texel = texel;
  m_deepest = std::min(deepest, 0.0f);
  m_tiles = std::move(read);
  for (const auto& [key, tile] : m_tiles)
    m_changed.insert(key);
  return true;
}

// --- GroundSurface
// ------------------------------------------------------------

GroundSurface::GroundSurface(std::span<const glm::vec3> positions,
                             std::span<const uint32_t> indices,
                             std::span<const glm::vec2> uvs,
                             const glm::mat4& model)
{
  const bool hasUvs = uvs.size() == positions.size();
  glm::vec2 low(std::numeric_limits<float>::max());
  glm::vec2 high(std::numeric_limits<float>::lowest());
  for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
  {
    const uint32_t i[3] = {indices[t], indices[t + 1], indices[t + 2]};
    if (i[0] >= positions.size() || i[1] >= positions.size() ||
        i[2] >= positions.size())
      continue;
    Triangle triangle;
    for (int k = 0; k < 3; ++k)
    {
      triangle.p[k] = glm::vec3(model * glm::vec4(positions[i[k]], 1.0f));
      triangle.uv[k] = hasUvs ? uvs[i[k]] : glm::vec2(0.0f);
    }
    // Ground only: facing up enough, not walls (as the puddles take it).
    const glm::vec3 normal = glm::cross(
        triangle.p[1] - triangle.p[0], triangle.p[2] - triangle.p[0]);
    if (std::abs(normal.z) < 1e-8f ||
        std::abs(normal.z) < 0.2f * glm::length(normal))
      continue;
    for (const glm::vec3& p : triangle.p)
    {
      low = glm::min(low, glm::vec2(p));
      high = glm::max(high, glm::vec2(p));
    }
    m_triangles.push_back(triangle);
  }
  if (m_triangles.empty())
    return;
  m_low = low;
  m_high = high;
  // Cells of about a triangle each, at most 512 a side.
  const glm::vec2 extent = glm::max(high - low, glm::vec2(1e-3f));
  m_cell = std::max(
      std::sqrt(extent.x * extent.y / static_cast<float>(m_triangles.size())),
      std::max(extent.x, extent.y) / 512.0f);
  m_cell = std::max(m_cell, 0.25f);
  m_cells = glm::max(glm::ivec2(glm::ceil(extent / m_cell)), glm::ivec2(1));
  m_buckets.assign(std::size_t(m_cells.x) * m_cells.y, {});
  for (uint32_t t = 0; t < m_triangles.size(); ++t)
  {
    const Triangle& triangle = m_triangles[t];
    glm::vec2 a = glm::vec2(triangle.p[0]);
    glm::vec2 b = a;
    for (const glm::vec3& p : triangle.p)
    {
      a = glm::min(a, glm::vec2(p));
      b = glm::max(b, glm::vec2(p));
    }
    const glm::ivec2 from = glm::clamp(
        glm::ivec2(glm::floor((a - low) / m_cell)), glm::ivec2(0), m_cells - 1);
    const glm::ivec2 to = glm::clamp(
        glm::ivec2(glm::floor((b - low) / m_cell)), glm::ivec2(0), m_cells - 1);
    for (int y = from.y; y <= to.y; ++y)
      for (int x = from.x; x <= to.x; ++x)
        m_buckets[std::size_t(y) * m_cells.x + x].push_back(t);
  }
}

std::optional<GroundSurface::Point> GroundSurface::at(glm::vec2 point) const
{
  if (m_triangles.empty())
    return std::nullopt;
  const glm::ivec2 cell(glm::floor((point - m_low) / m_cell));
  if (cell.x < 0 || cell.y < 0 || cell.x >= m_cells.x || cell.y >= m_cells.y)
    return std::nullopt;
  std::optional<Point> best;
  for (const uint32_t t : m_buckets[std::size_t(cell.y) * m_cells.x + cell.x])
  {
    const Triangle& tri = m_triangles[t];
    const glm::vec3* p = tri.p;
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                       (p[1].y - p[0].y) * (p[2].x - p[0].x);
    const auto edge = [&](int a, int b)
    {
      return ((p[b].x - p[a].x) * (point.y - p[a].y) -
              (p[b].y - p[a].y) * (point.x - p[a].x)) /
             area;
    };
    const float w0 = edge(1, 2), w1 = edge(2, 0), w2 = edge(0, 1);
    constexpr float kSlack = -1e-4f; // so shared edges leave no gaps
    if (w0 < kSlack || w1 < kSlack || w2 < kSlack)
      continue;
    const float z = w0 * p[0].z + w1 * p[1].z + w2 * p[2].z;
    if (!best || z > best->height)
      best = Point{z, w0 * tri.uv[0] + w1 * tri.uv[1] + w2 * tri.uv[2]};
  }
  return best;
}

std::vector<glm::vec4>
GroundSurface::raster(glm::vec2 origin, float texel, glm::ivec2 size) const
{
  std::vector<glm::vec4> out(
      std::size_t(std::max(size.x, 0)) * std::max(size.y, 0), glm::vec4(0.0f));
  for (int y = 0; y < size.y; ++y)
    for (int x = 0; x < size.x; ++x)
      if (const std::optional<Point> point =
              at(origin + (glm::vec2(x, y) + 0.5f) * texel))
        out[std::size_t(y) * size.x + x] =
            glm::vec4(point->height, point->uv, 1.0f);
  return out;
}

// --- Undersides -----------------------------------------------------------

Underside underside(std::span<const glm::vec3> positions,
                    std::span<const uint32_t> indices,
                    const glm::mat4& model,
                    float below,
                    float texel)
{
  Underside out;
  if (positions.empty() || texel <= 0.0f)
    return out;
  std::vector<glm::vec3> world(positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
    world[i] = glm::vec3(model * glm::vec4(positions[i], 1.0f));

  // The triangles reaching below, and the texels they're over.
  std::vector<uint32_t> low;
  glm::vec2 from(std::numeric_limits<float>::max());
  glm::vec2 to(std::numeric_limits<float>::lowest());
  for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
  {
    const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
    if (a >= world.size() || b >= world.size() || c >= world.size())
      continue;
    if (std::min({world[a].z, world[b].z, world[c].z}) >= below)
      continue;
    low.push_back(static_cast<uint32_t>(t));
    for (const uint32_t k : {a, b, c})
    {
      from = glm::min(from, glm::vec2(world[k]));
      to = glm::max(to, glm::vec2(world[k]));
    }
  }
  if (low.empty())
    return out;
  out.from = glm::ivec2(glm::floor(from / texel));
  out.size = glm::ivec2(glm::floor(to / texel)) - out.from + 1;
  // Laid at four times the resolution (sixteen samples a texel), then each
  // texel's averaged.
  const float half = texel / static_cast<float>(kSamples);
  const glm::ivec2 fine = out.size * kSamples;
  std::vector<float> samples(std::size_t(fine.x) * fine.y, kNone);
  const auto lower = [&](glm::ivec2 sample, float z)
  {
    const glm::ivec2 at = sample - out.from * kSamples;
    if (at.x < 0 || at.y < 0 || at.x >= fine.x || at.y >= fine.y)
      return;
    float& bottom = samples[std::size_t(at.y) * fine.x + at.x];
    if (std::isnan(bottom) || z < bottom)
      bottom = z;
  };

  for (const uint32_t t : low)
  {
    const glm::vec3 p[3] = {
        world[indices[t]], world[indices[t + 1]], world[indices[t + 2]]};
    // Its corners, so thin parts (a tine's edge, seen from below) still
    // show.
    for (const glm::vec3& corner : p)
      if (corner.z < below)
        lower(glm::ivec2(glm::floor(glm::vec2(corner) / half)), corner.z);
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                       (p[1].y - p[0].y) * (p[2].x - p[0].x);
    if (std::abs(area) < 1e-10f)
      continue;
    const glm::vec2 a =
        glm::min(glm::vec2(p[0]), glm::min(glm::vec2(p[1]), glm::vec2(p[2])));
    const glm::vec2 b =
        glm::max(glm::vec2(p[0]), glm::max(glm::vec2(p[1]), glm::vec2(p[2])));
    const glm::ivec2 first(glm::floor(a / half - 0.5f));
    const glm::ivec2 last(glm::ceil(b / half - 0.5f));
    for (int y = first.y; y <= last.y; ++y)
      for (int x = first.x; x <= last.x; ++x)
      {
        const glm::vec2 c = (glm::vec2(x, y) + 0.5f) * half;
        const auto edge = [&](int i, int j)
        {
          return ((p[j].x - p[i].x) * (c.y - p[i].y) -
                  (p[j].y - p[i].y) * (c.x - p[i].x)) /
                 area;
        };
        const float w0 = edge(1, 2), w1 = edge(2, 0), w2 = edge(0, 1);
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
          continue;
        lower({x, y}, w0 * p[0].z + w1 * p[1].z + w2 * p[2].z);
      }
  }

  out.bottom.assign(std::size_t(out.size.x) * out.size.y, kNone);
  out.coverage.assign(out.bottom.size(), 0.0f);
  for (int y = 0; y < out.size.y; ++y)
    for (int x = 0; x < out.size.x; ++x)
    {
      float sum = 0.0f;
      int covered = 0;
      for (int k = 0; k < kSamples * kSamples; ++k)
      {
        const float z =
            samples[std::size_t(kSamples * y + k / kSamples) * fine.x +
                    kSamples * x + k % kSamples];
        if (std::isnan(z))
          continue;
        sum += z;
        ++covered;
      }
      if (covered == 0)
        continue;
      const std::size_t i = std::size_t(y) * out.size.x + x;
      out.bottom[i] = sum / static_cast<float>(covered);
      out.coverage[i] =
          static_cast<float>(covered) / static_cast<float>(kSamples * kSamples);
    }
  return out;
}

Underside merged(const Underside& a, const Underside& b)
{
  if (a.empty())
    return b;
  if (b.empty())
    return a;
  Underside out;
  out.from = glm::min(a.from, b.from);
  out.size = glm::max(a.from + a.size, b.from + b.size) - out.from;
  out.bottom.assign(std::size_t(out.size.x) * out.size.y, kNone);
  out.coverage.assign(out.bottom.size(), 0.0f);
  for (const Underside* part : {&a, &b})
    for (int y = 0; y < part->size.y; ++y)
      for (int x = 0; x < part->size.x; ++x)
      {
        const std::size_t from = std::size_t(y) * part->size.x + x;
        const float z = part->bottom[from];
        if (std::isnan(z))
          continue;
        const glm::ivec2 at = part->from + glm::ivec2(x, y) - out.from;
        const std::size_t to = std::size_t(at.y) * out.size.x + at.x;
        if (std::isnan(out.bottom[to]) || z < out.bottom[to])
          out.bottom[to] = z;
        if (from < part->coverage.size())
          out.coverage[to] = std::max(out.coverage[to], part->coverage[from]);
      }
  return out;
}

uint16_t toHalf(float value) { return glm::packHalf1x16(value); }

} // namespace sfs
