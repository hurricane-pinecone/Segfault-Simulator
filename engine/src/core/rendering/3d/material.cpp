#include "engine/core/rendering/3d/material.h"

#include "engine/core/rendering/3d/materialNodes.h"

namespace sfs
{

namespace
{
template <typename T>
void read(const nlohmann::json& json, const char* key, T& out)
{
  if (const auto it = json.find(key); it != json.end())
  {
    if constexpr (std::is_same_v<T, std::string>)
    {
      if (it->is_string())
        out = it->get<std::string>();
    }
    else if (it->is_number())
      out = it->get<T>();
  }
}
} // namespace

const char* alphaModeName(AlphaMode mode)
{
  switch (mode)
  {
  case AlphaMode::Mask:
    return "mask";
  case AlphaMode::Blend:
    return "blend";
  case AlphaMode::Opaque:
    break;
  }
  return "opaque";
}

std::optional<AlphaMode> alphaModeFromName(std::string_view name)
{
  for (const AlphaMode mode :
       {AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend})
    if (name == alphaModeName(mode))
      return mode;
  return std::nullopt;
}

materialGraph::Graph MaterialAsset::toGraph() const
{
  using namespace materialGraph;
  Graph graph;
  const NodeId output = graph.add(kOutputType, {0.0f, 0.0f});
  Node* out = graph.find(output);
  out->inputs[kMetallic].x = metallic;
  out->inputs[kRoughness].x = roughness;
  out->inputs[kAlphaClip].x = alphaCutoff;

  // Colour x vertex colour, as untextured models carry their palette in their
  // vertex colours.
  const NodeId color = graph.add("color", {-700.0f, -60.0f});
  graph.find(color)->value = baseColor;
  const NodeId vertexColor = graph.add("vertexColor", {-700.0f, 60.0f});
  const NodeId tinted = graph.add("multiply", {-470.0f, -20.0f});
  graph.connect(color, 0, tinted, 0);
  graph.connect(vertexColor, 0, tinted, 1);

  // The textures' coordinates: the mesh's, repeated and shifted.
  const bool textured = !baseTexture.empty() ||
                        !metallicRoughnessTexture.empty() ||
                        !normalTexture.empty();
  NodeId uv = 0;
  if (textured && (tiling != glm::vec2(1.0f) || offset != glm::vec2(0.0f)))
  {
    uv = graph.add("tilingOffset", {-1150.0f, 140.0f});
    graph.find(uv)->inputs[1] = glm::vec4(tiling, 0.0f, 0.0f);
    graph.find(uv)->inputs[2] = glm::vec4(offset, 0.0f, 0.0f);
  }
  const auto texture =
      [&](const std::string& name, bool data, glm::vec2 position)
  {
    const NodeId node = graph.add("texture", position);
    graph.find(node)->texture = name;
    graph.find(node)->dataTexture = data;
    if (uv)
      graph.connect(uv, 0, node, 0);
    return node;
  };

  if (baseTexture.empty())
  {
    graph.connect(tinted, 0, output, kBaseColor);
    graph.connect(color, 1, output, kAlpha);
  }
  else
  {
    const NodeId base = texture(baseTexture, false, {-920.0f, 160.0f});
    const NodeId albedo = graph.add("multiply", {-230.0f, -20.0f});
    graph.connect(tinted, 0, albedo, 0);
    graph.connect(base, 1, albedo, 1); // RGB
    graph.connect(albedo, 0, output, kBaseColor);
    const NodeId alpha = graph.add("multiply", {-230.0f, 140.0f});
    graph.connect(color, 1, alpha, 0);
    graph.connect(base, 5, alpha, 1); // A
    graph.connect(alpha, 0, output, kAlpha);
  }

  if (!metallicRoughnessTexture.empty())
  {
    // Metalness in blue, roughness in green, multiplying the numbers.
    const NodeId data =
        texture(metallicRoughnessTexture, true, {-920.0f, 420.0f});
    const auto scaled =
        [&](float value, uint32_t channel, uint32_t input, float y)
    {
      const NodeId number = graph.add("number", {-700.0f, y});
      graph.find(number)->value.x = value;
      const NodeId multiply = graph.add("multiply", {-470.0f, y});
      graph.connect(number, 0, multiply, 0);
      graph.connect(data, channel, multiply, 1);
      graph.connect(multiply, 0, output, input);
    };
    scaled(metallic, 4, kMetallic, 330.0f);   // B
    scaled(roughness, 3, kRoughness, 470.0f); // G
  }

  if (!normalTexture.empty())
  {
    const NodeId normalMap = graph.add("normalMap", {-470.0f, 620.0f});
    graph.find(normalMap)->texture = normalTexture;
    graph.find(normalMap)->inputs[1].x = normalScale; // Strength
    if (uv)
      graph.connect(uv, 0, normalMap, 0);
    graph.connect(normalMap, 0, output, kNormal);
  }
  return graph;
}

nlohmann::json MaterialAsset::toJson() const
{
  if (graph)
    return {
        {"formatVersion", kFormatVersion},
        {"alphaMode", alphaModeName(alphaMode)},
        {"graph", graph->toJson()},
    };
  return {
      {"formatVersion", kFormatVersion},
      {"baseColor", {baseColor.r, baseColor.g, baseColor.b, baseColor.a}},
      {"baseTexture", baseTexture},
      {"metallicRoughnessTexture", metallicRoughnessTexture},
      {"normalTexture", normalTexture},
      {"normalScale", normalScale},
      {"tiling", {tiling.x, tiling.y}},
      {"offset", {offset.x, offset.y}},
      {"metallic", metallic},
      {"roughness", roughness},
      {"alphaMode", alphaModeName(alphaMode)},
      {"alphaCutoff", alphaCutoff},
  };
}

std::optional<MaterialAsset> MaterialAsset::fromJson(const nlohmann::json& json)
{
  if (!json.is_object())
    return std::nullopt;

  MaterialAsset material;
  if (const auto it = json.find("baseColor");
      it != json.end() && it->is_array() && it->size() == 4)
    for (int i = 0; i < 4; ++i)
      if ((*it)[i].is_number())
        material.baseColor[i] = (*it)[i].get<float>();
  const auto readVec2 = [&json](const char* key, glm::vec2& out)
  {
    if (const auto it = json.find(key);
        it != json.end() && it->is_array() && it->size() == 2)
      for (int i = 0; i < 2; ++i)
        if ((*it)[i].is_number())
          out[i] = (*it)[i].get<float>();
  };
  readVec2("tiling", material.tiling);
  readVec2("offset", material.offset);
  read(json, "baseTexture", material.baseTexture);
  read(json, "metallicRoughnessTexture", material.metallicRoughnessTexture);
  read(json, "normalTexture", material.normalTexture);
  read(json, "normalScale", material.normalScale);
  read(json, "metallic", material.metallic);
  read(json, "roughness", material.roughness);
  read(json, "alphaCutoff", material.alphaCutoff);
  if (const auto it = json.find("alphaMode");
      it != json.end() && it->is_string())
    material.alphaMode =
        alphaModeFromName(it->get<std::string>()).value_or(AlphaMode::Opaque);
  if (const auto it = json.find("graph"); it != json.end())
    material.graph = materialGraph::Graph::fromJson(*it);
  return material;
}

} // namespace sfs
