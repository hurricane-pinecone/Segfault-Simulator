#include "engine/runtime/rendering/modules/decalModule.h"

#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/imageQuads.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{
// The images marks use unless their spawn names one.
constexpr const char* kSoftImage = "white_dot";
constexpr const char* kCrispImage = "white_pixel";
// In 3D, how far above its surface a mark lies (so it doesn't flicker with it).
constexpr float kLift = 0.01f;
} // namespace

DecalModule::DecalModule(AssetStore& assets) : m_assets(assets)
{
  if (!m_assets.getSurface(kSoftImage))
    m_assets.addRadialTexture(kSoftImage, 32);
  if (!m_assets.getSurface(kCrispImage))
    m_assets.addWhitePixelTexture(kCrispImage);
}

void DecalModule::setCoverage(float cellSize, int perCell)
{
  m_cellSize = cellSize;
  m_perCell = perCell;
}

long long DecalModule::cellOf(glm::vec2 position) const
{
  const auto x = static_cast<long long>(std::floor(position.x / m_cellSize));
  const auto y = static_cast<long long>(std::floor(position.y / m_cellSize));
  return x * 73856093LL ^ y * 19349663LL;
}

void DecalModule::addDecal(const DecalSpawn& spawn)
{
  Decal decal;
  decal.position = spawn.worldPos;
  decal.height = spawn.elevation;
  decal.size = spawn.size;
  decal.rotation = spawn.rotation;
  decal.color = spawn.color;
  decal.image = spawn.textureId ? *spawn.textureId
                : spawn.crisp   ? kCrispImage
                                : kSoftImage;
  // fadeRate (alpha a second) -> how long it lasts.
  decal.lifetime = spawn.fadeRate > 0.0f ? 1.0f / spawn.fadeRate : -1.0f;
  // A drip hangs from where it landed and runs down.
  if (spawn.dripSpeed > 0.0f)
  {
    decal.anchor = {0.5f, 0.0f};
    decal.growTo = {spawn.size.x, spawn.size.y * 5.0f};
    decal.growRate = spawn.dripSpeed;
  }
  decal.clipped = spawn.clipping == Clipping::Surface &&
                  spawn.clipMax.x > spawn.clipMin.x &&
                  spawn.clipMax.y > spawn.clipMin.y;
  decal.clipMin = spawn.clipMin;
  decal.clipMax = spawn.clipMax;

  // A spot full of lasting marks loses its oldest.
  if (decal.lifetime < 0.0f && m_perCell > 0 && m_cellSize > 0.0f)
  {
    const long long cell = cellOf(decal.position);
    int count = 0;
    std::size_t oldest = m_decals.size();
    for (std::size_t i = 0; i < m_decals.size(); ++i)
      if (m_decals[i].lifetime < 0.0f && cellOf(m_decals[i].position) == cell)
      {
        if (oldest == m_decals.size())
          oldest = i;
        ++count;
      }
    if (count >= m_perCell)
      m_decals.erase(m_decals.begin() + static_cast<std::ptrdiff_t>(oldest));
  }
  m_decals.push_back(std::move(decal));
  if (m_decals.size() > m_max)
    m_decals.erase(m_decals.begin(),
                   m_decals.begin() +
                       static_cast<std::ptrdiff_t>(m_decals.size() - m_max));
}

void DecalModule::update(double deltaTime)
{
  const auto dt = static_cast<float>(deltaTime);
  for (Decal& decal : m_decals)
  {
    decal.age += dt;
    for (int axis = 0; axis < 2; ++axis)
      if (decal.growTo[axis] > 0.0f && decal.size[axis] < decal.growTo[axis])
        decal.size[axis] = std::min(
            decal.growTo[axis], decal.size[axis] + decal.growRate * dt);
  }
  std::erase_if(m_decals,
                [](const Decal& decal) {
                  return decal.lifetime >= 0.0f && decal.age >= decal.lifetime;
                });
}

void DecalModule::computeCommands(const RenderContext& context)
{
  flush();
  if (!context.meshes || m_decals.empty())
    return;
  // In 3D the game's plane is the ground (z up), each mark a hair above what
  // it landed on.
  const Space2D* space2D = context.space2D;
  const Space2D space = space2D ? *space2D : Space2D{};
  const auto place = [&](glm::vec2 position, float height)
  {
    return space2D ? space.place(position, height, m_layer)
                   : glm::vec3(position, height + kLift);
  };
  MeshLibrary& meshes = *context.meshes;
  if (!m_quad.valid())
    m_quad = imageQuad(meshes);

  std::unordered_map<uint32_t, std::size_t> byMaterial; // into m_commands
  for (const Decal& decal : m_decals)
  {
    auto [at, added] = m_materials.emplace(decal.image, MaterialHandle{});
    if (added)
      at->second = meshes.setMaterial(
          "decal:" + decal.image,
          imageMaterial(m_assets, decal.image, {}, AlphaMode::Blend));
    auto [slot, fresh] = byMaterial.emplace(at->second.id, m_commands.size());
    if (fresh)
    {
      MeshDrawCommand command;
      command.pass = RenderPass::Transparent;
      command.mesh = m_quad;
      command.materials = {at->second};
      m_commands.push_back(std::move(command));
    }

    MeshInstance instance;
    if (space2D && space.projection == Space2D::Projection::Flat)
    {
      // Upright on the screen, turned by its rotation (as the game's plane
      // turns), centred off its anchor (its image's y runs down the screen).
      const glm::vec2 size = decal.size;
      const float angle = decal.rotation * space.ySign();
      const glm::vec3 x(std::cos(angle), std::sin(angle), 0.0f);
      const glm::vec3 y(-x.y, x.x, 0.0f);
      glm::vec3 centre = space.place(decal.position, decal.height, m_layer) +
                         x * ((0.5f - decal.anchor.x) * size.x) -
                         y * ((0.5f - decal.anchor.y) * size.y);
      glm::vec2 shown = size;
      glm::vec4 uvRect(0.0f, 0.0f, 1.0f, 1.0f);
      // Unturned, it's cut to the surface it landed on, its image alike.
      if (decal.clipped && std::abs(angle) < 1.0e-3f)
      {
        const glm::vec3 a = space.place(decal.clipMin, decal.height);
        const glm::vec3 b = space.place(decal.clipMax, decal.height);
        const glm::vec2 from = glm::vec2(centre) - size * 0.5f;
        const glm::vec2 low =
            glm::max(from, glm::min(glm::vec2(a), glm::vec2(b)));
        const glm::vec2 high =
            glm::min(from + size, glm::max(glm::vec2(a), glm::vec2(b)));
        if (high.x <= low.x || high.y <= low.y)
          continue;
        // Image u runs along +X; v runs down it (-Y).
        uvRect = glm::vec4((low.x - from.x) / size.x,
                           1.0f - (high.y - from.y) / size.y,
                           (high.x - low.x) / size.x,
                           (high.y - low.y) / size.y);
        shown = high - low;
        centre = glm::vec3((low + high) * 0.5f, centre.z);
      }
      instance.model[0] = glm::vec4(x * shown.x, 0.0f);
      instance.model[1] = glm::vec4(y * shown.y, 0.0f);
      instance.model[3] = glm::vec4(centre, 1.0f);
      instance.uvRect = uvRect;
    }
    else
    {
      // Lying on the ground: a rectangle of the plane, drawn as the plane is
      // (isometric, or a 3D world's ground).
      const float c = std::cos(decal.rotation), s = std::sin(decal.rotation);
      const glm::vec2 x(c, s), y(-s, c);
      const glm::vec2 centre = decal.position +
                               x * ((0.5f - decal.anchor.x) * decal.size.x) +
                               y * ((decal.anchor.y - 0.5f) * decal.size.y);
      const glm::vec3 origin = place(centre, decal.height);
      const glm::vec3 along =
          place(centre + x * decal.size.x, decal.height) - origin;
      const glm::vec3 up =
          place(centre + y * decal.size.y, decal.height) - origin;
      instance.model[0] = glm::vec4(along, 0.0f);
      instance.model[1] = glm::vec4(up, 0.0f);
      instance.model[3] = glm::vec4(origin, 1.0f);
    }
    instance.model[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    instance.tint = decal.color;
    if (decal.lifetime > 0.0f)
      instance.tint.a *=
          glm::clamp(1.0f - decal.age / decal.lifetime, 0.0f, 1.0f);
    m_commands[slot->second].instances.push_back(instance);
  }
}

} // namespace sfs
