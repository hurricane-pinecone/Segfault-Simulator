#include "engine/core/rendering/terrain.h"

#include "engine/core/rendering/mesh.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

std::array<float, kMaxTerrainLayers> terrainWeights(const glm::vec4& splat)
{
  const glm::vec4 painted = glm::clamp(splat, glm::vec4(0.0f), glm::vec4(1.0f));
  const float sum = painted.x + painted.y + painted.z + painted.w;
  const glm::vec4 shared = sum > 1.0f ? painted / sum : painted;
  return {std::max(1.0f - std::min(sum, 1.0f), 0.0f),
          shared.x,
          shared.y,
          shared.z,
          shared.w};
}

glm::vec4 sampleSplat(const TextureData& splat, glm::vec2 uv)
{
  if (splat.empty() ||
      splat.rgba.size() < std::size_t(splat.width) * splat.height * 4)
    return glm::vec4(0.0f);
  const glm::vec2 size(splat.width, splat.height);
  const glm::vec2 at =
      glm::clamp(uv * size - 0.5f, glm::vec2(0.0f), size - 1.0f);
  const glm::ivec2 low(glm::floor(at));
  const glm::ivec2 high =
      glm::min(low + 1, glm::ivec2(splat.width - 1, splat.height - 1));
  const glm::vec2 f = at - glm::vec2(low);
  const auto texel = [&](int x, int y)
  {
    const uint8_t* p = &splat.rgba[(std::size_t(y) * splat.width + x) * 4];
    return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
  };
  return glm::mix(glm::mix(texel(low.x, low.y), texel(high.x, low.y), f.x),
                  glm::mix(texel(low.x, high.y), texel(high.x, high.y), f.x),
                  f.y);
}

float terrainGrass(const TerrainMaterial& terrain, glm::vec2 uv)
{
  if (terrain.layers.empty())
    return 1.0f;
  const std::array<float, kMaxTerrainLayers> weights = terrainWeights(
      terrain.splat ? sampleSplat(*terrain.splat, uv) : glm::vec4(0.0f));
  float grass = 0.0f;
  for (std::size_t i = 0; i < weights.size(); ++i)
  {
    // A channel with no layer of its own shows the first.
    const TerrainLayer& layer =
        terrain.layers[i < terrain.layers.size() ? i : 0];
    grass += weights[i] * std::clamp(layer.grass, 0.0f, 1.0f);
  }
  return grass;
}

bool terrainLimitsGrass(const TerrainMaterial& terrain)
{
  return std::any_of(terrain.layers.begin(),
                     terrain.layers.end(),
                     [](const TerrainLayer& layer)
                     { return layer.grass < 1.0f; });
}

std::pair<glm::ivec2, glm::ivec2> paintSplat(TextureData& splat,
                                             glm::vec2 center,
                                             glm::vec2 radius,
                                             int layer,
                                             float strength,
                                             float softness)
{
  const std::pair<glm::ivec2, glm::ivec2> none{{0, 0}, {0, 0}};
  if (splat.empty() || layer < 0 ||
      layer >= static_cast<int>(kMaxTerrainLayers) || radius.x <= 0.0f ||
      radius.y <= 0.0f ||
      splat.rgba.size() < std::size_t(splat.width) * splat.height * 4)
    return none;
  const glm::vec2 size(splat.width, splat.height);
  const glm::vec2 middle = center * size;
  const glm::vec2 reach = radius * size;
  const glm::ivec2 from = glm::clamp(
      glm::ivec2(glm::floor(middle - reach)), glm::ivec2(0), glm::ivec2(size));
  const glm::ivec2 to = glm::clamp(glm::ivec2(glm::ceil(middle + reach)) + 1,
                                   glm::ivec2(0),
                                   glm::ivec2(size));
  if (to.x <= from.x || to.y <= from.y)
    return none;

  // Where painting is full, inside; easing out over the soft rim.
  const float inner = 1.0f - std::clamp(softness, 0.0f, 1.0f);
  const float amount = std::clamp(strength, 0.0f, 1.0f);
  glm::vec4 target(0.0f);
  if (layer > 0)
    target[layer - 1] = 1.0f;
  bool changed = false;
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x)
    {
      const glm::vec2 offset = (glm::vec2(x, y) + 0.5f - middle) / reach;
      const float distance = glm::length(offset);
      if (distance >= 1.0f)
        continue;
      float weight = 1.0f;
      if (distance > inner)
      {
        const float t = (distance - inner) / std::max(1.0f - inner, 1e-4f);
        weight = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      weight *= amount;
      uint8_t* p = &splat.rgba[(std::size_t(y) * splat.width + x) * 4];
      std::array<int, 4> next{};
      for (int c = 0; c < 4; ++c)
      {
        const float value = p[c] / 255.0f;
        next[c] = static_cast<int>(
            std::lround((value + (target[c] - value) * weight) * 255.0f));
        // Always at least a step toward the target, so slow strokes still
        // arrive.
        if (next[c] == p[c] && weight > 0.0f && target[c] * 255.0f != p[c])
          next[c] += target[c] > value ? 1 : -1;
        next[c] = std::clamp(next[c], 0, 255);
      }
      // Never past one in all (rounding): what's over off the others.
      int over = next[0] + next[1] + next[2] + next[3] - 255;
      for (int c = 0; c < 4 && over > 0; ++c)
        if (target[c] == 0.0f)
        {
          const int taken = std::min(over, next[c]);
          next[c] -= taken;
          over -= taken;
        }
      for (int c = 0; c < 4; ++c)
      {
        changed = changed || next[c] != p[c];
        p[c] = static_cast<uint8_t>(next[c]);
      }
    }
  return changed ? std::pair{from, to} : none;
}

std::shared_ptr<TextureData> blankSplat(int size)
{
  auto splat = std::make_shared<TextureData>();
  splat->width = splat->height = static_cast<uint32_t>(std::max(size, 1));
  splat->rgba.assign(std::size_t(splat->width) * splat->height * 4, 0);
  splat->linear = true;
  return splat;
}

namespace
{

void read(const nlohmann::json& json, const char* key, float& out)
{
  if (const auto it = json.find(key); it != json.end() && it->is_number())
    out = it->get<float>();
}

void read(const nlohmann::json& json, const char* key, std::string& out)
{
  if (const auto it = json.find(key); it != json.end() && it->is_string())
    out = it->get<std::string>();
}

} // namespace

nlohmann::json TerrainAsset::toJson() const
{
  nlohmann::json out = {{"splatMap", splatMap},
                        {"blendSharpness", blendSharpness},
                        {"antiTiling", antiTiling},
                        {"variation", variation}};
  nlohmann::json saved = nlohmann::json::array();
  for (const TerrainLayerAsset& layer : layers)
    saved.push_back({{"name", layer.name},
                     {"baseTexture", layer.baseTexture},
                     {"normalTexture", layer.normalTexture},
                     {"roughnessTexture", layer.roughnessTexture},
                     {"tint", {layer.tint.x, layer.tint.y, layer.tint.z}},
                     {"tileSize", layer.tileSize},
                     {"roughness", layer.roughness},
                     {"normalScale", layer.normalScale},
                     {"grass", layer.grass}});
  out["layers"] = std::move(saved);
  return out;
}

TerrainAsset TerrainAsset::fromJson(const nlohmann::json& json)
{
  TerrainAsset out;
  if (!json.is_object())
    return out;
  read(json, "splatMap", out.splatMap);
  read(json, "blendSharpness", out.blendSharpness);
  read(json, "variation", out.variation);
  if (const auto it = json.find("antiTiling");
      it != json.end() && it->is_boolean())
    out.antiTiling = it->get<bool>();
  if (const auto it = json.find("layers"); it != json.end() && it->is_array())
    for (const nlohmann::json& item : *it)
    {
      if (!item.is_object() || out.layers.size() >= kMaxTerrainLayers)
        continue;
      TerrainLayerAsset layer;
      read(item, "name", layer.name);
      read(item, "baseTexture", layer.baseTexture);
      read(item, "normalTexture", layer.normalTexture);
      read(item, "roughnessTexture", layer.roughnessTexture);
      if (const auto tint = item.find("tint");
          tint != item.end() && tint->is_array() && tint->size() == 3)
        for (int i = 0; i < 3; ++i)
          if ((*tint)[i].is_number())
            layer.tint[i] = (*tint)[i].get<float>();
      read(item, "tileSize", layer.tileSize);
      read(item, "roughness", layer.roughness);
      read(item, "normalScale", layer.normalScale);
      read(item, "grass", layer.grass);
      out.layers.push_back(std::move(layer));
    }
  return out;
}

} // namespace sfs
