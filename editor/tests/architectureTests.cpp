#include "../../engine/tests/testHarness.h"
#include "architecture/graph.h"
#include "architecture/highlight.h"
#include "architecture/layout.h"
#include "architecture/positions.h"
#include "architecture/routing.h"
#include "widgets/boardCanvas.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
  #include "architecture/generator.h"

  #include <chrono>
  #include <thread>
#endif

namespace arch = architecture;

using board::operator+;

namespace
{

// A small game's export: a combat system and a spawner, the components and
// event they share, and a utils file. The node and edge of unknown kinds (a
// newer sfs-lsp's) and the edge to a missing node are left out when read.
constexpr std::string_view FIXTURE = R"({
  "version": 1,
  "root": "/games/Arena",
  "nodes": [
    {"id": "component:Armor", "kind": "component", "name": "Armor",
     "qualifiedName": "Armor", "file": "scripts/components/Armor.h",
     "line": 3, "declared": true, "engine": false, "marked": false},
    {"id": "component:Health", "kind": "component", "name": "Health",
     "qualifiedName": "Health", "file": "scripts/components/Health.h",
     "line": 3, "declared": true, "engine": false, "marked": false},
    {"id": "component:sfs::TransformComponent", "kind": "component",
     "name": "TransformComponent", "qualifiedName": "sfs::TransformComponent",
     "declared": false, "engine": true},
    {"id": "event:EnemyDiedEvent", "kind": "event", "name": "EnemyDiedEvent",
     "qualifiedName": "EnemyDiedEvent", "file": "scripts/events/EnemyEvents.h",
     "line": 4, "declared": true, "engine": false, "role": "news"},
    {"id": "gizmo:Strange", "kind": "gizmo", "name": "Strange",
     "qualifiedName": "Strange", "declared": true, "engine": false},
    {"id": "system:CombatSystem", "kind": "system", "name": "CombatSystem",
     "qualifiedName": "CombatSystem", "file": "scripts/systems/CombatSystem.h",
     "line": 6, "declared": true, "engine": false},
    {"id": "system:SpawnSystem", "kind": "system", "name": "SpawnSystem",
     "qualifiedName": "SpawnSystem", "file": "scripts/systems/SpawnSystem.h",
     "line": 5, "declared": true, "engine": false},
    {"id": "utils:scripts/utils/Damage", "kind": "utils", "name": "Damage",
     "qualifiedName": "scripts/utils/Damage", "file": "scripts/utils/Damage.h",
     "line": 1, "declared": true, "engine": false,
     "files": ["scripts/utils/Damage.h", "scripts/utils/Damage.cpp"]}
  ],
  "edges": [
    {"from": "system:CombatSystem", "to": "component:Armor", "kind": "reads",
     "sites": [{"file": "scripts/systems/CombatSystem.cpp", "line": 20,
                "via": "query", "term": "required"}]},
    {"from": "system:CombatSystem", "to": "component:Health", "kind": "writes",
     "sites": [{"file": "scripts/systems/CombatSystem.cpp", "line": 31,
                "via": "query", "term": "required"},
               {"file": "scripts/systems/CombatSystem.cpp", "line": 58,
                "via": "getComponent"}]},
    {"from": "system:CombatSystem", "to": "event:EnemyDiedEvent",
     "kind": "sends",
     "sites": [{"file": "scripts/systems/CombatSystem.cpp", "line": 40,
                "via": "send"}]},
    {"from": "system:CombatSystem", "to": "utils:scripts/utils/Damage",
     "kind": "uses-util",
     "sites": [{"file": "scripts/systems/CombatSystem.cpp", "line": 2,
                "via": "include"}]},
    {"from": "system:SpawnSystem", "to": "component:Health", "kind": "adds",
     "sites": [{"file": "scripts/systems/SpawnSystem.cpp", "line": 12,
                "via": "addComponent"}]},
    {"from": "system:SpawnSystem", "to": "component:sfs::TransformComponent",
     "kind": "reads",
     "sites": [{"file": "scripts/systems/SpawnSystem.cpp", "line": 14,
                "via": "getComponent"}]},
    {"from": "system:SpawnSystem", "to": "event:EnemyDiedEvent",
     "kind": "consumes",
     "sites": [{"file": "scripts/systems/SpawnSystem.cpp", "line": 8,
                "via": "consumer"}]},
    {"from": "system:SpawnSystem", "to": "system:CombatSystem",
     "kind": "reads-system",
     "sites": [{"file": "scripts/systems/SpawnSystem.cpp", "line": 9,
                "via": "getSystem"}]},
    {"from": "system:SpawnSystem", "to": "component:Health", "kind": "teleports",
     "sites": []},
    {"from": "system:SpawnSystem", "to": "component:Missing", "kind": "reads",
     "sites": []},
    {"from": "utils:scripts/utils/Damage", "to": "component:Health",
     "kind": "writes",
     "sites": [{"file": "scripts/utils/Damage.cpp", "line": 7,
                "via": "getComponent"}]}
  ]
})";

arch::Graph fixture()
{
  std::string error;
  std::optional<arch::Graph> graph = arch::parseGraph(FIXTURE, error);
  return graph ? std::move(*graph) : arch::Graph();
}

uint32_t nodeOf(const arch::Graph& graph, std::string_view id)
{
  return graph.find(id).value_or(UINT32_MAX);
}

// Every box the same size, as the board's are (a title and one line).
std::vector<ImVec2> sizesOf(const arch::Graph& graph)
{
  return std::vector<ImVec2>(graph.nodes.size(), ImVec2(160.0f, 50.0f));
}

// A board's grid of boxes, as the layout makes them: columns of differing
// widths, rows a pitch apart, a gutter between columns. Two more are dragged
// off it: one into a gutter, one across a gap between rows.
std::vector<ImRect> gridOfBoxes()
{
  std::vector<ImRect> boxes;

  for (int column = 0; column < 6; ++column)
    for (int row = 0; row < 8; ++row)
    {
      const float x = static_cast<float>(column) * 360.0f;
      const float y = static_cast<float>(row) * 90.0f;
      const float width =
          140.0f + static_cast<float>((row * 7 + column) % 5) * 12.0f;
      boxes.emplace_back(ImVec2(x, y), ImVec2(x + width, y + 50.0f));
    }

  boxes.emplace_back(ImVec2(230.0f, 200.0f), ImVec2(330.0f, 250.0f));
  boxes.emplace_back(ImVec2(1290.0f, 300.0f), ImVec2(1420.0f, 380.0f));
  return boxes;
}

// Links between them, picked by a fixed sequence: near and far, both ways,
// several from one box, and one box's link to itself.
std::vector<arch::RouteLink> linksOf(std::size_t boxes)
{
  std::vector<arch::RouteLink> links;
  uint32_t state = 12345;
  const auto next = [&]
  {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % static_cast<uint32_t>(boxes);
  };

  for (int i = 0; i < 160; ++i)
  {
    const uint32_t from = i < 20 ? 9 : next();
    uint32_t to = next();

    if (to == from)
      to = (to + 1) % static_cast<uint32_t>(boxes);

    links.push_back({from, to});
  }

  links.push_back({3, 3});
  return links;
}

// Whether a segment passes through a box (not just along its side).
bool through(ImVec2 a, ImVec2 b, const ImRect& box)
{
  ImRect inside = box;
  inside.Expand(-0.5f);
  const ImRect segment(ImMin(a, b), ImMax(a, b));
  return segment.Max.x > inside.Min.x && segment.Min.x < inside.Max.x &&
         segment.Max.y > inside.Min.y && segment.Min.y < inside.Max.y;
}

bool overlapping(std::span<const ImVec2> at, std::span<const ImVec2> sizes)
{
  for (std::size_t a = 0; a < at.size(); ++a)
    for (std::size_t b = a + 1; b < at.size(); ++b)
      if (ImRect(at[a], at[a] + sizes[a])
              .Overlaps(ImRect(at[b], at[b] + sizes[b])))
        return true;

  return false;
}

std::vector<std::string> names(const arch::Graph& graph,
                               const std::vector<arch::SearchHit>& hits)
{
  std::vector<std::string> out;

  for (const arch::SearchHit& hit : hits)
    out.push_back(graph.nodes[hit.node].name);

  return out;
}

} // namespace

int main()
{
  TEST("an export should be read: its nodes, edges, sites and files")
  {
    std::string error;
    const std::optional<arch::Graph> graph = arch::parseGraph(FIXTURE, error);
    CHECK(graph.has_value());
    CHECK(error.empty());

    if (!graph)
      return testing::report("architectureTests");

    CHECK(graph->root == std::string("/games/Arena"));
    CHECK(graph->nodes.size() == 7u); // the gizmo is left out
    CHECK(graph->edges.size() == 9u); // and the teleport and the missing node

    const uint32_t combat = nodeOf(*graph, "system:CombatSystem");
    CHECK(combat < graph->nodes.size());
    const arch::Node& node = graph->nodes[combat];
    CHECK(node.kind == arch::NodeKind::System);
    CHECK(node.name == std::string("CombatSystem"));
    CHECK(node.line == 6);
    CHECK(node.declared);
    CHECK(graph->files[node.file] ==
          std::string("scripts/systems/CombatSystem.h"));

    const arch::Node& transform =
        graph->nodes[nodeOf(*graph, "component:sfs::TransformComponent")];
    CHECK(transform.engine);
    CHECK(!transform.declared);
    CHECK(transform.file == arch::NO_FILE);
    CHECK(graph->nodes[nodeOf(*graph, "event:EnemyDiedEvent")].role ==
          std::string("news"));

    // Edges keep their order (from, to, kind) and every site.
    const uint32_t health = nodeOf(*graph, "component:Health");
    const auto writes = std::ranges::find_if(
        graph->edges,
        [&](const arch::Edge& edge)
        {
          return edge.from == combat && edge.to == health &&
                 edge.kind == arch::EdgeKind::Writes;
        });
    CHECK(writes != graph->edges.end());
    CHECK(writes->sites.size() == 2u);
    CHECK(writes->sites[1].line == 58);
    CHECK(writes->sites[1].via == std::string("getComponent"));
    CHECK(writes->sites[0].term == std::string("required"));
    CHECK(graph->files[writes->sites[0].file] ==
          std::string("scripts/systems/CombatSystem.cpp"));

    // A file named many times is kept once.
    CHECK(std::ranges::count(
              graph->files, std::string("scripts/systems/CombatSystem.cpp")) ==
          1);

    // Health is touched by the combat system, the spawner and the utils.
    CHECK(graph->edgesOf(health).size() == 3u);

    for (const uint32_t edge : graph->edgesOf(health))
      CHECK(graph->edges[edge].to == health);

    CHECK(graph->edgesOf(combat).size() == 5u); // 4 out, 1 reads-system in
  }

  TEST("an export that can't be read should say why")
  {
    std::string error;
    CHECK(!arch::parseGraph("not json", error).has_value());
    CHECK(!error.empty());

    error.clear();
    CHECK(
        !arch::parseGraph(R"({"version": 2, "nodes": [], "edges": []})", error)
             .has_value());
    CHECK(error.find("version 2") != std::string::npos);

    error.clear();
    CHECK(
        !arch::parseGraph(R"({"version": 1, "nodes": []})", error).has_value());
    CHECK(!error.empty());

    error.clear();
    CHECK(!arch::parseGraph("[1, 2]", error).has_value());
    CHECK(!error.empty());

    // Fields of the wrong type are taken as missing, not as an error.
    error.clear();
    const std::optional<arch::Graph> odd = arch::parseGraph(
        R"({"version": 1, "nodes": [{"id": "system:A", "kind": "system",
             "name": 5, "line": "x", "declared": "yes"}, 7], "edges": [3]})",
        error);
    CHECK(odd.has_value());

    if (odd)
    {
      CHECK(odd->nodes.size() == 1u);
      CHECK(odd->nodes[0].name == std::string("system:A"));
      CHECK(odd->nodes[0].line == 0);
      CHECK(!odd->nodes[0].declared);
    }
  }

  TEST("progress lines should be read, and other lines not")
  {
    const std::optional<arch::Progress> progress =
        arch::parseProgress("progress 312 623 index");
    CHECK(progress.has_value());

    if (progress)
    {
      CHECK(progress->done == 312);
      CHECK(progress->total == 623);
      CHECK(progress->phase == std::string("index"));
    }

    CHECK(!arch::parseProgress("").has_value());
    CHECK(!arch::parseProgress("progress").has_value());
    CHECK(!arch::parseProgress("progress 1 2").has_value());
    CHECK(!arch::parseProgress("progress 1 2 ").has_value());
    CHECK(!arch::parseProgress("progress a 2 parse").has_value());
    CHECK(!arch::parseProgress("progress -1 2 parse").has_value());
    CHECK(!arch::parseProgress("warning: progress 1 2 parse").has_value());
    CHECK(!arch::parseProgress("progress 1x 2 parse").has_value());
  }

  TEST("the layout should be the same every time, in its columns, unoverlapped")
  {
    const arch::Graph graph = fixture();
    const std::vector<ImVec2> sizes = sizesOf(graph);
    std::vector<ImVec2> first;
    std::vector<ImVec2> second;
    arch::layoutGraph(graph, sizes, {}, first);
    arch::layoutGraph(graph, sizes, {}, second);
    CHECK(first.size() == graph.nodes.size());

    bool same = first.size() == second.size();

    for (std::size_t i = 0; same && i < first.size(); ++i)
      same = first[i].x == second[i].x && first[i].y == second[i].y;

    CHECK(same);
    CHECK(!overlapping(first, sizes));

    // Left to right: components, systems, utils, events.
    const auto x = [&](std::string_view id)
    { return first[nodeOf(graph, id)].x; };
    CHECK(x("component:Armor") == x("component:Health"));
    CHECK(x("component:sfs::TransformComponent") == x("component:Health"));
    CHECK(x("component:Health") < x("system:CombatSystem"));
    CHECK(x("system:CombatSystem") == x("system:SpawnSystem"));
    CHECK(x("system:CombatSystem") < x("utils:scripts/utils/Damage"));
    CHECK(x("utils:scripts/utils/Damage") < x("event:EnemyDiedEvent"));
  }

  TEST("each kind of node should have one column, holding only that kind")
  {
    const arch::Graph graph = fixture();
    std::vector<ImVec2> sizes = sizesOf(graph);

    // Boxes of differing widths, so a column's width is its widest box's.
    for (std::size_t i = 0; i < sizes.size(); ++i)
      sizes[i].x += static_cast<float>(i % 3) * 40.0f;

    std::vector<ImVec2> at;
    arch::layoutGraph(graph, sizes, {}, at);
    std::array<float, arch::NODE_KINDS> left;
    std::array<float, arch::NODE_KINDS> right;
    std::array<bool, arch::NODE_KINDS> present{};
    left.fill(0.0f);
    right.fill(0.0f);
    bool oneColumn = true;

    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    {
      const auto kind = static_cast<std::size_t>(graph.nodes[i].kind);

      if (!present[kind])
      {
        present[kind] = true;
        left[kind] = at[i].x;
        right[kind] = at[i].x + sizes[i].x;
        continue;
      }

      oneColumn &= at[i].x == left[kind];
      right[kind] = std::max(right[kind], at[i].x + sizes[i].x);
    }

    CHECK(oneColumn);
    CHECK(std::ranges::count(present, true) == 4);

    // No two kinds share a column or overlap, and each has room beside it.
    bool apart = true;

    for (std::size_t a = 0; a < arch::NODE_KINDS; ++a)
      for (std::size_t b = 0; b < arch::NODE_KINDS; ++b)
        if (a != b && present[a] && present[b] && left[a] <= left[b])
          apart &= right[a] + 100.0f < left[b];

    CHECK(apart);
    CHECK(!overlapping(at, sizes));
  }

  TEST("saved positions should be kept, and new boxes placed clear of them")
  {
    const arch::Graph graph = fixture();
    const std::vector<ImVec2> sizes = sizesOf(graph);
    std::vector<ImVec2> fresh;
    arch::layoutGraph(graph, sizes, {}, fresh);

    // Every box saved but the event, and the combat system moved onto the
    // event's spot.
    arch::Positions saved;

    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
      saved[graph.nodes[i].id] = fresh[i];

    const uint32_t event = nodeOf(graph, "event:EnemyDiedEvent");
    saved.erase(graph.nodes[event].id);
    saved["system:CombatSystem"] = fresh[event];
    saved["system:Gone"] = ImVec2(5.0f, 5.0f); // a node no longer there

    std::vector<ImVec2> placed;
    arch::layoutGraph(graph, sizes, saved, placed);

    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    {
      if (i == event)
        continue;

      const ImVec2 kept = saved[graph.nodes[i].id];
      CHECK(placed[i].x == kept.x);
      CHECK(placed[i].y == kept.y);
    }

    // The event stays in its column, moved down clear of the combat system.
    CHECK(placed[event].x == fresh[event].x);
    CHECK(placed[event].y > fresh[event].y);
    CHECK(!overlapping(placed, sizes));
  }

  TEST("a neighbourhood should hold what points at a node and what it points "
       "at, round it")
  {
    const arch::Graph graph = fixture();
    const uint32_t combat = nodeOf(graph, "system:CombatSystem");
    arch::Neighbourhood around;
    arch::neighbourhood(graph, {}, combat, around);
    CHECK(around.centre == combat);
    CHECK(around.outgoing.size() == 4u);
    CHECK(around.incoming.size() == 1u);
    CHECK(around.incoming.front() == nodeOf(graph, "system:SpawnSystem"));

    // Hidden arrows don't make neighbours.
    arch::Filter filter;
    filter.edgeKinds[static_cast<std::size_t>(arch::EdgeKind::ReadsSystem)] =
        false;
    filter.nodeKinds[static_cast<std::size_t>(arch::NodeKind::Event)] = false;
    arch::neighbourhood(graph, filter, combat, around);
    CHECK(around.incoming.empty());
    CHECK(around.outgoing.size() == 3u);

    // Laid out round the centre: in to the left, out to the right.
    arch::neighbourhood(graph, {}, combat, around);
    const std::vector<ImVec2> sizes = sizesOf(graph);
    std::vector<ImVec2> at(graph.nodes.size(), ImVec2(0.0f, 0.0f));
    arch::layoutNeighbourhood(around, sizes, at);
    CHECK(at[combat].x == -80.0f);
    CHECK(at[combat].y == 0.0f);
    CHECK(at[around.incoming.front()].x + sizes[0].x < at[combat].x);

    for (const uint32_t node : around.outgoing)
      CHECK(at[node].x > at[combat].x + sizes[combat].x);
  }

  TEST("hovering a box should light it, its arrows and its neighbours")
  {
    const arch::Graph graph = fixture();
    const uint32_t health = nodeOf(graph, "component:Health");
    const uint32_t armor = nodeOf(graph, "component:Armor");
    const uint32_t combat = nodeOf(graph, "system:CombatSystem");
    const uint32_t spawn = nodeOf(graph, "system:SpawnSystem");
    const uint32_t damage = nodeOf(graph, "utils:scripts/utils/Damage");
    arch::Lit lit;

    arch::light(graph, {}, {}, {}, lit);
    CHECK(!lit.active);

    const uint32_t focus[] = {health};
    arch::light(graph, {}, focus, {}, lit);
    CHECK(lit.active);
    CHECK(lit.nodes[health] == 1);
    CHECK(lit.nodes[combat] == 1);
    CHECK(lit.nodes[spawn] == 1);
    CHECK(lit.nodes[damage] == 1);
    CHECK(lit.nodes[armor] == 0);
    CHECK(std::ranges::count(lit.edges, 1) == 3);

    // Only writes shown: the spawner's add lights nothing.
    arch::Filter writes;
    writes.edgeKinds.fill(false);
    writes.edgeKinds[static_cast<std::size_t>(arch::EdgeKind::Writes)] = true;
    arch::light(graph, writes, focus, {}, lit);
    CHECK(lit.nodes[spawn] == 0);
    CHECK(lit.nodes[combat] == 1);
    CHECK(std::ranges::count(lit.edges, 1) == 2);

    // A hovered arrow lights itself and its two ends.
    const uint32_t first[] = {0};
    arch::light(graph, {}, {}, first, lit);
    CHECK(lit.active);
    CHECK(std::ranges::count(lit.edges, 1) == 1);
    CHECK(std::ranges::count(lit.nodes, 1) == 2);
    CHECK(lit.nodes[graph.edges[0].from] == 1);
    CHECK(lit.nodes[graph.edges[0].to] == 1);
  }

  TEST("a search should find names, best matches first, ignoring case")
  {
    const arch::Graph graph = fixture();
    std::vector<arch::SearchHit> hits;

    arch::search(graph, "", 10, hits);
    CHECK(hits.empty());

    arch::search(graph, "health", 10, hits);
    CHECK(names(graph, hits) == std::vector<std::string>{"Health"});

    // Whole name, then prefix, then a later word, then anywhere, then the
    // letters in order.
    arch::search(graph, "system", 10, hits);
    CHECK(names(graph, hits) ==
          (std::vector<std::string>{"CombatSystem", "SpawnSystem"}));

    arch::search(graph, "died", 10, hits);
    CHECK(names(graph, hits) == std::vector<std::string>{"EnemyDiedEvent"});

    arch::search(graph, "ar", 10, hits);
    CHECK(names(graph, hits) ==
          (std::vector<std::string>{"Armor", "TransformComponent"}));
    CHECK(hits[0].rank < hits[1].rank);

    arch::search(graph, "edv", 10, hits);
    CHECK(names(graph, hits) == std::vector<std::string>{"EnemyDiedEvent"});

    arch::search(graph, "e", 2, hits);
    CHECK(hits.size() == 2u);
    CHECK(names(graph, hits)[0] == std::string("EnemyDiedEvent"));

    arch::search(graph, "zzz", 10, hits);
    CHECK(hits.empty());
  }

  TEST("a file should lead to the node it's of, in any path form")
  {
    constexpr std::string_view FILES = R"({
      "version": 1,
      "root": "/home/me/Arena",
      "nodes": [
        {"id": "component:Health", "kind": "component", "name": "Health",
         "file": "scripts/components/Health.h"},
        {"id": "component:MaxHealth", "kind": "component", "name": "MaxHealth",
         "file": "scripts/components/Health.h"},
        {"id": "component:Shield", "kind": "component", "name": "Shield",
         "file": "scripts/components/Guard.h"},
        {"id": "component:Ward", "kind": "component", "name": "Ward",
         "file": "scripts/components/Guard.h"},
        {"id": "system:CombatSystem", "kind": "system", "name": "CombatSystem",
         "file": "scripts/systems/CombatSystem.h"},
        {"id": "utils:scripts/utils/Damage", "kind": "utils", "name": "Damage",
         "file": "scripts/utils/Damage.h",
         "files": ["scripts/utils/Damage.h", "scripts/utils/Damage.cpp"]}
      ],
      "edges": [
        {"from": "system:CombatSystem", "to": "component:Health",
         "kind": "reads",
         "sites": [{"file": "scripts/systems/CombatHits.cpp", "line": 3},
                   {"file": "scripts/systems/CombatHits.cpp", "line": 9}]},
        {"from": "system:CombatSystem", "to": "utils:scripts/utils/Damage",
         "kind": "uses-util",
         "sites": [{"file": "scripts/systems/CombatSystem.cpp", "line": 2}]}
      ]
    })";
    std::string error;
    const std::optional<arch::Graph> graph = arch::parseGraph(FILES, error);
    CHECK(graph.has_value());

    if (!graph)
      return testing::report("architectureTests");

    const auto named = [&](std::string_view path) -> std::string
    {
      const std::optional<uint32_t> node = arch::nodeOfFile(*graph, path);
      return node ? graph->nodes[*node].name : std::string("none");
    };

    // A type's header and its source, however the path is written.
    CHECK(named("/home/me/Arena/scripts/systems/CombatSystem.cpp") ==
          "CombatSystem");
    CHECK(named("C:\\Games\\Arena\\Scripts\\Systems\\combatsystem.h") ==
          "CombatSystem");
    CHECK(named("//wsl.localhost/Ubuntu/home/me/Arena/scripts/systems/"
                "CombatSystem.h") == "CombatSystem");
    CHECK(named("/mnt/c/Arena/scripts/systems/CombatSystem.cpp") ==
          "CombatSystem");

    // Several declared in one file: the one named after it, else the first.
    CHECK(named("/home/me/Arena/scripts/components/Health.h") == "Health");
    CHECK(named("/home/me/Arena/scripts/components/Guard.h") == "Shield");

    // A utils file, and a type's code split into a file of its own.
    CHECK(named("/home/me/Arena/scripts/utils/Damage.cpp") == "Damage");
    CHECK(named("/home/me/Arena/scripts/systems/CombatHits.cpp") ==
          "CombatSystem");

    // Nothing of it: a name that only ends like a file's, or another file.
    CHECK(named("/home/me/Arena/scripts/components/MaxHealth.h") == "none");
    CHECK(named("/home/me/Arena/README.md") == "none");
    CHECK(named("") == "none");
  }

  TEST("links should go round every box, bundled, spread or between")
  {
    const std::vector<ImRect> boxes = gridOfBoxes();
    const std::vector<arch::RouteLink> links = linksOf(boxes.size());
    arch::Router router;
    arch::Routes routes;
    router.route(boxes, links, routes);
    CHECK(routes.cornerStart.size() == links.size() + 1);
    std::vector<ImVec2> path;
    int crossings = 0;
    int straight = 0;

    for (uint32_t link = 0; link < links.size(); ++link)
    {
      straight += routes.routed[link] ? 0 : 1;

      for (const float spread : {0.0f, 0.5f, 1.0f})
      {
        arch::routePath(routes, link, spread, path);

        for (std::size_t k = 0; k + 1 < path.size(); ++k)
        {
          // Every segment runs across or along, never slanted.
          CHECK((path[k].x == path[k + 1].x || path[k].y == path[k + 1].y));

          for (const ImRect& box : boxes)
            crossings += through(path[k], path[k + 1], box) ? 1 : 0;
        }

        // It starts and ends on its boxes' sides.
        const ImRect& from = boxes[links[link].from];
        const ImRect& to = boxes[links[link].to];
        CHECK((path.front().x == from.Min.x || path.front().x == from.Max.x));
        CHECK((path.back().x == to.Min.x || path.back().x == to.Max.x));
        CHECK(path.front().y >= from.Min.y);
        CHECK(path.front().y <= from.Max.y);
        CHECK(path.back().y >= to.Min.y);
        CHECK(path.back().y <= to.Max.y);

        // Its bounds hold it however far it's spread.
        const ImRect& bounds = routes.bounds[link];

        for (const ImVec2& point : path)
          CHECK((point.x >= bounds.Min.x && point.x <= bounds.Max.x &&
                 point.y >= bounds.Min.y && point.y <= bounds.Max.y));
      }
    }

    CHECK(crossings == 0);
    CHECK(straight == 0);
  }

  TEST("routing should be the same every time, a router reused or not")
  {
    const std::vector<ImRect> boxes = gridOfBoxes();
    const std::vector<arch::RouteLink> links = linksOf(boxes.size());
    arch::Router reused;
    arch::Routes first, again, fresh;
    reused.route(boxes, links, first);
    reused.route(boxes, links, again);
    arch::Router other;
    other.route(boxes, links, fresh);

    const auto same = [](const arch::Routes& a, const arch::Routes& b)
    {
      if (a.corners.size() != b.corners.size() ||
          a.cornerStart != b.cornerStart || a.pieces.size() != b.pieces.size())
        return false;

      for (std::size_t c = 0; c < a.corners.size(); ++c)
        if (a.corners[c].x != b.corners[c].x ||
            a.corners[c].y != b.corners[c].y ||
            a.spread[c].x != b.spread[c].x || a.spread[c].y != b.spread[c].y)
          return false;

      return a.pieceLinks == b.pieceLinks;
    };

    CHECK(same(first, again));
    CHECK(same(first, fresh));
  }

  TEST("spread links should each have a lane: no two run along one line")
  {
    const std::vector<ImRect> boxes = gridOfBoxes();
    const std::vector<arch::RouteLink> links = linksOf(boxes.size());
    arch::Router router;
    arch::Routes routes;
    router.route(boxes, links, routes);

    // Every segment of every link spread, then any two of different links
    // that lie along one line and overlap there.
    struct Segment
    {
      ImVec2 a, b;
      uint32_t link;
    };
    std::vector<Segment> segments;
    std::vector<ImVec2> path;

    for (uint32_t link = 0; link < links.size(); ++link)
    {
      arch::routePath(routes, link, 1.0f, path);

      for (std::size_t k = 0; k + 1 < path.size(); ++k)
        segments.push_back(
            {ImMin(path[k], path[k + 1]), ImMax(path[k], path[k + 1]), link});
    }

    int coinciding = 0;

    for (std::size_t i = 0; i < segments.size(); ++i)
      for (std::size_t j = i + 1; j < segments.size(); ++j)
      {
        const Segment& s = segments[i];
        const Segment& t = segments[j];

        if (s.link == t.link)
          continue;

        const bool vertical =
            s.a.x == s.b.x && t.a.x == t.b.x && s.a.x == t.a.x &&
            std::min(s.b.y, t.b.y) - std::max(s.a.y, t.a.y) > 1.0f;
        const bool horizontal =
            s.a.y == s.b.y && t.a.y == t.b.y && s.a.y == t.a.y &&
            std::min(s.b.x, t.b.x) - std::max(s.a.x, t.a.x) > 1.0f;
        coinciding += vertical || horizontal ? 1 : 0;
      }

    CHECK(coinciding == 0);

    // Bundled, a box's links leave it as one: the stub out of the side the
    // most-linked box uses carries several.
    const auto busiest = std::ranges::max_element(
        routes.pieces,
        [](const arch::RoutePiece& a, const arch::RoutePiece& b)
        { return a.linkCount < b.linkCount; });
    CHECK(busiest != routes.pieces.end());
    CHECK(busiest->linkCount >= 10u);
  }

  TEST("a box sitting on every way round should leave its links straight")
  {
    // The target walled in on all four sides: nowhere to go round, so the
    // link is drawn straight rather than not at all.
    const std::vector<ImRect> boxes = {
        {{0.0f, 0.0f}, {100.0f, 50.0f}},
        {{400.0f, 0.0f}, {500.0f, 50.0f}},
        {{380.0f, -40.0f}, {520.0f, -2.0f}},
        {{380.0f, 52.0f}, {520.0f, 90.0f}},
        {{360.0f, -40.0f}, {398.0f, 90.0f}},
        {{502.0f, -40.0f}, {540.0f, 90.0f}},
    };
    const std::vector<arch::RouteLink> links = {{0, 1}};
    arch::Router router;
    arch::Routes routes;
    router.route(boxes, links, routes);
    CHECK(routes.routed[0] == 0);
    std::vector<ImVec2> path;
    arch::routePath(routes, 0, 1.0f, path);
    CHECK(path.size() == 2u);
  }

  TEST("positions should be saved and read back; a bad file reads as none")
  {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "sfsArchitectureTests";
    std::filesystem::remove_all(folder);
    const std::filesystem::path file = folder / ".sfs" / "positions.json";

    CHECK(arch::readPositions(file).empty());

    arch::Positions positions;
    positions["system:CombatSystem"] = ImVec2(10.5f, -20.5f);
    positions["component:\"Quoted\""] = ImVec2(0.0f, 3.0f);
    CHECK(arch::writePositions(file, positions));

    const arch::Positions read = arch::readPositions(file);
    CHECK(read.size() == 2u);
    CHECK(read.at("system:CombatSystem").x == 10.5f);
    CHECK(read.at("system:CombatSystem").y == -20.5f);
    CHECK(read.at("component:\"Quoted\"").y == 3.0f);

    // A file from an older layout is laid out afresh.
    std::ofstream(file)
        << R"({"version": 2, "positions": {"system:A": [1, 2]}})";
    CHECK(arch::readPositions(file).empty());

    std::ofstream(file) << "{ not json";
    CHECK(arch::readPositions(file).empty());
    std::filesystem::remove_all(folder);
  }

#if !defined(_WIN32)
  TEST("a run should report progress and give the graph it reads")
  {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "sfsArchitectureRun";
    std::filesystem::create_directories(folder);
    const std::filesystem::path json = folder / "graph.json";
    std::ofstream(json) << FIXTURE;

    const auto finish = [](arch::Generator& generator)
    {
      std::optional<arch::Generated> done;

      while (!(done = generator.poll()))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

      return *done;
    };

    arch::Generator generator;
    CHECK(generator.start({"sh",
                           "-c",
                           "echo 'progress 1 2 parse' >&2; echo 'progress 2 2 "
                           "write' >&2; cat \"$0\"",
                           json.string()}));
    const arch::Generated done = finish(generator);
    CHECK(done.graph.has_value());
    CHECK(done.graph && done.graph->nodes.size() == 7u);
    CHECK(generator.progress().done == 2);
    CHECK(generator.progress().phase == std::string("write"));
    CHECK(!generator.running());

    // A failed run gives its exit code and what it said.
    CHECK(generator.start({"sh", "-c", "echo 'no such folder' >&2; exit 2"}));
    const arch::Generated failed = finish(generator);
    CHECK(!failed.graph.has_value());
    CHECK(failed.error.find("exit code 2") != std::string::npos);
    CHECK(failed.error.find("no such folder") != std::string::npos);

    // Output that isn't an export.
    CHECK(generator.start({"sh", "-c", "echo hello"}));
    CHECK(!finish(generator).graph.has_value());

    // Stopped: nothing comes.
    CHECK(generator.start({"sh", "-c", "sleep 100"}));
    generator.stop();
    CHECK(!generator.running());
    CHECK(!generator.poll().has_value());
    std::filesystem::remove_all(folder);
  }
#endif

  return testing::report("architectureTests");
}
