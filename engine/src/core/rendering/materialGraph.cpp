#include "engine/core/rendering/materialGraph.h"

#include "engine/core/rendering/materialNodes.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace sfs::materialGraph
{

namespace
{

nlohmann::json vec4Json(const glm::vec4& v) { return {v.x, v.y, v.z, v.w}; }

// Up to four numbers into `out`; anything else leaves it as it was.
void readVec4(const nlohmann::json& json, glm::vec4& out)
{
  if (!json.is_array())
    return;
  for (std::size_t i = 0; i < std::min<std::size_t>(json.size(), 4); ++i)
    if (json[i].is_number())
      out[static_cast<int>(i)] = json[i].get<float>();
}

// A node id: a whole number above 0 (saved unsigned, but hand-written JSON's
// numbers read as signed).
std::optional<NodeId> readId(const nlohmann::json& json)
{
  if (!json.is_number_integer())
    return std::nullopt;
  const int64_t id = json.get<int64_t>();
  if (id <= 0 || id > std::numeric_limits<NodeId>::max())
    return std::nullopt;
  return static_cast<NodeId>(id);
}

// [nodeId, "pin name"]
std::optional<std::pair<NodeId, std::string>>
readEnd(const nlohmann::json& json)
{
  if (!json.is_array() || json.size() != 2 || !json[1].is_string())
    return std::nullopt;
  const std::optional<NodeId> id = readId(json[0]);
  if (!id)
    return std::nullopt;
  return std::make_pair(*id, json[1].get<std::string>());
}

} // namespace

Node* Graph::find(NodeId id)
{
  const auto it =
      std::find_if(nodes.begin(),
                   nodes.end(),
                   [id](const Node& node) { return node.id == id; });
  return it != nodes.end() ? &*it : nullptr;
}

const Node* Graph::find(NodeId id) const
{
  return const_cast<Graph*>(this)->find(id);
}

NodeId Graph::add(std::string_view type, glm::vec2 position)
{
  const NodeType* nodeType = findNodeType(type);
  if (!nodeType)
    return 0;
  Node node;
  node.id = nextId++;
  node.type = std::string(type);
  node.position = position;
  for (const InputPin& pin : nodeType->inputs)
    node.inputs.push_back(pin.defaultValue);
  node.value = nodeType->propertyDefault;
  nodes.push_back(std::move(node));
  return nodes.back().id;
}

void Graph::remove(NodeId id)
{
  std::erase_if(nodes, [id](const Node& node) { return node.id == id; });
  std::erase_if(links,
                [id](const Link& link)
                { return link.fromNode == id || link.toNode == id; });
}

bool Graph::connect(NodeId from, uint32_t output, NodeId to, uint32_t input)
{
  const Node* source = find(from);
  const Node* destination = find(to);
  if (!source || !destination || from == to)
    return false;
  const NodeType* sourceType = findNodeType(source->type);
  const NodeType* destinationType = findNodeType(destination->type);
  if (!sourceType || !destinationType || output >= sourceType->outputs.size() ||
      input >= destinationType->inputs.size())
    return false;
  // `to` already feeding `from` would make a loop.
  if (feeds(to, from))
    return false;
  disconnect(to, input);
  links.push_back({from, output, to, input});
  return true;
}

void Graph::disconnect(NodeId to, uint32_t input)
{
  std::erase_if(links,
                [&](const Link& link)
                { return link.toNode == to && link.toInput == input; });
}

const Link* Graph::linkInto(NodeId to, uint32_t input) const
{
  const auto it =
      std::find_if(links.begin(),
                   links.end(),
                   [&](const Link& link)
                   { return link.toNode == to && link.toInput == input; });
  return it != links.end() ? &*it : nullptr;
}

bool Graph::feeds(NodeId from, NodeId to) const
{
  std::vector<NodeId> pending = {from};
  std::unordered_set<NodeId> seen = {from};
  while (!pending.empty())
  {
    const NodeId node = pending.back();
    pending.pop_back();
    for (const Link& link : links)
      if (link.fromNode == node)
      {
        if (link.toNode == to)
          return true;
        if (seen.insert(link.toNode).second)
          pending.push_back(link.toNode);
      }
  }
  return false;
}

NodeId Graph::output() const
{
  for (const Node& node : nodes)
    if (node.type == kOutputType)
      return node.id;
  return 0;
}

Graph Graph::makeDefault()
{
  Graph graph;
  const NodeId output = graph.add(kOutputType, {0.0f, 0.0f});
  const NodeId color = graph.add("color", {-460.0f, -40.0f});
  const NodeId vertexColor = graph.add("vertexColor", {-460.0f, 90.0f});
  const NodeId multiply = graph.add("multiply", {-230.0f, 0.0f});
  graph.connect(color, 0, multiply, 0);
  graph.connect(vertexColor, 0, multiply, 1);
  graph.connect(multiply, 0, output, kBaseColor);
  return graph;
}

nlohmann::json Graph::toJson() const
{
  nlohmann::json jsonNodes = nlohmann::json::array();
  for (const Node& node : nodes)
  {
    nlohmann::json json = {{"id", node.id},
                           {"type", node.type},
                           {"position", {node.position.x, node.position.y}}};
    const NodeType* type = findNodeType(node.type);
    if (type)
    {
      // Only the inputs that hold a value of their own.
      nlohmann::json inputs = nlohmann::json::object();
      for (std::size_t i = 0; i < type->inputs.size() && i < node.inputs.size();
           ++i)
        if (type->inputs[i].fallback == Fallback::Value)
          inputs[type->inputs[i].name] = vec4Json(node.inputs[i]);
      if (!inputs.empty())
        json["inputs"] = std::move(inputs);
      if (type->property == Property::Color ||
          type->property == Property::Number)
        json["value"] = vec4Json(node.value);
      if (type->property == Property::Texture)
      {
        json["texture"] = node.texture;
        json["dataTexture"] = node.dataTexture;
      }
    }
    jsonNodes.push_back(std::move(json));
  }

  nlohmann::json jsonLinks = nlohmann::json::array();
  for (const Link& link : links)
  {
    const Node* from = find(link.fromNode);
    const Node* to = find(link.toNode);
    const NodeType* fromType = from ? findNodeType(from->type) : nullptr;
    const NodeType* toType = to ? findNodeType(to->type) : nullptr;
    if (!fromType || !toType || link.fromOutput >= fromType->outputs.size() ||
        link.toInput >= toType->inputs.size())
      continue;
    jsonLinks.push_back(
        {{"from", {link.fromNode, fromType->outputs[link.fromOutput].name}},
         {"to", {link.toNode, toType->inputs[link.toInput].name}}});
  }

  return {{"nodes", std::move(jsonNodes)}, {"links", std::move(jsonLinks)}};
}

std::optional<Graph> Graph::fromJson(const nlohmann::json& json)
{
  if (!json.is_object())
    return std::nullopt;

  Graph graph;
  if (const auto it = json.find("nodes"); it != json.end() && it->is_array())
    for (const nlohmann::json& entry : *it)
    {
      if (!entry.is_object())
        continue;
      const auto id = entry.find("id");
      const auto typeName = entry.find("type");
      if (id == entry.end() || typeName == entry.end() ||
          !typeName->is_string())
        continue;
      Node node;
      node.id = readId(*id).value_or(0);
      if (node.id == 0 || graph.find(node.id))
        continue; // ids are unique, and never 0
      node.type = typeName->get<std::string>();
      if (const auto position = entry.find("position");
          position != entry.end() && position->is_array() &&
          position->size() == 2 && (*position)[0].is_number() &&
          (*position)[1].is_number())
        node.position = {
            (*position)[0].get<float>(), (*position)[1].get<float>()};

      if (const NodeType* type = findNodeType(node.type))
      {
        for (const InputPin& pin : type->inputs)
          node.inputs.push_back(pin.defaultValue);
        if (const auto inputs = entry.find("inputs");
            inputs != entry.end() && inputs->is_object())
          for (const auto& [name, value] : inputs->items())
            if (const auto index = type->findInput(name))
              readVec4(value, node.inputs[*index]);
        node.value = type->propertyDefault;
        if (const auto value = entry.find("value"); value != entry.end())
          readVec4(*value, node.value);
        if (const auto texture = entry.find("texture");
            texture != entry.end() && texture->is_string())
          node.texture = texture->get<std::string>();
        if (const auto data = entry.find("dataTexture");
            data != entry.end() && data->is_boolean())
          node.dataTexture = data->get<bool>();
      }
      graph.nextId = std::max(graph.nextId, node.id + 1);
      graph.nodes.push_back(std::move(node));
    }

  if (const auto it = json.find("links"); it != json.end() && it->is_array())
    for (const nlohmann::json& entry : *it)
    {
      if (!entry.is_object() || !entry.contains("from") ||
          !entry.contains("to"))
        continue;
      const auto from = readEnd(entry["from"]);
      const auto to = readEnd(entry["to"]);
      if (!from || !to)
        continue;
      const Node* source = graph.find(from->first);
      const Node* destination = graph.find(to->first);
      const NodeType* sourceType =
          source ? findNodeType(source->type) : nullptr;
      const NodeType* destinationType =
          destination ? findNodeType(destination->type) : nullptr;
      if (!sourceType || !destinationType)
        continue;
      const auto output = sourceType->findOutput(from->second);
      const auto input = destinationType->findInput(to->second);
      // A second link into an input, or one making a loop, isn't kept.
      if (output && input && !graph.linkInto(to->first, *input))
        graph.connect(from->first, *output, to->first, *input);
    }

  return graph;
}

} // namespace sfs::materialGraph
