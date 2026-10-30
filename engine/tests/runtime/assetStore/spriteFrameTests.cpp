#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/assetStore/textureImporter.h>

#include <filesystem>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
// A project with one 64 x 32 image in it.
struct Project
{
  fs::path root = fs::temp_directory_path() / "sfs_sprite_frames";
  Project()
  {
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    TextureData image;
    image.width = 64;
    image.height = 32;
    image.rgba.assign(64 * 32 * 4, 255);
    saveTexturePng(image, root / "assets/sheet.png");
  }
  ~Project() { fs::remove_all(root); }
};
} // namespace

int main()
{
  TEST("a texture with a sprite grid should give its frames, across then down")
  {
    Project project;
    AssetDatabase catalogue(
        project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    const AssetRecord* sheet = catalogue.findByPath("assets/sheet.png");
    CHECK(sheet != nullptr);
    if (!sheet)
      return testing::report("spriteFrameTests");
    const AssetId id = sheet->id;
    const std::string name = "asset:" + id.toString();
    AssetStore store;
    store.setDatabase(&catalogue);

    // No grid: the whole image.
    std::optional<AssetStore::SpriteFrame> whole = store.spriteFrame(name, 3);
    CHECK(whole.has_value());
    CHECK(whole && testing::approx(whole->pixels.x, 64.0f));
    CHECK(whole && testing::approx(whole->uvRect.z, 1.0f));

    // 16 x 16 cells: 4 across, 2 down.
    catalogue.setSettings(id, {{"spriteCell", {16, 16}}});
    std::optional<AssetStore::SpriteFrame> sixth = store.spriteFrame(name, 5);
    CHECK(sixth && testing::approx(sixth->pixels.x, 16.0f));
    CHECK(sixth && testing::approx(sixth->uvRect.x, 0.25f)); // column 1
    CHECK(sixth && testing::approx(sixth->uvRect.y, 0.5f));  // row 1
    CHECK(sixth && testing::approx(sixth->uvRect.z, 0.25f));
    // Past the last frame wraps round to the first.
    std::optional<AssetStore::SpriteFrame> wrapped = store.spriteFrame(name, 8);
    CHECK(wrapped && testing::approx(wrapped->uvRect.x, 0.0f) &&
          testing::approx(wrapped->uvRect.y, 0.0f));

    CHECK(!store.spriteFrame("asset:" + AssetId{}.toString(), 0).has_value());
  }

  return testing::report("spriteFrameTests");
}
