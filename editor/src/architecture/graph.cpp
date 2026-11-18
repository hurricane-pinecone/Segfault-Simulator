#include "architecture/graph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace architecture
{

namespace
{

constexpr std::array<std::string_view, NODE_KINDS> NODE_KIND_NAMES = {
    "system",
    "service",
    "gameObject",
    "component",
    "sceneData",
    "event",
    "scriptableObject",
    "meshSource",
    "utils",
};

constexpr std::array<std::string_view, EDGE_KINDS> EDGE_KIND_NAMES = {
    "registers",
    "observes",
    "reads",
    "writes",
    "adds",
    "removes",
    "filters",
    "sends",
    "consumes",
    "requests",
    "responds",
    "reads-system",
    "uses-util",
};

template <typename Kind, std::size_t N>
std::optional<Kind> kindNamed(const std::array<std::string_view, N>& names,
                              std::string_view name);

bool boolField(const nlohmann::json& object, const char* key);

std::string comparable(std::string_view path);

int intField(const nlohmann::json& object, const char* key);

std::optional<Graph> readGraph(const nlohmann::json& root, std::string& error);

std::string stringField(const nlohmann::json& object, const char* key);

std::string_view withoutExtension(std::string_view path);

} // namespace

std::optional<uint32_t> Graph::find(std::string_view id) const
{
  const auto it = std::lower_bound(nodes.begin(),
                                   nodes.end(),
                                   id,
                                   [](const Node& node, std::string_view key)
                                   { return node.id < key; });

  if (it == nodes.end() || it->id != id)
    return std::nullopt;

  return static_cast<uint32_t>(it - nodes.begin());
}

std::string_view edgeKindName(EdgeKind kind)
{
  return EDGE_KIND_NAMES[static_cast<std::size_t>(kind)];
}

std::string_view nodeKindName(NodeKind kind)
{
  return NODE_KIND_NAMES[static_cast<std::size_t>(kind)];
}

std::optional<uint32_t> nodeOfFile(const Graph& graph, std::string_view path)
{
  // Compared from a slash before each, so "Health.h" doesn't end
  // "scripts/MaxHealth.h"; without extensions, for a .cpp to find its .h.
  const std::string wanted = comparable(path);
  const std::string_view wantedStem = withoutExtension(wanted);
  const std::string_view wantedName =
      wantedStem.substr(wantedStem.find_last_of('/') + 1);
  std::vector<uint8_t> exact(graph.files.size(), 0);
  std::vector<uint8_t> sibling(graph.files.size(), 0);

  for (std::size_t i = 0; i < graph.files.size(); ++i)
  {
    const std::string file = comparable(graph.files[i]);
    exact[i] = wanted.ends_with(file) ? 1 : 0;
    sibling[i] = wantedStem.ends_with(withoutExtension(file)) ? 1 : 0;
  }

  // A type declared there; then a utils node made of it.
  std::optional<uint32_t> declared;
  std::optional<uint32_t> utils;

  for (uint32_t node = 0; node < graph.nodes.size(); ++node)
  {
    const Node& candidate = graph.nodes[node];

    if (candidate.kind != NodeKind::Utils && candidate.file != NO_FILE &&
        sibling[candidate.file])
    {
      if (comparable(candidate.name).substr(1) == wantedName)
        return node;

      if (!declared)
        declared = node;
    }

    if (!utils && candidate.kind == NodeKind::Utils &&
        std::ranges::any_of(
            candidate.files, [&](uint32_t file) { return sibling[file] != 0; }))
      utils = node;
  }

  if (declared)
    return declared;

  if (utils)
    return utils;

  // The node whose code is written there the most.
  std::vector<uint32_t> sites(graph.nodes.size(), 0);

  for (const Edge& edge : graph.edges)
    for (const Site& site : edge.sites)
      sites[edge.from] += exact[site.file];

  const auto most = std::ranges::max_element(sites);

  if (most == sites.end() || *most == 0)
    return std::nullopt;

  return static_cast<uint32_t>(most - sites.begin());
}

std::optional<Graph> parseGraph(std::string_view json, std::string& error)
{
  const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);

  if (root.is_discarded() || !root.is_object())
  {
    error = "The export isn't JSON";
    return std::nullopt;
  }

  try
  {
    return readGraph(root, error);
  }
  catch (const nlohmann::json::exception& exception)
  {
    error = std::string("The export can't be read: ") + exception.what();
    return std::nullopt;
  }
}

std::optional<Progress> parseProgress(std::string_view line)
{
  constexpr std::string_view PREFIX = "progress ";

  if (!line.starts_with(PREFIX))
    return std::nullopt;

  line.remove_prefix(PREFIX.size());
  Progress progress;
  const char* end = line.data() + line.size();
  const auto [doneEnd, doneError] =
      std::from_chars(line.data(), end, progress.done);

  if (doneError != std::errc() || doneEnd == end || *doneEnd != ' ')
    return std::nullopt;

  const auto [totalEnd, totalError] =
      std::from_chars(doneEnd + 1, end, progress.total);

  if (totalError != std::errc() || totalEnd == end || *totalEnd != ' ' ||
      progress.done < 0 || progress.total < 0)
    return std::nullopt;

  progress.phase.assign(totalEnd + 1, end);

  if (progress.phase.empty())
    return std::nullopt;

  return progress;
}

namespace
{

// A true/false field, or false if it's missing or isn't one.
bool boolField(const nlohmann::json& object, const char* key)
{
  const auto it = object.find(key);
  return it != object.end() && it->is_boolean() && it->get<bool>();
}

template <typename Kind, std::size_t N>
std::optional<Kind> kindNamed(const std::array<std::string_view, N>& names,
                              std::string_view name)
{
  const auto it = std::ranges::find(names, name);

  if (it == names.end())
    return std::nullopt;

  return static_cast<Kind>(it - names.begin());
}

// A whole-number field, or 0 if it's missing or isn't one.
int intField(const nlohmann::json& object, const char* key)
{
  const auto it = object.find(key);

  if (it == object.end() || !it->is_number_integer())
    return 0;

  return it->get<int>();
}

// The graph an export's JSON holds.
std::optional<Graph> readGraph(const nlohmann::json& root, std::string& error)
{
  const int version = intField(root, "version");

  if (version != GRAPH_VERSION)
  {
    error = "The export is of schema version " + std::to_string(version) +
            ", and this editor reads version " + std::to_string(GRAPH_VERSION);
    return std::nullopt;
  }

  const auto nodes = root.find("nodes");
  const auto edges = root.find("edges");

  if (nodes == root.end() || !nodes->is_array() || edges == root.end() ||
      !edges->is_array())
  {
    error = "The export has no nodes or edges";
    return std::nullopt;
  }

  Graph graph;
  graph.root = stringField(root, "root");
  std::unordered_map<std::string, uint32_t> fileIndex;
  const auto fileOf = [&](std::string name) -> uint32_t
  {
    const auto [it, added] =
        fileIndex.try_emplace(name, static_cast<uint32_t>(graph.files.size()));

    if (added)
      graph.files.push_back(std::move(name));

    return it->second;
  };

  graph.nodes.reserve(nodes->size());

  for (const nlohmann::json& entry : *nodes)
  {
    if (!entry.is_object())
      continue;

    const std::optional<NodeKind> kind =
        kindNamed<NodeKind>(NODE_KIND_NAMES, stringField(entry, "kind"));
    std::string id = stringField(entry, "id");

    if (!kind || id.empty())
      continue;

    Node& node = graph.nodes.emplace_back();
    node.id = std::move(id);
    node.kind = *kind;
    node.name = stringField(entry, "name");
    node.qualifiedName = stringField(entry, "qualifiedName");
    node.role = stringField(entry, "role");
    node.declared = boolField(entry, "declared");
    node.engine = boolField(entry, "engine");
    node.line = intField(entry, "line");

    if (std::string file = stringField(entry, "file"); !file.empty())
      node.file = fileOf(std::move(file));

    if (const auto files = entry.find("files");
        files != entry.end() && files->is_array())
      for (const nlohmann::json& file : *files)
        if (file.is_string())
          node.files.push_back(fileOf(file.get<std::string>()));

    if (node.name.empty())
      node.name = node.id;
  }

  // Sorted by id, as the export writes them, so find() can search them.
  std::ranges::sort(graph.nodes, {}, &Node::id);
  graph.edges.reserve(edges->size());

  for (const nlohmann::json& entry : *edges)
  {
    if (!entry.is_object())
      continue;

    const std::optional<EdgeKind> kind =
        kindNamed<EdgeKind>(EDGE_KIND_NAMES, stringField(entry, "kind"));
    const std::optional<uint32_t> from = graph.find(stringField(entry, "from"));
    const std::optional<uint32_t> to = graph.find(stringField(entry, "to"));

    if (!kind || !from || !to)
      continue;

    Edge& edge = graph.edges.emplace_back();
    edge.from = *from;
    edge.to = *to;
    edge.kind = *kind;
    const auto sites = entry.find("sites");

    if (sites == entry.end() || !sites->is_array())
      continue;

    edge.sites.reserve(sites->size());

    for (const nlohmann::json& place : *sites)
    {
      if (!place.is_object())
        continue;

      Site& site = edge.sites.emplace_back();
      site.file = fileOf(stringField(place, "file"));
      site.line = intField(place, "line");
      site.via = stringField(place, "via");
      site.term = stringField(place, "term");
    }
  }

  // Each node's edges, counted then filled in.
  const std::size_t count = graph.nodes.size();
  graph.touchingStart.assign(count + 1, 0);

  for (const Edge& edge : graph.edges)
  {
    ++graph.touchingStart[edge.from + 1];

    if (edge.to != edge.from)
      ++graph.touchingStart[edge.to + 1];
  }

  for (std::size_t i = 0; i < count; ++i)
    graph.touchingStart[i + 1] += graph.touchingStart[i];

  graph.touching.resize(graph.touchingStart[count]);
  std::vector<uint32_t> filled(
      graph.touchingStart.begin(), graph.touchingStart.end() - 1);

  for (uint32_t i = 0; i < graph.edges.size(); ++i)
  {
    const Edge& edge = graph.edges[i];
    graph.touching[filled[edge.from]++] = i;

    if (edge.to != edge.from)
      graph.touching[filled[edge.to]++] = i;
  }

  return graph;
}

// A path to compare: forward slashes, lower case, a slash before it.
std::string comparable(std::string_view path)
{
  std::string out = "/";
  out.reserve(path.size() + 1);

  for (const char c : path)
    out += c == '\\'
               ? '/'
               : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  return out;
}

// A string field, or "" if it's missing or isn't one (a null `role`).
std::string stringField(const nlohmann::json& object, const char* key)
{
  const auto it = object.find(key);

  if (it == object.end() || !it->is_string())
    return {};

  return it->get<std::string>();
}

// A path without its file's extension.
std::string_view withoutExtension(std::string_view path)
{
  const std::size_t dot = path.find_last_of('.');
  const std::size_t slash = path.find_last_of('/');
  const bool extension = dot != std::string_view::npos &&
                         (slash == std::string_view::npos || dot > slash);
  return extension ? path.substr(0, dot) : path;
}

} // namespace

} // namespace architecture
