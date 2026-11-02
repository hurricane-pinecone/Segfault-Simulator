#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/rendering/visualStyle.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/rendering/visualStyles.h>
#include <engine/runtime/sceneManager/scene.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
// A project folder with two prefabs, "a" and "b".
struct Project
{
  fs::path folder = fs::temp_directory_path() / "sfsAssetStoreTests";

  Project()
  {
    fs::remove_all(folder);
    fs::create_directories(folder / "prefabs");
    for (const char* name : {"a", "b"})
      std::ofstream(folder / "prefabs" / (std::string(name) + ".prefab"))
          << R"({"formatVersion": 1, "root": {"NameComponent": {"name": ")"
          << name << R"("}}})";
  }
  ~Project() { fs::remove_all(folder); }
};
} // namespace

int main()
{
  TEST("collectUnused should let go of what wasn't asked for since "
       "beginUseTracking, and keep the rest")
  {
    const Project project;
    {
      const AssetDatabase database(project.folder,
                                   AssetDatabase::Mode::ReadWrite,
                                   AssetDatabase::projectFolders());
      const AssetRecord* a = database.findByPath("prefabs/a.prefab");
      const AssetRecord* b = database.findByPath("prefabs/b.prefab");
      CHECK(a != nullptr && b != nullptr);
      if (a && b)
      {
        AssetStore store;
        store.setDatabase(&database);
        CHECK(store.prefab(a->id).has_value()); // the first scene uses both
        CHECK(store.prefab(b->id).has_value());

        store.beginUseTracking();               // the next scene loads...
        CHECK(store.prefab(b->id).has_value()); // ...and uses only b
        CHECK(store.collectUnused() == 1u);
        CHECK(store.collectUnused() == 0u); // nothing more to let go

        // Asked for again, it's read again.
        fs::remove(project.folder / "prefabs" / "a.prefab");
        CHECK(!store.prefab(a->id).has_value());
        CHECK(store.prefab(b->id).has_value()); // kept: not read again
      }
    }
  }

  TEST("a visual style asset should load by id, edits showing next time, and "
       "the project's styles list by path")
  {
    const Project project;
    fs::create_directories(project.folder / "assets" / "styles");
    const fs::path cozy = project.folder / "assets" / "styles" / "Cozy.style";
    const fs::path real =
        project.folder / "assets" / "styles" / "Realistic.style";
    std::ofstream(cozy) << VisualStyle{}.toJson().dump();
    VisualStyle realistic;
    realistic.grading.toneMapper = ToneMapper::AgX;
    realistic.materialVariant = "realistic";
    std::ofstream(real) << realistic.toJson().dump();

    const AssetDatabase database(project.folder,
                                 AssetDatabase::Mode::ReadWrite,
                                 AssetDatabase::projectFolders());
    const AssetRecord* record =
        database.findByPath("assets/styles/Realistic.style");
    CHECK(record && record->kind == AssetKind::VisualStyle);
    AssetStore store;
    store.setDatabase(&database);
    if (record)
    {
      const std::optional<VisualStyle> loaded = store.visualStyle(record->id);
      CHECK(loaded && *loaded == realistic);
      realistic.grading.exposure = 1.0f;
      std::ofstream(real) << realistic.toJson().dump();
      const std::optional<VisualStyle> again = store.visualStyle(record->id);
      CHECK(again && again->grading.exposure == 1.0f);
    }
    // Not a style: nothing.
    const AssetRecord* prefab = database.findByPath("prefabs/a.prefab");
    CHECK(prefab && !store.visualStyle(prefab->id).has_value());

    const auto styles = visualStyleAssets(database);
    CHECK(styles.size() == 2u);
    if (styles.size() == 2u)
    {
      CHECK(styles[0].second == "assets/styles/Cozy.style");
      CHECK(styles[1].second == "assets/styles/Realistic.style");
    }

    // The development key's cycle: from none the first, then on, wrapping;
    // back the other way. Each switch takes the variant tag with it.
    testing3d::MockRenderer renderer;
    Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
    RenderSystem& render = scene.addSystem<RenderSystem>(renderer);
    render.setStyleSource(
        [&store, &database](std::string_view name) -> std::optional<VisualStyle>
        {
          for (const AssetRecord& candidate : database.records())
            if ("asset:" + candidate.id.toString() == name)
              return store.visualStyle(candidate.id);
          return std::nullopt;
        });
    CHECK(cycleVisualStyle(render, database) == "assets/styles/Cozy.style");
    CHECK(render.meshes().materialVariant().empty());
    CHECK(cycleVisualStyle(render, database) ==
          "assets/styles/Realistic.style");
    CHECK(render.meshes().materialVariant() == "realistic");
    CHECK(render.visualStyle() &&
          render.visualStyle()->grading.toneMapper == ToneMapper::AgX);
    CHECK(cycleVisualStyle(render, database) == "assets/styles/Cozy.style");
    CHECK(cycleVisualStyle(render, database, -1) ==
          "assets/styles/Realistic.style");
    render.setVisualStyle("");
    CHECK(cycleVisualStyle(render, database, -1) ==
          "assets/styles/Realistic.style");
  }

  return testing::report("assetStoreTests");
}
