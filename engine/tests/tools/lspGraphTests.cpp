#include "../testHarness.h"
#include "graph.h"
#include "lspTestSupport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace sfs::lsp;
using Json = nlohmann::ordered_json;

namespace
{

// The systems' sources, kept to look their lines up.
const std::string kHealthSystem = R"(#include "systems/HealthSystem.h"

#include "utils/Healing.h"

namespace
{

void calm(const sfs::Entity& entity)
{
  entity.removeComponent<Frozen>();
}

struct ThawJob
{
  void run(const sfs::Entity& entity) { entity.addComponent<Frozen>(); }
};

} // namespace

void HealthSystem::create()
{
  registerComponent<Health>();
  registerComponent<sfs::Any<Door, Motion>>();
  registerComponent<sfs::Excluding<Frozen>>();
  observe<Motion, sfs::With<Door>>([this](const sfs::Entity&) {});
  m_paths = consumer<PathRequest>();
}

void HealthSystem::update(double)
{
  for (const sfs::Entity& e :
       registry->query<const Motion, sfs::With<Door>, sfs::Excluding<Frozen>>())
    calm(e);

  const Score* score = registry->sceneData<const Score>();
  registry->sceneData<Score>()->points = 1;
  const auto look = [this](const sfs::Entity& e)
  { return e.getComponent<const Door>(); };
  send(EnemyDiedEvent{1});

  for (auto [e, request, responder] : m_paths.read())
    responder.respond(PathFoundResponse{1});

  getSystem<MoveSystem>();
}
)";

const std::string kMoveSystem = R"(#include "systems/MoveSystem.h"

void MoveSystem::create()
{
  registerComponent<Motion>();
  m_paths = requester<PathRequest>();
}

void MoveSystem::update(double)
{
  for (auto [e, motion] : getEntities<Motion>())
  {
    e.getComponent<sfs::TransformComponent>();
    send(e, DoorOpenedEvent{2});
  }

  hasSystem<MoveSystem>();
}
)";

// A temporary SFS project of each kind of node and edge.
struct Project
{
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / "sfsLspGraphProject";
  std::vector<std::filesystem::path> files;

  Project()
  {
    std::filesystem::remove_all(root);
    lspTest::writeFile(root / "project.sfsproj", "{}");
    write("scripts/components/Health.h",
          "#pragma once\nstruct Health { float points = 0; };\n");
    write("scripts/components/Motion.h",
          "#pragma once\nstruct Motion { float speed = 0; };\n");
    write("scripts/components/Door.h",
          "#pragma once\nstruct Door { bool open = false; };\n");
    write("scripts/components/Frozen.h",
          "#pragma once\nstruct [[sfs::component]] Frozen {};\n");
    write("scripts/components/Score.h",
          "#pragma once\nstruct [[sfs::scene_data]] Score { int points = 0; "
          "};\n");
    write("scripts/events/GameEvents.h",
          "#pragma once\n"
          "struct [[sfs::event_news]] EnemyDiedEvent { int id = 0; };\n"
          "struct [[sfs::event_entity_news]] DoorOpenedEvent { int id = 0; "
          "};\n"
          "struct [[sfs::event_response]] PathFoundResponse { int steps = 0; "
          "};\n"
          "struct [[sfs::event_response]] PathFailedResponse { int why = 0; "
          "};\n"
          "struct [[sfs::event_request(PathFoundResponse, failure = "
          "PathFailedResponse)]] PathRequest { int goal = 0; };\n");
    write("scripts/utils/Healing.h",
          "#pragma once\nvoid heal(const sfs::Entity& entity);\n");
    write("scripts/utils/Healing.cpp",
          "#include \"utils/Healing.h\"\n\nvoid heal(const sfs::Entity& "
          "entity)\n{\n  entity.getComponent<Health>()->points += 1;\n}\n");
    write("scripts/systems/HealthSystem.h",
          "#pragma once\nclass HealthSystem : public sfs::System\n{\n"
          "protected:\n  void create() override;\n  void update(double) "
          "override;\n\nprivate:\n  sfs::EventConsumer<PathRequest> "
          "m_paths;\n};\n");
    write("scripts/systems/HealthSystem.cpp", kHealthSystem);
    write("scripts/systems/MoveSystem.h",
          "#pragma once\nclass MoveSystem : public sfs::System\n{\n"
          "protected:\n  void create() override;\n  void update(double) "
          "override;\n\nprivate:\n  sfs::EventRequester<PathRequest> "
          "m_paths;\n};\n");
    write("scripts/systems/MoveSystem.cpp", kMoveSystem);
    write(
        "scripts/services/SpawnService.h",
        "#pragma once\nclass SpawnService : public sfs::Service\n{\n"
        "protected:\n  void create() override\n  {\n    m_died = "
        "consumer<EnemyDiedEvent>().onEvent(\n        [this](const "
        "EnemyDiedEvent&) { createEntity().addComponent<Health>(); });\n"
        "  }\n\nprivate:\n  sfs::EventConsumer<EnemyDiedEvent> m_died;\n};\n");
    write("scripts/objects/Player.h",
          "#pragma once\nclass Player : public sfs::GameObject\n{\npublic:\n"
          "  void onUpdate(double) override\n  {\n    "
          "scene().sceneData<const Score>();\n    "
          "scene().events().send(EnemyDiedEvent{3});\n  }\n};\n");
    std::sort(files.begin(), files.end());
  }

  ~Project() { std::filesystem::remove_all(root); }

  void write(const std::string& relative, std::string_view text)
  {
    lspTest::writeFile(root / relative, text);
    files.push_back((root / relative).lexically_normal());
  }
};

// The node with `id`, or null.
const Json* node(const Json& graph, std::string_view id)
{
  for (const Json& item : graph["nodes"])
    if (item["id"] == id)
      return &item;

  return nullptr;
}

// The edge `kind` from `from` to `to`, or null.
const Json* edge(const Json& graph,
                 std::string_view from,
                 std::string_view kind,
                 std::string_view to)
{
  for (const Json& item : graph["edges"])
    if (item["from"] == from && item["kind"] == kind && item["to"] == to)
      return &item;

  return nullptr;
}

// Whether an edge has a site at `line` of `file`, by `via`, with `term` if
// one is given.
bool hasSite(const Json* found,
             std::string_view file,
             int line,
             std::string_view via,
             std::string_view term = {})
{
  if (!found)
    return false;

  for (const Json& site : (*found)["sites"])
    if (site["file"] == file && site["line"] == line && site["via"] == via &&
        (term.empty() || site["term"] == term))
      return true;

  return false;
}

// The 1-based line of the first `needle` in `text`.
int lineOf(std::string_view text, std::string_view needle)
{
  const std::size_t at = text.find(needle);
  int line = 1;

  for (std::size_t i = 0; i < at && i < text.size(); ++i)
    line += text[i] == '\n' ? 1 : 0;

  return line;
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

} // namespace

int main()
{
  const Project project;
  const Json graph = exportGraph(project.root, project.files);
  const std::string health = "scripts/systems/HealthSystem.cpp";
  const std::string move = "scripts/systems/MoveSystem.cpp";

  TEST("the graph should hold its version, root and every kind of node")
  {
    CHECK(graph["version"] == GRAPH_VERSION);
    CHECK(graph["root"] == project.root.generic_string());

    for (const char* id : {"system:HealthSystem",
                           "system:MoveSystem",
                           "service:SpawnService",
                           "gameObject:Player",
                           "component:Health",
                           "component:Frozen",
                           "sceneData:Score",
                           "event:EnemyDiedEvent",
                           "event:DoorOpenedEvent",
                           "event:PathRequest",
                           "event:PathFoundResponse",
                           "event:PathFailedResponse",
                           "utils:scripts/utils/Healing",
                           "component:sfs::TransformComponent"})
    {
      if (!node(graph, id))
        std::printf("      no node %s\n", id);

      CHECK(node(graph, id));
    }

    const Json* system = node(graph, "system:HealthSystem");
    CHECK(system && (*system)["file"] == "scripts/systems/HealthSystem.h" &&
          (*system)["line"] == 2 && (*system)["declared"] == true &&
          (*system)["engine"] == false && (*system)["name"] == "HealthSystem");

    // A marked component and scene data say so; a plain one doesn't.
    CHECK((*node(graph, "component:Frozen"))["marked"] == true);
    CHECK((*node(graph, "component:Health"))["marked"] == false);
    CHECK((*node(graph, "sceneData:Score"))["marked"] == true);

    // Each event has its role, and a request its responses by role.
    CHECK((*node(graph, "event:EnemyDiedEvent"))["role"] == "news");
    CHECK((*node(graph, "event:DoorOpenedEvent"))["role"] == "entityNews");
    CHECK((*node(graph, "event:PathFoundResponse"))["role"] == "response");
    const Json& request = *node(graph, "event:PathRequest");
    CHECK(request["role"] == "request");
    CHECK(request["responses"]["success"] == "event:PathFoundResponse");
    CHECK(request["responses"]["failure"] == "event:PathFailedResponse");
    CHECK(!request["responses"].contains("progress"));

    // An engine type the files don't declare is a node too.
    const Json& transform = *node(graph, "component:sfs::TransformComponent");
    CHECK(transform["engine"] == true && transform["declared"] == false &&
          !transform.contains("file"));

    // A utils node is its header and source.
    const Json& utils = *node(graph, "utils:scripts/utils/Healing");
    CHECK(utils["files"].size() == 2);
  }

  TEST("registrations and observers should give each term's membership")
  {
    const int line = lineOf(kHealthSystem, "registerComponent<Health>");
    const Json* any =
        edge(graph, "system:HealthSystem", "registers", "component:Door");
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "registers", "component:Health"),
        health,
        line,
        "registerComponent",
        "required"));
    CHECK(hasSite(any, health, line + 1, "registerComponent", "any"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "registers", "component:Motion"),
        health,
        line + 1,
        "registerComponent",
        "any"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "registers", "component:Frozen"),
        health,
        line + 2,
        "registerComponent",
        "excluding"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "observes", "component:Door"),
        health,
        line + 3,
        "observe",
        "with"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "observes", "component:Motion"),
        health,
        line + 3,
        "observe",
        "required"));
  }

  TEST("const access should read, mutable access write, and With and "
       "Excluding filter")
  {
    const int query = lineOf(kHealthSystem, "registry->query");
    CHECK(
        hasSite(edge(graph, "system:HealthSystem", "reads", "component:Motion"),
                health,
                query,
                "query",
                "required"));
    CHECK(!edge(graph, "system:HealthSystem", "writes", "component:Motion"));
    CHECK(
        hasSite(edge(graph, "system:HealthSystem", "filters", "component:Door"),
                health,
                query,
                "query",
                "with"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "filters", "component:Frozen"),
        health,
        query,
        "query",
        "excluding"));
    CHECK(
        hasSite(edge(graph, "system:HealthSystem", "reads", "sceneData:Score"),
                health,
                lineOf(kHealthSystem, "sceneData<const Score>"),
                "sceneData"));
    CHECK(
        hasSite(edge(graph, "system:HealthSystem", "writes", "sceneData:Score"),
                health,
                lineOf(kHealthSystem, "sceneData<Score>"),
                "sceneData"));
    CHECK(
        hasSite(edge(graph, "system:MoveSystem", "writes", "component:Motion"),
                move,
                lineOf(kMoveSystem, "getEntities<Motion>"),
                "getEntities",
                "required"));
    CHECK(hasSite(edge(graph,
                       "system:MoveSystem",
                       "writes",
                       "component:sfs::TransformComponent"),
                  move,
                  lineOf(kMoveSystem, "getComponent"),
                  "getComponent"));
  }

  TEST("access should go to the system of the lambda, free function or "
       "helper class it's written in, and to a utils file's node")
  {
    CHECK(hasSite(edge(graph, "system:HealthSystem", "reads", "component:Door"),
                  health,
                  lineOf(kHealthSystem, "getComponent<const Door>"),
                  "getComponent"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "removes", "component:Frozen"),
        health,
        lineOf(kHealthSystem, "removeComponent"),
        "removeComponent"));
    CHECK(
        hasSite(edge(graph, "system:HealthSystem", "adds", "component:Frozen"),
                health,
                lineOf(kHealthSystem, "addComponent"),
                "addComponent"));
    CHECK(hasSite(
        edge(
            graph, "utils:scripts/utils/Healing", "writes", "component:Health"),
        "scripts/utils/Healing.cpp",
        5,
        "getComponent"));
    CHECK(!edge(graph, "system:HealthSystem", "writes", "component:Health"));
    CHECK(hasSite(edge(graph,
                       "system:HealthSystem",
                       "uses-util",
                       "utils:scripts/utils/Healing"),
                  health,
                  3,
                  "include"));

    // A service's handler lambda, and a game object's method.
    CHECK(edge(graph, "service:SpawnService", "adds", "component:Health"));
    CHECK(edge(graph, "gameObject:Player", "reads", "sceneData:Score"));
  }

  TEST("events should be sent, consumed, requested and responded to")
  {
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "consumes", "event:PathRequest"),
        health,
        lineOf(kHealthSystem, "consumer<PathRequest>"),
        "consumer"));
    CHECK(hasSite(edge(graph,
                       "system:HealthSystem",
                       "responds",
                       "event:PathFoundResponse"),
                  health,
                  lineOf(kHealthSystem, "respond("),
                  "respond"));
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "sends", "event:EnemyDiedEvent"),
        health,
        lineOf(kHealthSystem, "send("),
        "send"));
    CHECK(hasSite(
        edge(graph, "system:MoveSystem", "sends", "event:DoorOpenedEvent"),
        move,
        lineOf(kMoveSystem, "send("),
        "send"));
    CHECK(hasSite(
        edge(graph, "system:MoveSystem", "requests", "event:PathRequest"),
        move,
        lineOf(kMoveSystem, "requester<"),
        "requester"));
    CHECK(edge(
        graph, "service:SpawnService", "consumes", "event:EnemyDiedEvent"));
    CHECK(edge(graph, "gameObject:Player", "sends", "event:EnemyDiedEvent"));
  }

  TEST("a system reading another should be an edge, and reading itself not")
  {
    CHECK(hasSite(
        edge(graph, "system:HealthSystem", "reads-system", "system:MoveSystem"),
        health,
        lineOf(kHealthSystem, "getSystem"),
        "getSystem"));
    CHECK(
        !edge(graph, "system:MoveSystem", "reads-system", "system:MoveSystem"));
  }

  TEST("every edge's ends should be nodes, and its sites sorted and once each")
  {
    for (const Json& item : graph["edges"])
    {
      CHECK(node(graph, item["from"].get<std::string>()));
      CHECK(node(graph, item["to"].get<std::string>()));
      CHECK(!item["sites"].empty());

      for (std::size_t i = 1; i < item["sites"].size(); ++i)
      {
        const Json& a = item["sites"][i - 1];
        const Json& b = item["sites"][i];
        CHECK(std::tie(a["file"], a["line"]) <= std::tie(b["file"], b["line"]));
        CHECK(a != b);
      }
    }
  }

  TEST("the same tree should give the same JSON, with the same ids")
  {
    CHECK(exportGraph(project.root, project.files).dump() == graph.dump());
  }

  TEST("--graph --progress should write the JSON on stdout and well-formed, "
       "rising progress lines on stderr")
  {
    const std::filesystem::path out = project.root / "graph.json";
    const std::filesystem::path err = project.root / "progress.txt";
    const int status = testing::runCommand(
        "\"" SFS_LSP_PATH "\" --graph --progress --root \"" +
        project.root.string() + "\" \"" + (project.root / "scripts").string() +
        "\" > \"" + out.string() + "\" 2> \"" + err.string() + "\"");
    CHECK(status == 0);

    const Json printed = Json::parse(readText(out), nullptr, false);
    CHECK(!printed.is_discarded());
    CHECK(printed == graph);

    std::istringstream lines(readText(err));
    std::string line;
    std::size_t last = 0;
    std::size_t total = 0;
    std::size_t count = 0;
    std::vector<std::string> phases;

    while (std::getline(lines, line))
    {
      std::istringstream words(line);
      std::string word;
      std::size_t done = 0;
      std::size_t of = 0;
      std::string phase;
      std::string extra;
      CHECK(static_cast<bool>(words >> word >> done >> of >> phase) &&
            word == "progress" && !(words >> extra));
      CHECK(done >= last && done <= of);
      CHECK(total == 0 || of == total);
      last = done;
      total = of;
      ++count;

      if (phases.empty() || phases.back() != phase)
        phases.push_back(phase);
    }

    CHECK(count > 2 && last == total);
    CHECK((phases == std::vector<std::string>{
                         "scan", "parse", "index", "edges", "write"}));
  }

  return testing::report("lspGraphTests");
}
