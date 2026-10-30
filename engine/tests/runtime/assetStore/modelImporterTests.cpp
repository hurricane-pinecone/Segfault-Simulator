#include "../../testHarness.h"

#include <engine/runtime/assetStore/modelImporter.h>

#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sfs;

namespace
{

std::string base64(const std::vector<uint8_t>& bytes)
{
  static const char* digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3)
  {
    const uint32_t n = (bytes[i] << 16) |
                       (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                       (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    out += digits[(n >> 18) & 63];
    out += digits[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? digits[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? digits[n & 63] : '=';
  }
  return out;
}

template <typename T>
void append(std::vector<uint8_t>& out, const std::vector<T>& values)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
  out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}

// A 2x2 image: red, green / blue, white (top row first), saved as a BMP.
void writeImage(const std::filesystem::path& file)
{
  SDL_Surface* surface =
      SDL_CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_RGBA32);
  const uint8_t pixels[2][2][4] = {{{255, 0, 0, 255}, {0, 255, 0, 255}},
                                   {{0, 0, 255, 255}, {255, 255, 255, 255}}};
  for (int y = 0; y < 2; ++y)
    std::memcpy(static_cast<uint8_t*>(surface->pixels) + y * surface->pitch,
                pixels[y],
                8);
  SDL_SaveBMP(surface, file.string().c_str());
  SDL_FreeSurface(surface);
}

// A .gltf with two meshes, each a one-triangle primitive: "Textured" uses the
// texture (an image file, nearest filtering), "Plain" a green base colour.
std::filesystem::path writeModel(const std::string& name,
                                 const std::string& image)
{
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "sfsModelImporterTests";
  std::filesystem::create_directories(folder);
  writeImage(folder / "checker.bmp");

  std::vector<uint8_t> buffer;
  append(buffer,
         std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}); // positions: 36 bytes
  append(buffer, std::vector<float>{0, 0, 1, 0, 0.25f, 0.75f}); // uvs: 24 bytes
  append(buffer,
         std::vector<uint16_t>{0, 1, 2, 0}); // indices (+ padding): 8 bytes

  const std::string gltf = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_texture_transform"],
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [{"mesh": 0}, {"mesh": 1}],
    "meshes": [
      {"name": "Textured", "primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]},
      {"name": "Plain", "primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 1}]}
    ],
    "materials": [
      {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "extensions": {"KHR_texture_transform":
                                    {"scale": [2, 3], "offset": [0.5, 0.25]}}},
                                "metallicRoughnessTexture": {"index": 0},
                                "metallicFactor": 0.75, "roughnessFactor": 0.25},
       "normalTexture": {"index": 0, "scale": 0.5},
       "alphaMode": "MASK", "alphaCutoff": 0.3},
      {"pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 0.5]}, "alphaMode": "BLEND"}
    ],
    "textures": [{"source": 0, "sampler": 0}],
    "samplers": [{"magFilter": 9728}],
    "images": [{"uri": ")" +
                           image + R"("}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 24},
      {"buffer": 0, "byteOffset": 60, "byteLength": 6}
    ],
    "buffers": [{"byteLength": 68, "uri": "data:application/octet-stream;base64,)" +
                           base64(buffer) + R"("}]
  })";

  const std::filesystem::path file = folder / name;
  std::ofstream(file) << gltf;
  return file;
}

// A one-triangle .gltf whose indices are these, and whose positions accessor
// claims `count` vertices (3 are in the buffer).
std::filesystem::path writeBrokenModel(const std::string& name,
                                       std::vector<uint16_t> indices,
                                       int count)
{
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "sfsModelImporterTests";
  std::filesystem::create_directories(folder);
  std::vector<uint8_t> buffer;
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}); // 36 bytes
  indices.resize(4);                                             // 8 bytes
  append(buffer, indices);
  const std::string gltf = R"({
    "asset": {"version": "2.0"},
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "nodes": [{"mesh": 0}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": )" +
                           std::to_string(count) + R"(, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,)" +
                           base64(buffer) + R"("}]
  })";
  const std::filesystem::path file = folder / name;
  std::ofstream(file) << gltf;
  return file;
}

std::vector<uint8_t> readFile(const std::filesystem::path& file)
{
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

int main()
{
  TEST("a textured primitive brings its UVs and image")
  {
    const auto model = importModel(writeModel("textured.gltf", "checker.bmp"));
    CHECK(model.has_value());
    if (!model)
      return testing::report("modelImporterTests");

    // Whole model: the textured triangle, then the plain one.
    const MeshData& whole = model->whole;
    CHECK(whole.sections.size() == 2u);
    CHECK(whole.sections[0].firstIndex == 0u);
    CHECK(whole.sections[0].indexCount == 3u);
    CHECK(whole.sections[1].firstIndex == 3u);
    CHECK(whole.sections[1].indexCount == 3u);
    CHECK(whole.vertices[2].uv.x == 0.25f);
    CHECK(whole.vertices[2].uv.y == 0.75f);

    const auto& texture = whole.sections[0].material.baseTexture;
    CHECK(texture != nullptr);
    if (texture)
    {
      CHECK(texture->width == 2u);
      CHECK(texture->height == 2u);
      CHECK(texture->nearest);
      // Top-left red, top-right green, bottom-left blue.
      CHECK(texture->rgba[0] == 255);
      CHECK(texture->rgba[1] == 0);
      CHECK(texture->rgba[5] == 255);
      CHECK(texture->rgba[10] == 255);
      CHECK(texture->rgba[3] == 255);
    }
  }

  TEST("materials bring metal, roughness and alpha")
  {
    const auto model = importModel(writeModel("materials.gltf", "checker.bmp"));
    CHECK(model.has_value());
    if (model)
    {
      const MeshMaterial& masked = model->whole.sections[0].material;
      CHECK(masked.metallic == 0.75f);
      CHECK(masked.roughness == 0.25f);
      CHECK(masked.alphaMode == AlphaMode::Mask);
      CHECK(masked.alphaCutoff == 0.3f);
      // KHR_texture_transform on the base texture: tiling and offset.
      CHECK(masked.tiling.x == 2.0f);
      CHECK(masked.tiling.y == 3.0f);
      CHECK(masked.offset.x == 0.5f);
      CHECK(model->whole.sections[1].material.tiling.x == 1.0f);
      CHECK(masked.metallicRoughnessTexture != nullptr);
      if (masked.metallicRoughnessTexture)
      {
        CHECK(masked.metallicRoughnessTexture->linear); // data, not colour
        CHECK(!masked.baseTexture->linear);
      }
      // The normal map, read as data, with its scale.
      CHECK(masked.normalTexture != nullptr);
      if (masked.normalTexture)
        CHECK(masked.normalTexture->linear);
      CHECK(masked.normalScale == 0.5f);
      CHECK(model->whole.sections[1].material.normalTexture == nullptr);

      const MeshMaterial& glass = model->whole.sections[1].material;
      CHECK(glass.alphaMode == AlphaMode::Blend);
      CHECK(glass.baseColor.a == 0.5f);
      CHECK(glass.metallic == 1.0f); // glTF's default when a file leaves it out
    }
  }

  TEST("a model with several meshes should keep its node tree")
  {
    const auto model = importModel(writeModel("tree.gltf", "checker.bmp"));
    CHECK(model.has_value());
    if (model)
    {
      CHECK(model->nodes.size() == 2u);
      if (model->nodes.size() == 2u)
      {
        CHECK(model->nodes[0].part == "Textured");
        CHECK(model->nodes[1].part == "Plain");
        CHECK(model->nodes[0].parent == -1);
        CHECK(model->nodes[0].local[3][0] == 0.0f); // the nodes aren't moved
      }
    }
  }

  TEST("the node tree alone should match the full import's, without the file's "
       "data")
  {
    const std::filesystem::path file =
        writeModel("treeonly.gltf", "checker.bmp");
    const auto model = importModel(file);
    const auto nodes = importModelNodes(file);
    CHECK(model.has_value() && nodes.has_value());
    if (model && nodes)
    {
      CHECK(nodes->size() == model->nodes.size());
      for (std::size_t i = 0; i < std::min(nodes->size(), model->nodes.size());
           ++i)
      {
        CHECK((*nodes)[i].name == model->nodes[i].name);
        CHECK((*nodes)[i].part == model->nodes[i].part);
        CHECK((*nodes)[i].parent == model->nodes[i].parent);
        CHECK((*nodes)[i].local == model->nodes[i].local);
      }
    }
    // The geometry isn't read: the tree comes even with the data file gone.
    std::filesystem::path data = file;
    data.replace_extension(".bin");
    if (std::filesystem::exists(data))
    {
      std::filesystem::remove(data);
      CHECK(importModelNodes(file).has_value());
    }
  }

  TEST("an untextured primitive gets an untextured section")
  {
    const auto model = importModel(writeModel("plain.gltf", "checker.bmp"));
    CHECK(model.has_value());
    if (model)
    {
      CHECK(model->whole.sections[1].material.baseTexture == nullptr);
      // The base colour is the material's, not baked into the vertices.
      CHECK(model->whole.sections[1].material.baseColor.g == 1.0f);
      CHECK(model->whole.sections[1].material.baseColor.r == 0.0f);
      CHECK(model->whole.vertices[3].color.r == 1.0f);
    }
  }

  TEST("the whole model and its parts share one decoded image")
  {
    const auto model = importModel(writeModel("shared.gltf", "checker.bmp"));
    CHECK(model.has_value());
    if (model)
    {
      CHECK(model->parts.size() == 2u);
      CHECK(model->parts[0].name == "Textured");
      CHECK(model->parts[0].mesh.sections.size() == 1u);
      CHECK(model->parts[0].mesh.sections[0].material.baseTexture ==
            model->whole.sections[0].material.baseTexture);
    }
  }

  TEST("an image in a data URI loads like a file")
  {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "sfsModelImporterTests";
    writeImage(folder / "checker.bmp");
    const std::string uri =
        "data:image/bmp;base64," + base64(readFile(folder / "checker.bmp"));
    const auto model = importModel(writeModel("embedded.gltf", uri));
    CHECK(model.has_value());
    if (model)
    {
      const auto& texture = model->whole.sections[0].material.baseTexture;
      CHECK(texture != nullptr);
      if (texture)
        CHECK(texture->width == 2u);
    }
  }

  TEST("a missing image leaves the model untextured, not unloaded")
  {
    const auto model = importModel(writeModel("missing.gltf", "nowhere.png"));
    CHECK(model.has_value());
    if (model)
    {
      CHECK(model->whole.sections.size() == 2u);
      CHECK(model->whole.sections[0].material.baseTexture == nullptr);
      CHECK(model->whole.sections[0].material.metallicRoughnessTexture ==
            nullptr);
    }
  }

  TEST("a well-formed one-triangle model should import (the broken ones' "
       "control)")
  {
    const auto model = importModel(writeBrokenModel("fine.gltf", {0, 1, 2}, 3));
    CHECK(model.has_value());
  }

  TEST("indices past the vertices should be refused, not read")
  {
    CHECK(!importModel(writeBrokenModel("indices.gltf", {0, 1, 7}, 3)));
  }

  TEST("an accessor longer than its data should be refused, not read")
  {
    CHECK(!importModel(writeBrokenModel("accessor.gltf", {0, 1, 2}, 1000)));
    CHECK(!importModelNodes(writeBrokenModel("nodes.gltf", {0, 1, 2}, 1000)));
  }

  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                              "sfsModelImporterTests");
  return testing::report("modelImporterTests");
}
