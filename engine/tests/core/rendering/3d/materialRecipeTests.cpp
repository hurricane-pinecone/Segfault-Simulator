#include "../../../testHarness.h"

#include <engine/core/rendering/3d/materialGraph.h>
#include <engine/core/rendering/3d/materialGraphCompiler.h>

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sfs::materialGraph;

// The material recipes in the editor docs (SFS_RECIPES_PAGE): every graph
// there, pasted as the docs say, must build what it claims.

namespace
{

struct Recipe
{
  std::string heading; // the section it's in
  std::string json;
};

// Each ```json block, with the heading above it.
std::vector<Recipe> recipes(const std::string& page)
{
  std::vector<Recipe> found;
  std::istringstream lines(page);
  std::string line, heading, block;
  bool inBlock = false;
  while (std::getline(lines, line))
  {
    if (!inBlock && line.rfind("## ", 0) == 0)
      heading = line.substr(3);
    else if (!inBlock && line == "```json")
    {
      inBlock = true;
      block.clear();
    }
    else if (inBlock && line == "```")
    {
      inBlock = false;
      found.push_back({heading, block});
    }
    else if (inBlock)
      block += line + "\n";
  }
  return found;
}

} // namespace

int main()
{
  std::ifstream file(SFS_RECIPES_PAGE);
  std::stringstream page;
  page << file.rdbuf();
  const std::vector<Recipe> all = recipes(page.str());

  TEST("the recipes page should have its recipes")
  {
    CHECK(file.good());
    CHECK(all.size() >= 10u);
  }

  TEST("every recipe should paste whole and compile, every node feeding the "
       "output")
  {
    for (const Recipe& recipe : all)
    {
      const nlohmann::json json =
          nlohmann::json::parse(recipe.json, nullptr, false);
      const bool wrapped =
          json.is_object() && json.contains("sfsMaterialNodes");
      if (!wrapped)
      {
        std::fprintf(
            stderr, "  %s: not a pasteable graph\n", recipe.heading.c_str());
        CHECK(false);
        continue;
      }
      const nlohmann::json& saved = json["sfsMaterialNodes"];
      const std::optional<Graph> graph = Graph::fromJson(saved);
      CHECK(graph.has_value());
      if (!graph)
        continue;

      // Nothing dropped while loading: every node's type and every link's
      // pins exist.
      bool whole = graph->nodes.size() == saved["nodes"].size() &&
                   graph->links.size() == saved["links"].size();
      for (const Node& node : graph->nodes)
        whole = whole && findNodeType(node.type);
      const CompiledGraph compiled = compile(*graph);
      bool used = true;
      for (const Node& node : graph->nodes)
        used = used && (node.id == graph->output() ||
                        graph->feeds(node.id, graph->output()));
      if (!whole || !compiled.errors.empty() || !used)
      {
        std::fprintf(stderr,
                     "  %s: %s\n",
                     recipe.heading.c_str(),
                     !whole ? "nodes or links dropped (a misspelt type or pin?)"
                     : !compiled.errors.empty()
                         ? compiled.errors.front().message.c_str()
                         : "a node feeds nothing");
        CHECK(false);
      }
    }
  }

  return testing::report("materialRecipeTests");
}
