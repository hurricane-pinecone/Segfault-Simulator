#include "engine/runtime/rendering/modules/spriteModule.h"

#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/shadowCasterComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/spriteTint.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/imageQuads.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>

namespace sfs
{

namespace
{
// Whether an image has soft edges (enough part-transparent texels to need
// blending rather than cutting out).
bool hasSoftEdges(const TextureData& texture)
{
  std::size_t soft = 0;
  for (std::size_t i = 3; i < texture.rgba.size(); i += 4)
    if (texture.rgba[i] > 8 && texture.rgba[i] < 247)
      ++soft;
  return soft * 50 > texture.rgba.size() / 4; // over 2% of it
}
} // namespace

SpriteModule::SpriteModule(AssetStore& assets, Facing facing)
    : m_assets(assets), m_facing(facing)
{
}

MaterialHandle SpriteModule::material(MeshLibrary& meshes,
                                      const std::string& sheet,
                                      const std::string& normalSheet)
{
  // Made again if its image has been (its file or settings changed).
  const std::string name = "sprite:" + sheet + "|" + normalSheet;
  const std::shared_ptr<const TextureData> image = m_assets.imageTexture(sheet);
  if (const auto it = m_materials.find(name);
      it != m_materials.end() && it->second.image == image.get())
    return it->second.handle;

  const MeshMaterial material = imageMaterial(
      m_assets,
      sheet,
      normalSheet,
      image && hasSoftEdges(*image) ? AlphaMode::Blend : AlphaMode::Mask);
  const MaterialHandle handle = meshes.setMaterial(name, material);
  m_materials[name] = {handle, image.get()};
  if (material.alphaMode == AlphaMode::Blend)
    m_softMaterials.insert(handle.id);
  else
    m_softMaterials.erase(handle.id);
  return handle;
}

void SpriteModule::computeCommands(const RenderContext& context)
{
  flush();
  m_placed.clear();
  if (!m_registry || !context.meshes || !context.camera)
    return;
  const bool flat =
      m_facing == Facing::Flat || (m_facing == Facing::Auto && context.space2D);
  const Space2D space = context.space2D ? *context.space2D : Space2D{};
  const float pixelsPerUnit =
      context.space2D ? space.pixelsPerUnit : m_pixelsPerUnit;
  MeshLibrary& meshes = *context.meshes;
  if (!m_quad.valid())
    m_quad = imageQuad(meshes);

  // Upright sprites stand across the view, facing the camera.
  const glm::vec3 across = context.camera->groundRight();
  const glm::vec3 up(0.0f, 0.0f, 1.0f);
  const glm::vec3 toward = glm::cross(across, up);

  std::unordered_map<uint32_t, std::size_t> byMaterial; // into m_commands
  for (const Entity& entity :
       m_registry->view<SpriteComponent, TransformComponent>())
  {
    const auto& sprite = entity.getComponent<SpriteComponent>();
    // A registered sprite, or a frame of an image.
    std::string sheetName;
    AssetStore::SpriteFrame frame;
    if (sprite.spriteId != SpriteComponent::kNoSprite)
    {
      const Sprite* registered = m_assets.getSprite(sprite.spriteId);
      if (!registered)
        continue;
      sheetName = registered->textureId;
      frame.sheet = m_assets.imageTexture(sheetName);
      if (!frame.sheet || frame.sheet->empty())
        continue;
      const glm::vec2 texels(frame.sheet->width, frame.sheet->height);
      const SDL_Rect& rect = registered->srcRect;
      frame.uvRect = glm::vec4(rect.x / texels.x,
                               rect.y / texels.y,
                               rect.w / texels.x,
                               rect.h / texels.y);
      frame.pixels = glm::vec2(rect.w, rect.h);
    }
    else if (!sprite.image.empty())
    {
      std::optional<AssetStore::SpriteFrame> found =
          m_assets.spriteFrame(sprite.image, sprite.frame);
      if (!found)
        continue;
      sheetName = sprite.image;
      frame = std::move(*found);
    }
    else
      continue;
    std::string normalSheet;
    if (entity.hasComponent<NormalMapComponent>())
      if (const Sprite* normal = m_assets.getSprite(
              entity.getComponent<NormalMapComponent>().spriteId))
        normalSheet = normal->textureId;
    const MaterialHandle look = material(meshes, sheetName, normalSheet);

    const auto& transform = entity.getComponent<TransformComponent>();
    // Its size in the world, and where its pivot sits on it (from its centre,
    // image y down).
    const glm::vec2 size = frame.pixels * transform.scale2D() / pixelsPerUnit;
    const glm::vec2 pivot(sprite.anchor.x - 0.5f, 0.5f - sprite.anchor.y);
    const glm::vec2 nudge =
        glm::vec2(sprite.renderOffset.x, -sprite.renderOffset.y) /
        pixelsPerUnit;
    const glm::vec2 centre = -pivot * size + nudge;
    // A negative scale mirrors it (its pivot too, above): the quad is drawn
    // the right way round, as a mirrored one would face away and be culled,
    // and its picture flipped instead.
    const glm::vec2 extent = glm::abs(size);

    MeshInstance instance;
    if (flat)
    {
      // Turned as the game's plane turns (the other way, if its y runs down).
      const float angle = transform.yaw() * space.ySign();
      const glm::vec3 x(std::cos(angle), std::sin(angle), 0.0f);
      const glm::vec3 y(-x.y, x.x, 0.0f);
      const int layer = entity.hasComponent<RenderLayerComponent>()
                            ? entity.getComponent<RenderLayerComponent>().layer
                            : 0;
      const glm::vec3 origin =
          space.place(transform.position2D(), transform.position.z, layer);
      // Standing up from the plane: leaning back as a real one would.
      const float lean = space.standingLean();
      const auto standing = [lean](glm::vec3 v)
      { return glm::vec3(v.x, v.y, v.y * lean); };
      instance.model[0] = glm::vec4(standing(x * extent.x), 0.0f);
      instance.model[1] = glm::vec4(standing(y * extent.y), 0.0f);
      instance.model[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
      instance.model[3] =
          glm::vec4(origin + standing(x * centre.x + y * centre.y), 1.0f);
    }
    else
    {
      instance.model[0] = glm::vec4(across * extent.x, 0.0f);
      instance.model[1] = glm::vec4(up * extent.y, 0.0f);
      instance.model[2] = glm::vec4(-toward, 0.0f);
      instance.model[3] = glm::vec4(
          transform.position + across * centre.x + up * centre.y, 1.0f);
    }
    if (entity.hasComponent<SpriteTint>())
    {
      const auto& tint = entity.getComponent<SpriteTint>();
      instance.tint = glm::vec4(tint.color, tint.alpha);
    }
    instance.uvRect = frame.uvRect;
    if (size.x < 0.0f)
      instance.uvRect = {instance.uvRect.x + instance.uvRect.z,
                         instance.uvRect.y,
                         -instance.uvRect.z,
                         instance.uvRect.w};
    if (size.y < 0.0f)
      instance.uvRect = {instance.uvRect.x,
                         instance.uvRect.y + instance.uvRect.w,
                         instance.uvRect.z,
                         -instance.uvRect.w};
    m_placed[entity.getId()] = instance.model;

    auto [at, added] = byMaterial.emplace(look.id, m_commands.size());
    if (added)
    {
      MeshDrawCommand command;
      if (m_softMaterials.contains(look.id))
        command.pass = RenderPass::Transparent;
      command.mesh = m_quad;
      command.materials = {look};
      m_commands.push_back(std::move(command));
    }
    MeshDrawCommand& command = m_commands[at->second];
    command.instances.push_back(instance);
    if (entity.hasComponent<ShadowCasterComponent>())
    {
      command.shadowInstances.push_back(instance);
      command.lightShadowInstances.push_back(instance);
    }
  }
}

} // namespace sfs
