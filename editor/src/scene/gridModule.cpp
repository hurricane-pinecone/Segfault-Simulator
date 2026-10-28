#include "scene/gridModule.h"

#include "engine/runtime/rendering3d/meshLibrary.h"

#include <cmath>
#include <memory>

namespace
{
constexpr float kHalfSize = 200.0f;  // metres from the origin to the edge
constexpr float kTileMetres = 10.0f; // the texture covers 10 x 10 m
constexpr int kTilePixels = 640;     // so a metre is 64 pixels
constexpr float kAxisHalfWidth = 0.01f;

// Lines on clear, one tile: metre lines, and the tenth metre's (the tile's
// edges) stronger. The colour is the same everywhere, so smaller mips (seen
// further away) fade the lines rather than darken them.
std::shared_ptr<const sfs::TextureData> gridTexture()
{
  auto texture = std::make_shared<sfs::TextureData>();
  texture->width = texture->height = kTilePixels;
  texture->rgba.resize(static_cast<std::size_t>(kTilePixels) * kTilePixels * 4);
  const int metre = kTilePixels / static_cast<int>(kTileMetres);
  // How strongly a pixel column (or row) is a line.
  const auto line = [&](int i) -> float
  {
    if (i == 0 || i == kTilePixels - 1)
      return 0.6f; // a tile edge: every tenth metre
    return i % metre == 0 ? 0.35f : 0.0f;
  };
  for (int y = 0; y < kTilePixels; ++y)
    for (int x = 0; x < kTilePixels; ++x)
    {
      uint8_t* pixel = &texture->rgba[(static_cast<std::size_t>(y) * kTilePixels + x) * 4];
      pixel[0] = pixel[1] = pixel[2] = 205;
      pixel[3] = static_cast<uint8_t>(std::max(line(x), line(y)) * 255.0f);
    }
  return texture;
}

void addQuad(sfs::MeshData& mesh, glm::vec2 min, glm::vec2 max, float z, const sfs::MeshMaterial& material)
{
  const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
  const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
  for (const glm::vec2 corner : {min, glm::vec2(max.x, min.y), max, glm::vec2(min.x, max.y)})
    mesh.vertices.push_back(sfs::MeshVertex{.position = glm::vec3(corner, z),
                                            .uv = corner / kTileMetres});
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
  addQuad(mesh, glm::vec2(-kHalfSize), glm::vec2(kHalfSize), -0.002f, lines);

  sfs::MeshMaterial xAxis;
  xAxis.baseColor = glm::vec4(0.85f, 0.22f, 0.24f, 1.0f);
  addQuad(mesh, glm::vec2(-kHalfSize, -kAxisHalfWidth), glm::vec2(kHalfSize, kAxisHalfWidth), -0.001f, xAxis);
  sfs::MeshMaterial yAxis;
  yAxis.baseColor = glm::vec4(0.45f, 0.78f, 0.22f, 1.0f);
  addQuad(mesh, glm::vec2(-kAxisHalfWidth, -kHalfSize), glm::vec2(kAxisHalfWidth, kHalfSize), -0.001f, yAxis);
  return mesh;
}
} // namespace

GridModule::GridModule(sfs::MeshLibrary& meshes) : m_mesh(meshes.add(gridMesh())) {}

void GridModule::computeCommands(const sfs::RenderContext3D&)
{
  flush();
  // Seen, never casting a shadow (the sun would draw it onto nothing).
  m_commands.push_back(sfs::MeshDrawCommand{.mesh = m_mesh, .instances = {sfs::MeshInstance{}}});
}
