#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/runtime/assetStore/assetStore.h>

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

  return testing::report("assetStoreTests");
}
