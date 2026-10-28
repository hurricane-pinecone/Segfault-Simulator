#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
// A fresh, empty folder that removes itself.
struct TempFolder
{
  fs::path path;
  explicit TempFolder(const char* name) : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempFolder() { fs::remove_all(path); }
};

void touch(const fs::path& file, const char* text = "x")
{
  fs::create_directories(file.parent_path());
  std::ofstream(file) << text;
}
} // namespace

int main()
{
  TEST("ids should round-trip through text and be random")
  {
    const AssetId id = AssetId::generate();
    CHECK(id.valid());
    CHECK(id.toString().size() == 32u);
    CHECK(AssetId::parse(id.toString()) == id);
    CHECK(AssetId::generate() != id);
    CHECK(!AssetId::parse("not an id").has_value());
    CHECK(!AssetId::parse("0123456789abcdef0123456789abcdeg").has_value());
  }

  TEST("kinds should come from extensions, ignoring case")
  {
    CHECK(assetKindOf("a/tree.PNG") == AssetKind::Texture);
    CHECK(assetKindOf("barn.glb") == AssetKind::Model);
    CHECK(assetKindOf("ui.ttf") == AssetKind::Font);
    CHECK(assetKindOf("hit.wav") == AssetKind::Audio);
    CHECK(assetKindOf("atlas.json") == AssetKind::Data);
    CHECK(assetKindOf("notes.xyz") == AssetKind::Other);
  }

  TEST("a scan should give every file a .meta with an id that lasts")
  {
    TempFolder root("sfs_assets_scan");
    touch(root.path / "tree.png");
    touch(root.path / "models/barn.glb");
    touch(root.path / ".hidden/skip.png");

    AssetDatabase database(root.path);
    CHECK(database.records().size() == 2u);
    CHECK(fs::exists(root.path / "tree.png.meta"));
    const AssetRecord* barn = database.findByPath("models/barn.glb");
    CHECK(barn && barn->kind == AssetKind::Model);
    CHECK(database.folders() == std::vector<fs::path>{"models"});
    const AssetId barnId = barn->id;

    AssetDatabase reopened(root.path);
    CHECK(reopened.findByPath("models/barn.glb")->id == barnId);
    CHECK(reopened.find(barnId)->path == fs::path("models/barn.glb"));
    CHECK(!reopened.refresh()); // nothing changed
  }

  TEST("a project catalogue should cover only its assets/ and prefabs/ folders")
  {
    TempFolder root("sfs_assets_project");
    touch(root.path / "assets/models/barn.glb");
    touch(root.path / "prefabs/Fence.prefab");
    touch(root.path / "scripts/Spin.cpp");
    touch(root.path / "scenes/farm.scene");

    AssetDatabase database(root.path,
                           AssetDatabase::Mode::ReadWrite,
                           AssetDatabase::projectFolders());
    CHECK(database.records().size() == 2u);
    CHECK(database.findByPath("assets/models/barn.glb") != nullptr);
    const AssetRecord* fence = database.findByPath("prefabs/Fence.prefab");
    CHECK(fence && fence->kind == AssetKind::Prefab);
    CHECK(!fs::exists(root.path / "scripts/Spin.cpp.meta")); // not catalogued
    CHECK(database.folders() ==
          (std::vector<fs::path>{"assets", "assets/models", "prefabs"}));

    // A missing folder is skipped; a shipped game reads it the same way.
    fs::remove_all(root.path / "prefabs");
    AssetDatabase shipped(root.path,
                          AssetDatabase::Mode::ReadOnly,
                          AssetDatabase::projectFolders());
    CHECK(shipped.records().size() == 1u);
    CHECK(shipped.folders() ==
          (std::vector<fs::path>{"assets", "assets/models"}));
  }

  TEST("edits should leave the catalogue just as a fresh scan finds it")
  {
    TempFolder root("sfs_assets_inplace");
    TempFolder outside("sfs_assets_inplace_outside");
    touch(outside.path / "rock.png");
    touch(root.path / "assets/props/crate.png");
    touch(root.path / "assets/props/barrel.png");
    touch(root.path / "assets/old/junk.png");
    touch(root.path / "prefabs/Fence.prefab");
    AssetDatabase database(root.path,
                           AssetDatabase::Mode::ReadWrite,
                           AssetDatabase::projectFolders());
    const uint64_t version = database.version();

    CHECK(database.move(database.findByPath("assets/props/crate.png")->id,
                        "assets/world/deep/box.png"));
    CHECK(database.moveFolder("assets/props", "assets/world/props"));
    CHECK(database.remove(database.findByPath("prefabs/Fence.prefab")->id));
    CHECK(database.removeFolder("assets/old"));
    CHECK(database.createFolder("assets/sounds/ui"));
    const std::optional<AssetId> rock =
        database.import(outside.path / "rock.png", "assets/world");
    CHECK(rock.has_value());
    CHECK(database.version() > version);

    AssetDatabase fresh(root.path,
                        AssetDatabase::Mode::ReadOnly,
                        AssetDatabase::projectFolders());
    CHECK(database.folders() == fresh.folders());
    CHECK(database.records().size() == fresh.records().size());
    for (std::size_t i = 0;
         i < std::min(database.records().size(), fresh.records().size());
         ++i)
    {
      CHECK(database.records()[i].path == fresh.records()[i].path);
      CHECK(database.records()[i].id == fresh.records()[i].id);
      CHECK(database.records()[i].kind == fresh.records()[i].kind);
    }
    CHECK(rock && fresh.find(*rock) != nullptr);
    CHECK(!database.refresh()); // nothing for a rescan to find
  }

  TEST("moving, renaming and moving folders should keep ids")
  {
    TempFolder root("sfs_assets_move");
    touch(root.path / "props/crate.png");
    touch(root.path / "props/barrel.png");
    AssetDatabase database(root.path);
    const AssetId crate = database.findByPath("props/crate.png")->id;
    const AssetId barrel = database.findByPath("props/barrel.png")->id;

    CHECK(database.move(crate, "props/box.png"));
    CHECK(database.find(crate)->path == fs::path("props/box.png"));
    CHECK(fs::exists(root.path / "props/box.png.meta"));
    CHECK(!fs::exists(root.path / "props/crate.png.meta"));
    CHECK(!database.move(crate, "props/barrel.png")); // taken

    CHECK(database.moveFolder("props", "world/props"));
    CHECK(database.find(crate)->path == fs::path("world/props/box.png"));
    CHECK(database.find(barrel)->path == fs::path("world/props/barrel.png"));
    CHECK(!database.moveFolder("world", "world/inner"));     // into itself
    CHECK(AssetDatabase(root.path).find(barrel) != nullptr); // on disk too
  }

  TEST("a copied file and .meta should get an id of its own")
  {
    TempFolder root("sfs_assets_copy");
    touch(root.path / "a.png");
    AssetDatabase database(root.path);
    const AssetId original = database.findByPath("a.png")->id;
    fs::copy_file(root.path / "a.png", root.path / "b.png");
    fs::copy_file(root.path / "a.png.meta", root.path / "b.png.meta");

    CHECK(database.refresh());
    CHECK(database.findByPath("a.png")->id == original); // first path keeps it
    CHECK(database.findByPath("b.png")->id != original);
    CHECK(database.findByPath("b.png")->id.valid());
  }

  TEST("delete, new folder and import should update the catalogue and disk")
  {
    TempFolder root("sfs_assets_edit");
    TempFolder outside("sfs_assets_outside");
    touch(outside.path / "tree.png");
    touch(root.path / "tree.png");
    AssetDatabase database(root.path);

    const std::optional<AssetId> copy =
        database.import(outside.path / "tree.png", "");
    CHECK(copy.has_value());
    CHECK(database.find(*copy)->path ==
          fs::path("tree 2.png")); // name was taken
    CHECK(database.createFolder("sounds"));
    CHECK(database.folders() == std::vector<fs::path>{"sounds"});

    CHECK(database.remove(*copy));
    CHECK(database.find(*copy) == nullptr);
    CHECK(!fs::exists(root.path / "tree 2.png.meta"));
    CHECK(database.removeFolder("sounds"));
    CHECK(database.folders().empty());
  }

  TEST("import settings should be saved in the .meta")
  {
    TempFolder root("sfs_assets_settings");
    touch(root.path / "grass.png");
    AssetDatabase database(root.path);
    const AssetId id = database.findByPath("grass.png")->id;
    CHECK(database.setSettings(id, {{"filter", "nearest"}}));
    CHECK(AssetDatabase(root.path).find(id)->settings["filter"] == "nearest");
  }

  TEST("a read-only database should never write")
  {
    TempFolder root("sfs_assets_readonly");
    touch(root.path / "tree.png");
    AssetDatabase database(root.path, AssetDatabase::Mode::ReadOnly);
    CHECK(!fs::exists(root.path / "tree.png.meta"));
    CHECK(database.records().size() == 1u);
    CHECK(!database.records()[0]
               .id.valid()); // listed, but has no id without a .meta
    CHECK(!database.createFolder("x"));
    CHECK(!fs::exists(root.path / "x"));
  }

  return testing::report("assetDatabaseTests");
}
