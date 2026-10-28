#include "../../../testHarness.h"

#include <engine/core/rendering/3d/material.h>

using namespace sfs;

int main()
{
  TEST("a material should survive a round trip through JSON")
  {
    MaterialAsset material;
    material.baseColor = {0.5f, 0.25f, 1.0f, 0.75f};
    material.baseTexture = "asset:3f55f38a39f69719fbe27c30a302af61";
    material.metallicRoughnessTexture =
        "asset:d743b1c5fad80d5a4ce07cea1079f7f4";
    material.tiling = {4.0f, 2.0f};
    material.offset = {0.5f, -0.25f};
    material.metallic = 1.0f;
    material.roughness = 0.3f;
    material.alphaMode = AlphaMode::Mask;
    material.alphaCutoff = 0.4f;

    const auto loaded = MaterialAsset::fromJson(material.toJson());
    CHECK(loaded.has_value());
    if (loaded)
      CHECK(*loaded == material);
    CHECK(material.toJson()["alphaMode"] == "mask");
  }

  TEST("missing fields should keep their defaults")
  {
    const auto loaded = MaterialAsset::fromJson({{"roughness", 0.5}});
    CHECK(loaded.has_value());
    if (loaded)
    {
      CHECK(loaded->roughness == 0.5f);
      CHECK(loaded->metallic == 0.0f);
      CHECK(loaded->baseColor.a == 1.0f);
      CHECK(loaded->alphaMode == AlphaMode::Opaque);
      CHECK(loaded->baseTexture.empty());
      CHECK(loaded->tiling.x == 1.0f);
      CHECK(loaded->offset.y == 0.0f);
    }
  }

  TEST("bad values should be ignored, and non-objects refused")
  {
    const auto loaded = MaterialAsset::fromJson({{"alphaMode", "sparkly"},
                                                 {"metallic", "very"},
                                                 {"baseColor", {1, 2}}});
    CHECK(loaded.has_value());
    if (loaded)
    {
      CHECK(loaded->alphaMode == AlphaMode::Opaque);
      CHECK(loaded->metallic == 0.0f);
      CHECK(loaded->baseColor.r == 1.0f);
    }
    CHECK(!MaterialAsset::fromJson(nlohmann::json::array()).has_value());
  }

  TEST("alpha mode names should round-trip")
  {
    for (const AlphaMode mode :
         {AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend})
      CHECK(alphaModeFromName(alphaModeName(mode)) == mode);
    CHECK(!alphaModeFromName("glass").has_value());
  }

  return testing::report("materialTests");
}
