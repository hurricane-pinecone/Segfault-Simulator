#include "architecture/highlight.h"

#include "architecture/graph.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace architecture
{

namespace
{

// How well a name matches a search, as search() ranks it.
enum Rank
{
  WHOLE,
  PREFIX,
  WORD,
  SUBSTRING,
  LETTERS,
};

void byKindThenName(const Graph& graph, std::vector<uint32_t>& nodes);

char lower(char c);

std::optional<int> rankOf(std::string_view name, std::string_view query);

} // namespace

bool Filter::shows(const Graph& graph, const Edge& edge) const
{
  return edgeKinds[static_cast<std::size_t>(edge.kind)] &&
         shows(graph.nodes[edge.from]) && shows(graph.nodes[edge.to]);
}

void light(const Graph& graph,
           const Filter& filter,
           std::span<const uint32_t> nodes,
           std::span<const uint32_t> edges,
           Lit& lit)
{
  lit.nodes.assign(graph.nodes.size(), 0);
  lit.edges.assign(graph.edges.size(), 0);
  lit.active = !nodes.empty() || !edges.empty();

  for (const uint32_t node : nodes)
  {
    if (!filter.shows(graph.nodes[node]))
      continue;

    lit.nodes[node] = 1;

    for (const uint32_t touching : graph.edgesOf(node))
    {
      const Edge& link = graph.edges[touching];

      if (!filter.shows(graph, link))
        continue;

      lit.edges[touching] = 1;
      lit.nodes[link.from] = 1;
      lit.nodes[link.to] = 1;
    }
  }

  for (const uint32_t edge : edges)
  {
    if (edge >= graph.edges.size())
      continue;

    const Edge& link = graph.edges[edge];
    lit.edges[edge] = 1;
    lit.nodes[link.from] = 1;
    lit.nodes[link.to] = 1;
  }
}

void neighbourhood(const Graph& graph,
                   const Filter& filter,
                   uint32_t centre,
                   Neighbourhood& out)
{
  out.centre = centre;
  out.incoming.clear();
  out.outgoing.clear();

  for (const uint32_t touching : graph.edgesOf(centre))
  {
    const Edge& edge = graph.edges[touching];

    if (!filter.shows(graph, edge) || edge.from == edge.to)
      continue;

    if (edge.from == centre)
      out.outgoing.push_back(edge.to);
    else
      out.incoming.push_back(edge.from);
  }

  byKindThenName(graph, out.outgoing);
  out.outgoing.erase(std::unique(out.outgoing.begin(), out.outgoing.end()),
                     out.outgoing.end());
  std::erase_if(
      out.incoming,
      [&](uint32_t node)
      { return std::ranges::find(out.outgoing, node) != out.outgoing.end(); });
  byKindThenName(graph, out.incoming);
  out.incoming.erase(std::unique(out.incoming.begin(), out.incoming.end()),
                     out.incoming.end());
}

void search(const Graph& graph,
            std::string_view query,
            std::size_t limit,
            std::vector<SearchHit>& hits)
{
  hits.clear();

  if (query.empty())
    return;

  for (uint32_t i = 0; i < graph.nodes.size(); ++i)
    if (const std::optional<int> rank = rankOf(graph.nodes[i].name, query))
      hits.push_back({i, *rank});

  const auto order = [&](const SearchHit& a, const SearchHit& b)
  {
    return std::tie(a.rank, graph.nodes[a.node].name, a.node) <
           std::tie(b.rank, graph.nodes[b.node].name, b.node);
  };

  if (hits.size() > limit)
  {
    std::partial_sort(hits.begin(),
                      hits.begin() + static_cast<std::ptrdiff_t>(limit),
                      hits.end(),
                      order);
    hits.resize(limit);
    return;
  }

  std::sort(hits.begin(), hits.end(), order);
}

namespace
{

// Sorted by kind, then name, then index (two nodes may share a name).
void byKindThenName(const Graph& graph, std::vector<uint32_t>& nodes)
{
  std::sort(nodes.begin(),
            nodes.end(),
            [&](uint32_t a, uint32_t b)
            {
              return std::tie(graph.nodes[a].kind, graph.nodes[a].name, a) <
                     std::tie(graph.nodes[b].kind, graph.nodes[b].name, b);
            });
}

char lower(char c)
{
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// How `name` matches `query` (not empty), ignoring case; nullopt if it
// doesn't.
std::optional<int> rankOf(std::string_view name, std::string_view query)
{
  const auto equal = [](char a, char b) { return lower(a) == lower(b); };
  const auto hit = std::ranges::search(name, query, equal);

  if (hit.empty())
  {
    std::size_t next = 0;

    for (const char c : name)
      if (next < query.size() && equal(c, query[next]))
        ++next;

    if (next == query.size())
      return LETTERS;

    return std::nullopt;
  }

  if (hit.begin() == name.begin())
    return name.size() == query.size() ? WHOLE : PREFIX;

  // A later word: a capital after a small letter, or after a separator.
  for (std::size_t at = 1; at + query.size() <= name.size(); ++at)
  {
    const char before = name[at - 1];
    const bool wordStart =
        (std::isupper(static_cast<unsigned char>(name[at])) &&
         !std::isupper(static_cast<unsigned char>(before))) ||
        before == '_' || before == ':' || before == '/';

    if (wordStart &&
        std::equal(query.begin(), query.end(), name.begin() + at, equal))
      return WORD;
  }

  return SUBSTRING;
}

} // namespace

} // namespace architecture
