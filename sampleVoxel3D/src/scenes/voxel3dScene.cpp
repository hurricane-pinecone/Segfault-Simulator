#include "scenes/voxel3dScene.h"

#include "config.h"

#include "engine/core/voxel/tinyVoxelChunk.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/systems/voxel3DRenderSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Each OLD block (the sample's 1-tile unit) is this many tiny voxels per edge.
// This is THE knob for how fine the voxels read: higher = voxels are a smaller
// fraction of the player/terrain, so they feel like an artistic texture rather
// than the blocks that define the landscape (the Minecraft look). The player
// and terrain are sized in blocks, so they scale up in voxels automatically
// with it.
constexpr int kVPB = 16;

// Terrain shape, in BLOCKS (matches the iso sample's scale), then * kVPB.
constexpr int kBaseBlocks = 2;
// BIG terrain so the ~1.5-block player is dwarfed: mountains many blocks tall
// AND the features wide (see the low macro frequency below) -- bigger all
// round, not just taller (taller-only spikes them).
constexpr int kRangeBlocks = 9;
constexpr int kSeaBlocks =
    5; // higher sea floods the broad valleys -> big lakes
constexpr float kFineAmp = 4.0f; // sub-block surface jitter, in voxels

// Patch size, in chunks (1 chunk = 32 voxels = 2 blocks at kVPB=16). Bigger
// than the camera ever frames, so the world extends past the view (expansive,
// no edges) while the camera stays close enough that the player reads as a
// figure. kGridY is tall to fit the big mountains; empty (all-air) chunks
// aren't stored, so the cost tracks the actual terrain volume, not the full
// grid.
constexpr int kGrid = 12; // 384 voxels / 24 blocks across x,z
constexpr int kGridY = 6; // 192 voxels tall (fits the tall mountains)

constexpr float kMoveSpeed = 100.0f; // voxels / second
constexpr float kYawSpeed = 1.7f;    // radians / second
constexpr float kZoomSpeed = 110.0f; // ortho half-height / second
// A ~1.5-block-tall, ~0.5-block-wide figure (1 block = kVPB voxels) -- now ~24
// voxels tall, so the voxels are fine detail on him, not big blocks.
constexpr float kPlayerHalfY = 12.0f;
constexpr float kPlayerHalfXZ = 4.0f;

// Cheap per-voxel hash in [0,1], for tiny brightness variation so a flat
// surface still reads as individual voxels.
float hash3(int x, int y, int z)
{
  std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                    static_cast<std::uint32_t>(y) * 19349663u ^
                    static_cast<std::uint32_t>(z) * 83492791u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

std::uint32_t shade(int r, int g, int b, int wx, int wy, int wz)
{
  const float k = 0.86f + 0.22f * hash3(wx, wy, wz);
  const auto ch = [&](int c)
  {
    return static_cast<std::uint8_t>(
        glm::clamp(static_cast<int>(static_cast<float>(c) * k), 0, 255));
  };
  return sfs::tinyColor(ch(r), ch(g), ch(b));
}
} // namespace

int Voxel3DScene::terrainHeight(int wx, int wz) const
{
  // Macro hills at block scale + fine sub-block detail, both in voxel units.
  const float macro =
      m_noise.get(static_cast<float>(wx), static_cast<float>(wz));
  const float fine =
      m_detail.get(static_cast<float>(wx), static_cast<float>(wz));
  const float hBlocks =
      static_cast<float>(kBaseBlocks) + (macro + 1.0f) * 0.5f * kRangeBlocks;
  return static_cast<int>(hBlocks * static_cast<float>(kVPB) + fine * kFineAmp);
}

std::uint32_t Voxel3DScene::voxelAt(int wx, int wy, int wz) const
{
  const int h = terrainHeight(wx, wz);
  const int sea = kSeaBlocks * kVPB;

  if (wy < h)
  {
    if (wy >= h - 3)                                            // top crust
      return h <= sea + kVPB ? shade(206, 192, 142, wx, wy, wz) // sandy shore
                             : shade(86, 168, 80, wx, wy, wz);  // grass
    if (wy >= h - 2 * kVPB)
      return shade(122, 92, 60, wx, wy, wz); // dirt
    return shade(108, 110, 124, wx, wy, wz); // stone
  }
  return 0u; // air -- water is a separate transparent surface pass
}

void Voxel3DScene::onInit()
{
  m_noise.setSeed(1337);
  m_noise.setFrequency(
      0.004f); // macro: BIG wide features (~16-block hills/lakes)
  m_noise.setType(sfs::Noise::Type::OpenSimplex);

  m_detail.setSeed(99);
  m_detail.setFrequency(0.09f); // fine: per-few-voxel surface bumps
  m_detail.setType(sfs::Noise::Type::OpenSimplex);

  auto& render =
      addSystem<sfs::Voxel3DRenderSystem>(WINDOW_WIDTH, WINDOW_HEIGHT);
  m_render = &render;

  for (int cz = 0; cz < kGrid; ++cz)
    for (int cx = 0; cx < kGrid; ++cx)
      for (int cy = 0; cy < kGridY; ++cy)
      {
        sfs::TinyVoxelChunk chunk;
        bool any = false;
        for (int lz = 0; lz < sfs::kTinyChunkSize; ++lz)
          for (int ly = 0; ly < sfs::kTinyChunkSize; ++ly)
            for (int lx = 0; lx < sfs::kTinyChunkSize; ++lx)
            {
              const std::uint32_t c = voxelAt(cx * sfs::kTinyChunkSize + lx,
                                              cy * sfs::kTinyChunkSize + ly,
                                              cz * sfs::kTinyChunkSize + lz);
              if (c != 0u)
              {
                chunk.set(lx, ly, lz, c);
                any = true;
              }
            }
        if (any)
          render.setChunk(glm::ivec3{cx, cy, cz}, chunk);
      }

  // Water: every column whose terrain dips below sea level. The render system
  // animates these as a transparent wave surface (re-meshed each frame), so you
  // see the bed through them -- and it's the per-frame "moving voxels" workload
  // for gauging performance.
  const int sea = kSeaBlocks * kVPB;
  std::vector<sfs::Voxel3DRenderSystem::WaterColumn> water;
  const int span = kGrid * sfs::kTinyChunkSize;
  for (int wz = 0; wz < span; ++wz)
    for (int wx = 0; wx < span; ++wx)
    {
      const int h = terrainHeight(wx, wz);
      if (h < sea)
        water.push_back({wx, wz, h});
    }
  render.setWater(std::move(water), sea);

  // Spawn on land near the centre (walk +x off any lake so the player isn't
  // submerged at start).
  int px = span / 2;
  const int pz = span / 2;
  for (int r = 0; r < span / 2 && terrainHeight(px, pz) < sea; r += kVPB)
    px = span / 2 + r;
  m_playerPos =
      glm::vec3{static_cast<float>(px),
                static_cast<float>(terrainHeight(px, pz)) + kPlayerHalfY,
                static_cast<float>(pz)};

  render.camera().focus = m_playerPos;
  render.camera().zoom = 150.0f; // ~19 blocks of view; player a clear figure
  render.setLightDir(glm::normalize(glm::vec3{0.4f, 0.9f, 0.35f}));
  render.setPlayerBox(m_playerPos,
                      glm::vec3{kPlayerHalfXZ, kPlayerHalfY, kPlayerHalfXZ},
                      glm::vec3{0.92f, 0.26f, 0.2f});
}

void Voxel3DScene::onProcessInput(const sfs::Input& input)
{
  const auto& kb = input.keyboard();

  m_moveInput = glm::vec2{0.0f, 0.0f};
  if (kb.keyHeld(sfs::Key::W))
    m_moveInput.y += 1.0f;
  if (kb.keyHeld(sfs::Key::S))
    m_moveInput.y -= 1.0f;
  if (kb.keyHeld(sfs::Key::D))
    m_moveInput.x += 1.0f;
  if (kb.keyHeld(sfs::Key::A))
    m_moveInput.x -= 1.0f;

  m_yawInput = (kb.keyHeld(sfs::Key::E) ? 1.0f : 0.0f) -
               (kb.keyHeld(sfs::Key::Q) ? 1.0f : 0.0f);
  m_zoomInput = (kb.keyHeld(sfs::Key::R) ? 1.0f : 0.0f) - // R = zoom in
                (kb.keyHeld(sfs::Key::F) ? 1.0f : 0.0f);  // F = zoom out
}

void Voxel3DScene::onUpdate(double deltaTime)
{
  if (deltaTime > 0.0001)
    m_fps += (1.0 / deltaTime - m_fps) * 0.1; // eased FPS readout

  if (!m_render)
    return;

  const float dt = static_cast<float>(deltaTime);
  sfs::OrthoOrbitCamera& cam = m_render->camera();

  cam.yaw += m_yawInput * kYawSpeed * dt;
  cam.zoom =
      glm::clamp(cam.zoom - m_zoomInput * kZoomSpeed * dt, 18.0f, 180.0f);

  glm::vec3 move = cam.forward() * m_moveInput.y + cam.right() * m_moveInput.x;
  if (glm::length(move) > 0.0001f)
  {
    move = glm::normalize(move);
    m_playerPos.x += move.x * kMoveSpeed * dt;
    m_playerPos.z += move.z * kMoveSpeed * dt;
  }
  m_playerPos.y = static_cast<float>(terrainHeight(
                      static_cast<int>(glm::floor(m_playerPos.x)),
                      static_cast<int>(glm::floor(m_playerPos.z)))) +
                  kPlayerHalfY;

  cam.focus = m_playerPos;
  m_render->setPlayerBox(m_playerPos,
                         glm::vec3{kPlayerHalfXZ, kPlayerHalfY, kPlayerHalfXZ},
                         glm::vec3{0.92f, 0.26f, 0.2f});
}

void Voxel3DScene::onRender()
{
  textRenderer().drawText(
      20, 20, "FPS: " + std::to_string(static_cast<int>(m_fps)));
}
