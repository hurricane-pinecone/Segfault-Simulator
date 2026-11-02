#include "../../testHarness.h"

#include <engine/core/rendering/material.h>
#include <engine/core/rendering/terrain.h>

using namespace sfs;

int main()
{
  TEST("a material's variants should round-trip, and only be written when "
       "it has some")
  {
    MaterialAsset material;
    CHECK(!material.toJson().contains("variants"));
    material.variants["realistic"] = "asset:9c1e0000000000000000000000000001";
    const auto loaded = MaterialAsset::fromJson(material.toJson());
    CHECK(loaded && *loaded == material);

    // A graph material keeps its too.
    material.graph = material.toGraph();
    const auto graph = MaterialAsset::fromJson(material.toJson());
    CHECK(graph && graph->variants == material.variants);

    // Empty tags and names, and non-strings, are dropped.
    const auto odd = MaterialAsset::fromJson(
        {{"variants",
          {{"", "asset:x"}, {"a", ""}, {"b", 3}, {"c", "asset:y"}}}});
    CHECK(odd && odd->variants.size() == 1 &&
          odd->variants.at("c") == "asset:y");
  }

  TEST("a material should survive a round trip through JSON")
  {
    MaterialAsset material;
    material.baseColor = {0.5f, 0.25f, 1.0f, 0.75f};
    material.baseTexture = "asset:3f55f38a39f69719fbe27c30a302af61";
    material.metallicRoughnessTexture =
        "asset:d743b1c5fad80d5a4ce07cea1079f7f4";
    material.normalTexture = "asset:0e5b2c37a1d94f6b8f3c2a1d9e8f7a6b";
    material.normalScale = 1.5f;
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
      CHECK(loaded->normalTexture.empty());
      CHECK(loaded->normalScale == 1.0f);
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
    for (const AlphaMode mode : {AlphaMode::Opaque,
                                 AlphaMode::Mask,
                                 AlphaMode::Blend,
                                 AlphaMode::Additive})
      CHECK(alphaModeFromName(alphaModeName(mode)) == mode);
    CHECK(!alphaModeFromName("glass").has_value());
  }

  TEST("a terrain material should round-trip, its layers kept in order and "
       "at most five")
  {
    MaterialAsset material;
    TerrainAsset terrain;
    terrain.splatMap = "asset:00000000000000000000000000000abc";
    terrain.blendSharpness = 0.7f;
    terrain.antiTiling = false;
    terrain.variation = 0.2f;
    terrain.triplanarSharpness = 0.8f;
    terrain.layers.push_back({.name = "Grass",
                              .baseTexture = "asset:1",
                              .tint = {0.9f, 1.0f, 0.8f},
                              .tileSize = 3.0f,
                              .roughness = 0.9f,
                              .meadow = 0.8f});
    terrain.layers.push_back({.name = "Dirt",
                              .normalTexture = "asset:2",
                              .grass = 0.0f,
                              .triplanar = true});
    material.terrain = terrain;
    material.variants["realistic"] = "asset:3";
    const nlohmann::json saved = material.toJson();
    CHECK(saved.contains("terrain") && !saved.contains("baseColor"));
    const auto loaded = MaterialAsset::fromJson(saved);
    CHECK(loaded && *loaded == material);
    CHECK(loaded && loaded->terrain->layers[1].name == "Dirt");
    CHECK(loaded && loaded->terrain->layers[0].meadow == 0.8f &&
          loaded->terrain->layers[1].meadow == 0.0f);
    CHECK(loaded && !loaded->terrain->layers[0].triplanar &&
          loaded->terrain->layers[1].triplanar &&
          loaded->terrain->triplanarSharpness == 0.8f);

    // More layers than there can be: the first five.
    nlohmann::json many = saved;
    for (int i = 0; i < 6; ++i)
      many["terrain"]["layers"].push_back({{"name", "Extra"}});
    const auto clipped = MaterialAsset::fromJson(many);
    CHECK(clipped && clipped->terrain->layers.size() == kMaxTerrainLayers);
    // A plain material has none.
    CHECK(!MaterialAsset::fromJson(MaterialAsset{}.toJson())->terrain);
  }

  TEST("a splat map's weights should sum to one, painting should move them "
       "and never past one, and grass grow where its layers let it")
  {
    const auto w = terrainWeights({0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(w[0] == 1.0f && w[1] == 0.0f);
    const auto half = terrainWeights({0.5f, 0.0f, 0.0f, 0.0f});
    CHECK(testing::approx(half[0], 0.5f) && testing::approx(half[1], 0.5f));
    const auto over = terrainWeights({1.0f, 1.0f, 0.0f, 0.0f});
    CHECK(over[0] == 0.0f && testing::approx(over[1], 0.5f) &&
          testing::approx(over[2], 0.5f));

    auto splat = blankSplat(32);
    // Layer 2 (red) in the middle, fully; untouched at the edge.
    const auto changed =
        paintSplat(*splat, {0.5f, 0.5f}, {0.25f, 0.25f}, 1, 1.0f, 0.5f);
    CHECK(changed.second.x > changed.first.x);
    CHECK(sampleSplat(*splat, {0.5f, 0.5f}).r > 0.99f);
    CHECK(sampleSplat(*splat, {0.05f, 0.05f}).r == 0.0f);
    // Then layer 3 over it: red goes as green comes, never past one.
    paintSplat(*splat, {0.5f, 0.5f}, {0.25f, 0.25f}, 2, 0.5f, 0.0f);
    const glm::vec4 mixed = sampleSplat(*splat, {0.5f, 0.5f});
    CHECK(testing::approx(mixed.r, 0.5f, 0.02f) &&
          testing::approx(mixed.g, 0.5f, 0.02f));
    CHECK(mixed.r + mixed.g + mixed.b + mixed.a <= 1.0f + 1e-3f);
    // The first layer: everything back toward none.
    paintSplat(*splat, {0.5f, 0.5f}, {0.25f, 0.25f}, 0, 1.0f, 0.0f);
    CHECK(sampleSplat(*splat, {0.5f, 0.5f}) == glm::vec4(0.0f));
    // Outside the map, or no layer: nothing.
    CHECK(paintSplat(*splat, {3.0f, 3.0f}, {0.1f, 0.1f}, 1, 1.0f).second ==
          glm::ivec2(0));

    TerrainMaterial terrain;
    CHECK(terrainGrass(terrain, {0.5f, 0.5f}) == 1.0f);
    terrain.layers = {TerrainLayer{}, TerrainLayer{.grass = 0.0f}};
    CHECK(terrainLimitsGrass(terrain));
    terrain.splat = splat;
    paintSplat(*splat, {0.5f, 0.5f}, {0.25f, 0.25f}, 1, 1.0f, 0.0f);
    CHECK(terrainGrass(terrain, {0.5f, 0.5f}) < 0.01f);
    CHECK(terrainGrass(terrain, {0.05f, 0.05f}) == 1.0f);
  }

  return testing::report("materialTests");
}
