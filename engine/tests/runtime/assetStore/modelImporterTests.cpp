#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/logger/logger.h>
#include <engine/core/reflection/reflection.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/assetStore/modelData.h>
#include <engine/runtime/assetStore/modelImporter.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/serialization/prefabComponents.h>
#include <engine/runtime/serialization/sceneSerializer.h>
#include <engine/runtime/serialization/unresolvedComponents.h>

#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

// A one-mesh .gltf skinned to two joints (a rig's "hip" and its "knee"),
// with a clip moving them: the hip up one (glTF's +y), linearly, and the knee
// a quarter turn about +y, in steps.
std::filesystem::path writeSkinnedModel(const std::string& name)
{
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "sfsModelImporterTests";
  std::filesystem::create_directories(folder);
  std::vector<uint8_t> buffer;
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}); // 36 bytes
  append(
      buffer, std::vector<uint8_t>{0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0}); // 12
  append(buffer,
         std::vector<float>{1, 0, 0, 0, 2, 2, 0, 0, 1, 0, 0, 0}); // 48 bytes
  append(buffer, std::vector<uint16_t>{0, 1, 2, 0});              // 8 bytes
  append(buffer,
         std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1,
                            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,  0, 1});
  append(buffer, std::vector<float>{0, 1});             // times: 8 bytes
  append(buffer, std::vector<float>{0, 0, 0, 0, 1, 0}); // 24 bytes
  append(buffer,
         std::vector<float>{0, 0, 0, 1, 0, 0.70710678f, 0, 0.70710678f}); // 32
  const std::string gltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [
      {"name": "Rig", "children": [1, 3]},
      {"name": "hip", "translation": [0, 1, 0], "children": [2]},
      {"name": "knee"},
      {"name": "Body", "mesh": 0, "skin": 0}
    ],
    "meshes": [{"name": "Body", "primitives": [{"attributes":
      {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}],
    "skins": [{"joints": [1, 2], "inverseBindMatrices": 4}],
    "animations": [{"name": "Walk",
      "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}},
                   {"sampler": 1, "target": {"node": 2, "path": "rotation"}}],
      "samplers": [{"input": 5, "output": 6},
                   {"input": 5, "output": 7, "interpolation": "STEP"}]}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
      {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4"},
      {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
      {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
      {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"},
      {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
      {"bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC3"},
      {"bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC4"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 12},
      {"buffer": 0, "byteOffset": 48, "byteLength": 48},
      {"buffer": 0, "byteOffset": 96, "byteLength": 6},
      {"buffer": 0, "byteOffset": 104, "byteLength": 128},
      {"buffer": 0, "byteOffset": 232, "byteLength": 8},
      {"buffer": 0, "byteOffset": 240, "byteLength": 24},
      {"buffer": 0, "byteOffset": 264, "byteLength": 32}
    ],
    "buffers": [{"byteLength": 296, "uri": "data:application/octet-stream;base64,)" +
                           base64(buffer) + R"("}]
  })";
  const std::filesystem::path file = folder / name;
  std::ofstream(file) << gltf;
  return file;
}

// A .gltf with a drawn "Body" (one triangle) and two collider nodes:
// "Boom-collider.001" under "Boom" (moved up glTF's y), its positions' min and
// max written down, and "Cab-COLLIDER", the same triangle without them (so its
// bounds come from the data). The colliders' triangle spans x 0..2, y -2..2,
// z -3..3 in glTF's axes.
std::filesystem::path writeColliderModel(const std::string& name)
{
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "sfsModelImporterTests";
  std::filesystem::create_directories(folder);
  std::vector<uint8_t> buffer;
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});    // 36 bytes
  append(buffer, std::vector<float>{0, -2, -3, 2, 2, 3, 2, -2, 3}); // 36 bytes
  append(buffer, std::vector<uint16_t>{0, 1, 2, 0});                // 8 bytes
  const std::string gltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 3]}],
    "nodes": [
      {"name": "Body", "mesh": 0, "children": [1]},
      {"name": "Boom", "translation": [0, 1, 0], "children": [2]},
      {"name": "Boom-collider.001", "mesh": 1},
      {"name": "Cab-COLLIDER", "mesh": 2}
    ],
    "meshes": [
      {"name": "Body", "primitives": [{"attributes": {"POSITION": 0}, "indices": 3}]},
      {"name": "BoomBox", "primitives": [{"attributes": {"POSITION": 1}, "indices": 3}]},
      {"name": "CabBox", "primitives": [{"attributes": {"POSITION": 2}, "indices": 3}]}
    ],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, -2, -3], "max": [2, 2, 3]},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 36},
      {"buffer": 0, "byteOffset": 72, "byteLength": 6}
    ],
    "buffers": [{"byteLength": 80, "uri": "data:application/octet-stream;base64,)" +
                           base64(buffer) + R"("}]
  })";
  const std::filesystem::path file = folder / name;
  std::ofstream(file) << gltf;
  return file;
}

// A .gltf with two one-triangle meshes ("A" and "B") and these nodes: `nodes`
// is the file's "nodes" array (a node may draw mesh 0 or 1), `tops` its
// scene's top nodes, e.g. "[0, 2]".
std::filesystem::path writeTreeModel(const std::filesystem::path& name,
                                     const std::string& nodes,
                                     const std::string& tops)
{
  const std::filesystem::path folder =
      std::filesystem::temp_directory_path() / "sfsModelImporterTests";
  std::vector<uint8_t> buffer;
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}); // 36 bytes
  append(buffer, std::vector<uint16_t>{0, 1, 2, 0});             // 8 bytes
  const std::string gltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": )" +
                           tops + R"(}],
    "nodes": )" + nodes + R"(,
    "meshes": [
      {"name": "A", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]},
      {"name": "B", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}
    ],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
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
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file) << gltf;
  return file;
}

// A model's nodes' ids by name path; empty if it doesn't import.
std::map<std::string, uint32_t> idsByPath(const std::filesystem::path& file)
{
  std::vector<std::string> paths;
  const auto nodes = importModelNodes(file, {}, &paths);
  std::map<std::string, uint32_t> ids;
  if (nodes && paths.size() == nodes->size())
    for (std::size_t i = 0; i < paths.size(); ++i)
      ids[paths[i]] = (*nodes)[i].id;
  return ids;
}

// Every warning logged while it's alive.
struct Warnings
{
  std::vector<std::string> messages;
  Logger::SinkId sink = Logger::addSink(
      [this](Logger::Level level, const std::string& message)
      {
        if (level == Logger::Level::WARN)
          messages.push_back(message);
      });
  ~Warnings() { Logger::removeSink(sink); }

  bool any(const std::string& text) const
  {
    return std::any_of(messages.begin(),
                       messages.end(),
                       [&](const std::string& message)
                       { return message.find(text) != std::string::npos; });
  }
};

// A body with an arm (and its hand) and a leg, and a cab beside it.
constexpr const char* kTreeNodes = R"([
  {"name": "Body", "mesh": 0, "children": [1, 3]},
  {"name": "Arm", "children": [2]},
  {"name": "Hand"},
  {"name": "Leg"},
  {"name": "Cab", "mesh": 1}
])";

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

  TEST("a skin and its clips should come in, in the engine's axes")
  {
    const std::filesystem::path file = writeSkinnedModel("skinned.gltf");
    const auto model = importModel(file);
    CHECK(model.has_value());
    if (model)
    {
      // Even one mesh is a part, placed in the node tree with its joints.
      CHECK(model->parts.size() == 1u);
      CHECK(model->nodes.size() == 4u);
      const MeshData& body = model->parts.front().mesh;
      CHECK(body.skinned());
      if (body.skinned())
      {
        CHECK(body.skin->joints == std::vector<std::string>{"hip", "knee"});
        // The hip's inverse bind: down one along glTF's y, the engine's z.
        const glm::vec4 moved =
            body.skin->inverseBind[0] * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        CHECK(testing::approx(moved.z, 0.0f));
        CHECK(testing::approx(moved.y, 0.0f));
        // Weights normalised; joints in order.
        CHECK(testing::approx(body.skinWeights[1].weights.x, 0.5f));
        CHECK(testing::approx(body.skinWeights[1].weights.y, 0.5f));
        CHECK(body.skinWeights[1].joints[1] == 1);
        CHECK(body.skinWeights[2].joints[0] == 1);
        CHECK(!body.skin->jointBounds[1].empty());
      }
      CHECK(model->animations != nullptr);
      const AnimationClip* walk =
          model->animations ? model->animations->find("Walk") : nullptr;
      CHECK(walk != nullptr);
      if (walk)
      {
        CHECK(testing::approx(walk->duration, 1.0f));
        CHECK(walk->channels.size() == 2u);
        // Up glTF's y: up the engine's z.
        const glm::vec4 up = sampleChannel(walk->channels[0], 1.0f);
        CHECK(testing::approx(up.z, 1.0f));
        CHECK(testing::approx(up.y, 0.0f));
        CHECK(walk->channels[0].node == "hip");
        // A turn about glTF's y: about the engine's z, held until its key.
        CHECK(walk->channels[1].interpolation == AnimationInterpolation::Step);
        const glm::vec4 turn = sampleChannel(walk->channels[1], 1.0f);
        CHECK(testing::approx(turn.z, 0.70710678f));
        CHECK(testing::approx(turn.y, 0.0f));
        CHECK(testing::approx(sampleChannel(walk->channels[1], 0.5f).w, 1.0f));
      }
    }
    const auto nodes = importModelNodes(file);
    CHECK(nodes.has_value() && nodes->size() == 4u);
  }

  TEST("only nodes named \"<name>-collider\" should be colliders")
  {
    CHECK(isColliderNodeName("Boom-collider"));
    CHECK(isColliderNodeName("boom-Collider"));
    CHECK(isColliderNodeName("Boom-collider.001")); // a Blender copy
    CHECK(!isColliderNodeName("Boom"));
    CHECK(!isColliderNodeName("Collider"));
    CHECK(!isColliderNodeName("-collider"));
    CHECK(!isColliderNodeName("Boom-colliders"));
    CHECK(!isColliderNodeName("Boom-collider.v2"));
    CHECK(!isColliderNodeName("Boom_collider"));
  }

  TEST("a collider node should come in as a collider around its mesh, in the "
       "engine's axes, its mesh not drawn")
  {
    const std::filesystem::path file = writeColliderModel("collider.gltf");
    const auto model = importModel(file);
    CHECK(model.has_value());
    if (model)
    {
      // Neither the whole model nor the parts draw the colliders.
      CHECK(model->whole.vertices.size() == 3u);
      CHECK(model->parts.size() == 1u);
      CHECK(model->parts.size() == 1u && model->parts[0].name == "Body");
      CHECK(model->nodes.size() == 4u);
      if (model->nodes.size() == 4u)
      {
        const ModelNode& body = model->nodes[0];
        const ModelNode& boom = model->nodes[2];
        const ModelNode& cab = model->nodes[3];
        CHECK(!body.collider && body.part == "Body");
        CHECK(boom.name == "Boom-collider.001");
        CHECK(boom.collider && boom.part.empty());
        CHECK(model->nodes[2].parent == 1); // still under Boom
        // glTF (x, y, z) is the engine's (x, -z, y): x 0..2, y -3..3, z -2..2.
        CHECK(testing::approx(boom.bounds.min.x, 0.0f));
        CHECK(testing::approx(boom.bounds.max.x, 2.0f));
        CHECK(testing::approx(boom.bounds.min.y, -3.0f));
        CHECK(testing::approx(boom.bounds.max.y, 3.0f));
        CHECK(testing::approx(boom.bounds.min.z, -2.0f));
        CHECK(testing::approx(boom.bounds.max.z, 2.0f));
        // Without a min and max written down: the same, from its data.
        CHECK(cab.collider && cab.part.empty());
        CHECK(testing::approx(cab.bounds.min.y, -3.0f));
        CHECK(testing::approx(cab.bounds.max.z, 2.0f));
      }
    }
    // The tree alone (the model placed before its geometry loads) agrees.
    const auto nodes = importModelNodes(file);
    CHECK(nodes.has_value() && nodes->size() == 4u);
    if (nodes && model && nodes->size() == model->nodes.size())
      for (std::size_t i = 0; i < nodes->size(); ++i)
      {
        CHECK((*nodes)[i].collider == model->nodes[i].collider);
        CHECK((*nodes)[i].bounds.min == model->nodes[i].bounds.min);
        CHECK((*nodes)[i].bounds.max == model->nodes[i].bounds.max);
      }
  }

  TEST("a placed model's collider node should be an entity with a box "
       "collider fitted to its mesh and no mesh renderer, scaled with the "
       "import")
  {
    // A project with the model, imported at twice its size.
    const std::filesystem::path project =
        std::filesystem::temp_directory_path() / "sfsModelImporterTests" /
        "project";
    std::filesystem::create_directories(project / "assets");
    std::filesystem::copy_file(
        writeColliderModel("sprayer.gltf"),
        project / "assets" / "sprayer.gltf",
        std::filesystem::copy_options::overwrite_existing);
    std::ofstream(project / "assets" / "sprayer.gltf.meta")
        << R"({"formatVersion": 1, "id": "0123456789abcdef0123456789abcdef",
               "settings": {"scale": 2}})";
    const AssetDatabase database(project,
                                 AssetDatabase::Mode::ReadOnly,
                                 AssetDatabase::projectFolders());
    const AssetRecord* record = database.findByPath("assets/sprayer.gltf");
    CHECK(record != nullptr);
    if (record)
    {
      AssetStore store;
      store.setDatabase(&database);
      testing3d::MockRenderer renderer;
      Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
      RenderSystem& render = scene.addSystem<RenderSystem>(renderer);
      SceneSerializer serializer(reflect::types(), &render.meshes());
      serializer.setPrefabSource(
          [&](std::string_view name) -> std::optional<nlohmann::json>
          {
            const auto asset = parseAssetMeshName(name);
            return asset ? store.prefab(asset->model) : std::nullopt;
          });
      serializer.instantiate("asset:" + record->id.toString(), scene);
      scene.update(0.0);
      const auto find = [&](const std::string& name)
      {
        for (Entity entity : scene.entities())
          if (entity.hasComponent<NameComponent>() &&
              entity.getComponent<NameComponent>().name == name)
            return entity;
        return Entity{};
      };
      const Entity body = find("Body");
      const Entity boom = find("Boom-collider.001");
      const Entity cab = find("Cab-COLLIDER");
      CHECK(body.isValid() && boom.isValid() && cab.isValid());
      if (body.isValid() && boom.isValid() && cab.isValid())
      {
        CHECK(body.hasComponent<MeshRendererComponent>());
        CHECK(!body.hasComponent<BoxCollider3DComponent>());
        CHECK(!boom.hasComponent<MeshRendererComponent>());
        CHECK(boom.hasComponent<BoxCollider3DComponent>());
        CHECK(hierarchy::parentOf(boom).getComponent<NameComponent>().name ==
              "Boom");
        if (boom.hasComponent<BoxCollider3DComponent>())
        {
          // Twice x 0..2, y -3..3, z -2..2.
          const auto& box = boom.getComponent<BoxCollider3DComponent>();
          CHECK(testing::approx(box.size.x, 4.0f));
          CHECK(testing::approx(box.size.y, 12.0f));
          CHECK(testing::approx(box.size.z, 8.0f));
          CHECK(testing::approx(box.offset.x, 2.0f));
          CHECK(testing::approx(box.offset.y, 0.0f));
          CHECK(!box.trigger);
        }
        CHECK(cab.hasComponent<BoxCollider3DComponent>());
        CHECK(!cab.hasComponent<MeshRendererComponent>());
      }
    }
  }

  TEST("a node's id should be a hash of its name path, the same in every "
       "build")
  {
    // Pinned: files store these ids, so the hash may never change.
    CHECK(stableNodeId("Body") == 2554517965u);
    CHECK(stableNodeId("") == 2847271067u);
    CHECK(stableNodeId("#1") == 2860501858u);
    const std::map<std::string, uint32_t> ids =
        idsByPath(writeTreeModel("ids.gltf", kTreeNodes, "[0, 4]"));
    CHECK(ids.size() == 5u);
    for (const auto& [path, id] : ids)
    {
      CHECK(id == stableNodeId(path));
      CHECK((id & kModelNodeIdBit) != 0u);
    }
    CHECK(ids.contains("Body/Arm/Hand"));
    CHECK(ids.contains("Cab"));
  }

  TEST("reordering nodes, or moving and renaming the file, should keep every "
       "id")
  {
    const std::map<std::string, uint32_t> ids =
        idsByPath(writeTreeModel("order.gltf", kTreeNodes, "[0, 4]"));
    // The cab first, and the leg before the arm.
    const std::map<std::string, uint32_t> reordered =
        idsByPath(writeTreeModel("reordered.gltf",
                                 R"([
      {"name": "Cab", "mesh": 1},
      {"name": "Leg"},
      {"name": "Hand"},
      {"name": "Body", "mesh": 0, "children": [1, 4]},
      {"name": "Arm", "children": [2]}
    ])",
                                 "[0, 3]"));
    CHECK(ids.size() == 5u);
    CHECK(reordered == ids);
    const std::map<std::string, uint32_t> moved = idsByPath(
        writeTreeModel(std::filesystem::path("elsewhere") / "renamed.gltf",
                       kTreeNodes,
                       "[0, 4]"));
    CHECK(moved == ids);
  }

  TEST("renaming a node should change its id and its descendants' only")
  {
    const std::map<std::string, uint32_t> ids =
        idsByPath(writeTreeModel("named.gltf", kTreeNodes, "[0, 4]"));
    const std::map<std::string, uint32_t> renamed =
        idsByPath(writeTreeModel("renamed.gltf",
                                 R"([
      {"name": "Body", "mesh": 0, "children": [1, 3]},
      {"name": "Boom", "children": [2]},
      {"name": "Hand"},
      {"name": "Leg"},
      {"name": "Cab", "mesh": 1}
    ])",
                                 "[0, 4]"));
    CHECK(renamed.size() == 5u);
    CHECK(renamed.at("Body") == ids.at("Body"));
    CHECK(renamed.at("Body/Leg") == ids.at("Body/Leg"));
    CHECK(renamed.at("Cab") == ids.at("Cab"));
    CHECK(renamed.contains("Body/Boom/Hand"));
    CHECK(!renamed.contains("Body/Arm"));
    CHECK(renamed.at("Body/Boom") != ids.at("Body/Arm"));
    CHECK(renamed.at("Body/Boom/Hand") != ids.at("Body/Arm/Hand"));
  }

  TEST("same-named and unnamed siblings should be numbered in order, and "
       "names escaped")
  {
    const std::filesystem::path file = writeTreeModel("repeats.gltf",
                                                      R"([
      {"name": "Body", "mesh": 0, "children": [1, 2, 3]},
      {"name": "Wheel"},
      {"name": "Axle", "children": [4]},
      {"name": "Wheel"},
      {"name": "Wheel"},
      {"mesh": 1},
      {},
      {"name": "a/b#c"}
    ])",
                                                      "[0, 5, 6, 7]");
    std::vector<std::string> paths;
    Warnings warnings;
    const auto nodes = importModelNodes(file, {}, &paths);
    CHECK(warnings.any("sibling nodes share the names '(unnamed)', 'Wheel'"));
    CHECK(nodes.has_value());
    CHECK(paths == std::vector<std::string>({"Body",
                                             "Body/Wheel",
                                             "Body/Axle",
                                             "Body/Axle/Wheel",
                                             "Body/Wheel#1",
                                             "",
                                             "#1",
                                             "a\\/b\\#c"}));
    if (nodes && nodes->size() == 8u)
    {
      CHECK((*nodes)[5].name == "Node 6"); // shown so, never used as identity
      CHECK((*nodes)[4].id == stableNodeId("Body/Wheel#1"));
      CHECK((*nodes)[6].id == stableNodeId("#1"));
    }
    // Another sibling between the two wheels keeps their ids.
    const std::map<std::string, uint32_t> ids = idsByPath(file);
    const std::map<std::string, uint32_t> spaced =
        idsByPath(writeTreeModel("spaced.gltf",
                                 R"([
      {"name": "Body", "mesh": 0, "children": [1, 2, 3, 5]},
      {"name": "Wheel"},
      {"name": "Seat", "mesh": 1},
      {"name": "Axle", "children": [4]},
      {"name": "Wheel"},
      {"name": "Wheel"}
    ])",
                                 "[0]"));
    CHECK(spaced.at("Body/Wheel") == ids.at("Body/Wheel"));
    CHECK(spaced.at("Body/Wheel#1") == ids.at("Body/Wheel#1"));
  }

  TEST("a name's id shouldn't depend on how the file spells it")
  {
    // The same names, written with and without JSON escapes.
    const std::map<std::string, uint32_t> plain =
        idsByPath(writeTreeModel("spelled.gltf",
                                 R"([
      {"name": "café", "mesh": 0, "children": [1]},
      {"name": "a/b"},
      {"name": "Cab", "mesh": 1}
    ])",
                                 "[0, 2]"));
    const std::map<std::string, uint32_t> escaped =
        idsByPath(writeTreeModel("escaped.gltf",
                                 R"([
      {"name": "caf\u00e9", "mesh": 0, "children": [1]},
      {"name": "a\/b"},
      {"name": "\u0043ab", "mesh": 1}
    ])",
                                 "[0, 2]"));
    CHECK(plain.size() == 3u);
    CHECK(escaped == plain);
    CHECK(plain.contains("caf\xC3\xA9/a\\/b"));
  }

  TEST("two name paths whose hashes collide should be told apart the same way "
       "every time")
  {
    // Found offline: these two collide in 31 bits.
    CHECK(stableNodeId("part3720") == 2174831622u);
    CHECK(stableNodeId("part83680") == 2174831622u);
    const std::filesystem::path file = writeTreeModel("collide.gltf",
                                                      R"([
      {"name": "part3720", "mesh": 0},
      {"name": "part83680", "mesh": 1}
    ])",
                                                      "[0, 1]");
    Warnings warnings;
    for (int run = 0; run < 2; ++run)
    {
      const auto nodes = importModelNodes(file);
      CHECK(nodes.has_value() && nodes->size() == 2u);
      if (nodes && nodes->size() == 2u)
      {
        CHECK((*nodes)[0].id == 2174831622u);
        CHECK((*nodes)[1].id == 2174831623u);
      }
    }
    // Logged naming both (once: a repeat soon after is only counted).
    CHECK(warnings.any(
        "the nodes 'part3720' and 'part83680' hash to the same id"));
  }

  TEST("a model's prefab should give its nodes their stable ids, in the "
       "current format")
  {
    const std::filesystem::path project =
        std::filesystem::temp_directory_path() / "sfsModelImporterTests" /
        "idProject";
    std::filesystem::create_directories(project / "assets");
    std::filesystem::copy_file(
        writeTreeModel("prefabIds.gltf", kTreeNodes, "[0, 4]"),
        project / "assets" / "tree.gltf",
        std::filesystem::copy_options::overwrite_existing);
    std::ofstream(project / "assets" / "tree.gltf.meta")
        << R"({"formatVersion": 1, "id": "1123456789abcdef0123456789abcdef"})";
    const AssetDatabase database(project,
                                 AssetDatabase::Mode::ReadOnly,
                                 AssetDatabase::projectFolders());
    const AssetRecord* record = database.findByPath("assets/tree.gltf");
    CHECK(record != nullptr);
    if (record)
    {
      AssetStore store;
      store.setDatabase(&database);
      const auto prefab = store.prefab(record->id);
      const auto nodes = importModelNodes(project / "assets" / "tree.gltf");
      CHECK(prefab.has_value() && nodes.has_value());
      if (prefab && nodes)
      {
        CHECK((*prefab)["formatVersion"] ==
              SceneSerializer::kPrefabFormatVersion);
        const nlohmann::json& body = (*prefab)["root"]["children"][0];
        CHECK(body["$id"] == (*nodes)[0].id);
        CHECK(body["children"][0]["$id"] == (*nodes)[1].id);
        CHECK(body["children"][0]["children"][0]["$id"] == (*nodes)[2].id);
        CHECK((*prefab)["root"]["children"][1]["$id"] == (*nodes)[4].id);
      }
    }
  }

  TEST("prefabs and scenes saved with model nodes' old ids (their place in "
       "the file) should load onto the same nodes")
  {
    const std::filesystem::path project =
        std::filesystem::temp_directory_path() / "sfsModelImporterTests" /
        "oldIdProject";
    std::filesystem::create_directories(project / "assets");
    std::filesystem::copy_file(
        writeTreeModel("oldIds.gltf", kTreeNodes, "[0, 4]"),
        project / "assets" / "tree.gltf",
        std::filesystem::copy_options::overwrite_existing);
    std::ofstream(project / "assets" / "tree.gltf.meta")
        << R"({"formatVersion": 1, "id": "2123456789abcdef0123456789abcdef"})";
    const AssetDatabase database(project,
                                 AssetDatabase::Mode::ReadOnly,
                                 AssetDatabase::projectFolders());
    AssetStore store;
    store.setDatabase(&database);
    testing3d::MockRenderer renderer;
    Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
    RenderSystem& render = scene.addSystem<RenderSystem>(renderer);
    const std::string model = "asset:2123456789abcdef0123456789abcdef";
    // A machine prefab as saved before stable ids: the model is its entity 1,
    // and the model's nodes are numbered in the file's order (Body 1, Arm 2,
    // Hand 3, Leg 4, Cab 5).
    const nlohmann::json machine = {
        {"formatVersion", 1},
        {"root",
         {{"NameComponent", {{"name", "Machine"}}},
          {"children",
           {{{"$id", 1},
             {"$prefab", model},
             {"$overrides",
              {{"2", {{"TransformComponent", {{"position", {0, 0, 2}}}}}}}},
             {"$removed", {"4"}},
             {"$added",
              {{{"$parent", "3"},
                {"NameComponent", {{"name", "Glove"}}}}}}}}}}}};
    SceneSerializer serializer(reflect::types(), &render.meshes());
    serializer.setPrefabSource(
        [&](std::string_view name) -> std::optional<nlohmann::json>
        {
          if (name == "asset:machine")
            return machine;
          const auto asset = parseAssetMeshName(name);
          return asset ? store.prefab(asset->model) : std::nullopt;
        });
    // A scene as saved before, moving the machine's cab ("1/5").
    const nlohmann::json saved = {
        {"formatVersion", 2},
        {"entities",
         {{{"$prefab", "asset:machine"},
           {"$overrides",
            {{"1/5", {{"TransformComponent", {{"position", {0, 0, 5}}}}}}}}}}}};
    CHECK(serializer.load(saved, scene));
    scene.update(0.0);
    const auto find = [&](const std::string& name)
    {
      for (Entity entity : scene.entities())
        if (entity.hasComponent<NameComponent>() &&
            entity.getComponent<NameComponent>().name == name)
          return entity;
      return Entity{};
    };
    const Entity arm = find("Arm");
    const Entity cab = find("Cab");
    const Entity glove = find("Glove");
    CHECK(arm.isValid() && cab.isValid() && glove.isValid());
    CHECK(!find("Leg").isValid());
    if (arm.isValid() && cab.isValid() && glove.isValid())
    {
      CHECK(arm.getComponent<TransformComponent>().position.z == 2.0f);
      CHECK(cab.getComponent<TransformComponent>().position.z == 5.0f);
      CHECK(find("Hand").getComponent<TransformComponent>().position.z == 0.0f);
      CHECK(hierarchy::parentOf(glove) == find("Hand"));
      CHECK(!find("Machine").hasComponent<UnresolvedComponents>());
    }

    // Saved again, it names the nodes by their stable ids.
    const auto nodes = importModelNodes(project / "assets" / "tree.gltf");
    CHECK(nodes.has_value() && nodes->size() == 5u);
    if (nodes && nodes->size() == 5u)
    {
      const nlohmann::json resaved = serializer.save(scene);
      CHECK(resaved["entities"][0]["$overrides"].contains(
          "1/" + std::to_string((*nodes)[4].id)));
      const nlohmann::json upgraded = serializer.savePrefab(
          scene, find("Machine"), true)["root"]["children"][0];
      CHECK(upgraded["$overrides"].contains(std::to_string((*nodes)[1].id)));
      CHECK(upgraded["$removed"] ==
            nlohmann::json::array({std::to_string((*nodes)[3].id)}));
      CHECK(upgraded["$added"][0]["$parent"] == std::to_string((*nodes)[2].id));
      // The edits that upgrade the old files say the same.
      const auto edits = serializer.nodeIdEdits(machine);
      CHECK(edits.size() == 3u);
      if (edits.size() == 3u)
      {
        CHECK(edits[0].pointer == "/root/children/0/$overrides/2");
        CHECK(edits[0].key);
        CHECK(edits[0].to == std::to_string((*nodes)[1].id));
        CHECK(edits[0].model == model);
        CHECK(edits[0].fromId == 2u);
        CHECK(edits[0].toId == (*nodes)[1].id);
        CHECK(edits[1].pointer == "/root/children/0/$removed/0");
        CHECK(!edits[1].key);
        CHECK(edits[2].pointer == "/root/children/0/$added/0/$parent");
        CHECK(edits[2].to == std::to_string((*nodes)[2].id));
      }
      const auto sceneEdits = serializer.nodeIdEdits(saved);
      CHECK(sceneEdits.size() == 1u);
      if (sceneEdits.size() == 1u)
      {
        CHECK(sceneEdits[0].pointer == "/entities/0/$overrides/1~15");
        CHECK(sceneEdits[0].from == "1/5");
        CHECK(sceneEdits[0].to == "1/" + std::to_string((*nodes)[4].id));
      }
      CHECK(serializer.nodeIdEdits(resaved).empty());
    }
  }

  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                              "sfsModelImporterTests");
  return testing::report("modelImporterTests");
}
