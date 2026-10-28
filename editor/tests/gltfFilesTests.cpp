#include "../../engine/tests/testHarness.h"

#include "assets/gltfFiles.h"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
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

void write(const fs::path& file, const std::string& text)
{
  fs::create_directories(file.parent_path());
  std::ofstream(file) << text;
}

json readJson(const fs::path& file)
{
  std::ifstream in(file);
  return json::parse(in, nullptr, false);
}

// A project with a model (a.gltf + a.bin) using a texture beside it.
void writeModel(const fs::path& root)
{
  write(root / "assets/models/a.gltf",
        json({{"buffers", json::array({{{"uri", "a.bin"}, {"byteLength", 4}}})},
              {"images", json::array({{{"uri", "tex.png"}}, {{"uri", "data:image/png;base64,AAAA"}}})}})
            .dump());
  write(root / "assets/models/a.bin", "data");
  write(root / "assets/models/tex.png", "png");
}
} // namespace

int main()
{
  TEST("a .gltf's .bin should count as its companion, not an asset to show")
  {
    TempFolder root("sfs_gltf_companion");
    writeModel(root.path);
    sfs::AssetDatabase assets(root.path, sfs::AssetDatabase::Mode::ReadWrite, sfs::AssetDatabase::projectFolders());
    const sfs::AssetRecord* model = assets.findByPath("assets/models/a.gltf");
    const sfs::AssetRecord* data = assets.findByPath("assets/models/a.bin");
    CHECK(model && data);
    CHECK(data && gltfFiles::isCompanion(assets, *data));
    CHECK(model && !gltfFiles::isCompanion(assets, *model));
    CHECK(model && gltfFiles::companionOf(assets, *model) == data);
  }

  TEST("moving a .gltf should take its .bin and keep its texture found")
  {
    TempFolder root("sfs_gltf_move");
    writeModel(root.path);
    sfs::AssetDatabase assets(root.path, sfs::AssetDatabase::Mode::ReadWrite, sfs::AssetDatabase::projectFolders());
    const sfs::AssetId model = assets.findByPath("assets/models/a.gltf")->id;
    const sfs::AssetId data = assets.findByPath("assets/models/a.bin")->id;

    CHECK(gltfFiles::move(assets, model, "assets/props/crate.gltf"));
    CHECK(assets.find(model)->path == fs::path("assets/props/crate.gltf"));
    CHECK(assets.find(data)->path == fs::path("assets/props/crate.bin")); // same id, renamed to match
    CHECK(fs::exists(root.path / "assets/props/crate.bin"));
    CHECK(!fs::exists(root.path / "assets/models/a.bin"));
    const json gltf = readJson(root.path / "assets/props/crate.gltf");
    CHECK(gltf["buffers"][0]["uri"] == "crate.bin");
    CHECK(gltf["images"][0]["uri"] == "../models/tex.png"); // still there, found from here
    CHECK(gltf["images"][1]["uri"] == "data:image/png;base64,AAAA");
    CHECK(fs::exists((root.path / "assets/props" / gltf["images"][0]["uri"].get<std::string>()).lexically_normal()));
  }

  TEST("renaming a .gltf in place should rename its .bin and leave its textures alone")
  {
    TempFolder root("sfs_gltf_rename");
    writeModel(root.path);
    sfs::AssetDatabase assets(root.path, sfs::AssetDatabase::Mode::ReadWrite, sfs::AssetDatabase::projectFolders());
    CHECK(gltfFiles::move(assets, assets.findByPath("assets/models/a.gltf")->id, "assets/models/barn.gltf"));
    const json gltf = readJson(root.path / "assets/models/barn.gltf");
    CHECK(gltf["buffers"][0]["uri"] == "barn.bin");
    CHECK(gltf["images"][0]["uri"] == "tex.png");
    CHECK(assets.findByPath("assets/models/barn.bin") != nullptr);
  }

  TEST("deleting a .gltf should delete its .bin, not other files")
  {
    TempFolder root("sfs_gltf_delete");
    writeModel(root.path);
    sfs::AssetDatabase assets(root.path, sfs::AssetDatabase::Mode::ReadWrite, sfs::AssetDatabase::projectFolders());
    CHECK(gltfFiles::remove(assets, assets.findByPath("assets/models/a.gltf")->id));
    CHECK(!fs::exists(root.path / "assets/models/a.bin"));
    CHECK(assets.findByPath("assets/models/a.bin") == nullptr);
    CHECK(fs::exists(root.path / "assets/models/tex.png"));
  }

  TEST("other assets should move as usual")
  {
    TempFolder root("sfs_gltf_other");
    writeModel(root.path);
    sfs::AssetDatabase assets(root.path, sfs::AssetDatabase::Mode::ReadWrite, sfs::AssetDatabase::projectFolders());
    CHECK(gltfFiles::move(assets, assets.findByPath("assets/models/tex.png")->id, "assets/textures/tex.png"));
    CHECK(fs::exists(root.path / "assets/textures/tex.png"));
    CHECK(fs::exists(root.path / "assets/models/a.bin")); // untouched
  }

  return testing::report("gltfFilesTests");
}
