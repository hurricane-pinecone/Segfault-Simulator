#include "scene/gridModule.h"

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/frame.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/renderContext.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace
{
constexpr float HALF_SIZE = 200.0f;  // metres from the origin to the edge
constexpr float TILE_METRES = 10.0f; // the texture covers 10 x 10 m
constexpr int TILE_PIXELS = 640;     // so a metre is 64 pixels
constexpr float AXIS_HALF_WIDTH = 0.01f;

// Lines on clear, one tile: metre lines, and the tenth metre's (the tile's
// edges) stronger. The colour is the same everywhere, so smaller mips (seen
// further away) fade the lines rather than darken them.
std::shared_ptr<const sfs::TextureData> gridTexture()
{
  auto texture = std::make_shared<sfs::TextureData>();
  texture->width = texture->height = TILE_PIXELS;
  texture->rgba.resize(static_cast<std::size_t>(TILE_PIXELS) * TILE_PIXELS * 4);
  const int metre = TILE_PIXELS / static_cast<int>(TILE_METRES);
  // How strongly a pixel column (or row) is a line.
  const auto line = [&](int i) -> float
  {
    if (i == 0 || i == TILE_PIXELS - 1)
      return 0.6f; // a tile edge: every tenth metre

    return i % metre == 0 ? 0.35f : 0.0f;
  };

  for (int y = 0; y < TILE_PIXELS; ++y)
    for (int x = 0; x < TILE_PIXELS; ++x)
    {
      uint8_t* pixel =
          &texture->rgba[(static_cast<std::size_t>(y) * TILE_PIXELS + x) * 4];
      pixel[0] = pixel[1] = pixel[2] = 205;
      pixel[3] = static_cast<uint8_t>(std::max(line(x), line(y)) * 255.0f);
    }

  return texture;
}

void addQuad(sfs::MeshData& mesh,
             glm::vec2 min,
             glm::vec2 max,
             float z,
             const sfs::MeshMaterial& material)
{
  const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
  const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());

  for (const glm::vec2 corner :
       {min, glm::vec2(max.x, min.y), max, glm::vec2(min.x, max.y)})
    mesh.vertices.push_back(sfs::MeshVertex{
        .position = glm::vec3(corner, z), .uv = corner / TILE_METRES});

  for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
    mesh.indices.push_back(first + i);

  mesh.sections.push_back(sfs::MeshSection{firstIndex, 6, material, {}});
}

sfs::MeshData gridMesh()
{
  sfs::MeshData mesh;
  sfs::MeshMaterial lines;
  lines.baseTexture = gridTexture();
  lines.alphaMode = sfs::AlphaMode::Blend;
  // Just under the ground, so things standing on it aren't cut by it.
  addQuad(mesh, glm::vec2(-HALF_SIZE), glm::vec2(HALF_SIZE), -0.002f, lines);

  sfs::MeshMaterial xAxis;
  xAxis.baseColor = glm::vec4(0.85f, 0.22f, 0.24f, 1.0f);
  addQuad(mesh,
          glm::vec2(-HALF_SIZE, -AXIS_HALF_WIDTH),
          glm::vec2(HALF_SIZE, AXIS_HALF_WIDTH),
          -0.001f,
          xAxis);
  sfs::MeshMaterial yAxis;
  yAxis.baseColor = glm::vec4(0.45f, 0.78f, 0.22f, 1.0f);
  addQuad(mesh,
          glm::vec2(-AXIS_HALF_WIDTH, -HALF_SIZE),
          glm::vec2(AXIS_HALF_WIDTH, HALF_SIZE),
          -0.001f,
          yAxis);
  return mesh;
}
} // namespace

GridModule::GridModule(sfs::MeshLibrary& meshes)
    : m_mesh(meshes.add(gridMesh()))
{
}

void GridModule::computeCommands(const sfs::RenderContext&)
{
  flush();
  // Seen, never casting a shadow (the sun would draw it onto nothing).
  m_commands.push_back(
      sfs::MeshDrawCommand{.mesh = m_mesh, .instances = {sfs::MeshInstance{}}});
}
