#include "engine/runtime/rendering3d/modules/spriteModule.h"

#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/shadowCasterComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/spriteTint.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/modules/imageQuads.h"
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
  const std::string name = "sprite:" + sheet + "|" + normalSheet;
  if (const auto it = m_materials.find(name); it != m_materials.end())
    return it->second;

  const std::shared_ptr<const TextureData> image = m_assets.imageTexture(sheet);
  const MeshMaterial material = imageMaterial(
      m_assets,
      sheet,
      normalSheet,
      image && hasSoftEdges(*image) ? AlphaMode::Blend : AlphaMode::Mask);
  const MaterialHandle handle = meshes.setMaterial(name, material);
  m_materials.emplace(name, handle);
  if (material.alphaMode == AlphaMode::Blend)
    m_softMaterials.insert(handle.id);
  return handle;
}

void SpriteModule::computeCommands(const RenderContext3D& context)
{
  flush();
  if (!m_registry || !context.meshes || !context.camera)
    return;
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
    const Sprite* frame = m_assets.getSprite(sprite.spriteId);
    if (!frame)
      continue;
    const std::shared_ptr<const TextureData> sheet =
        m_assets.imageTexture(frame->textureId);
    if (!sheet || sheet->empty())
      continue;
    std::string normalSheet;
    if (entity.hasComponent<NormalMapComponent>())
      if (const Sprite* normal = m_assets.getSprite(
              entity.getComponent<NormalMapComponent>().spriteId))
        normalSheet = normal->textureId;
    const MaterialHandle look = material(meshes, frame->textureId, normalSheet);

    const auto& transform = entity.getComponent<TransformComponent>();
    // Its size in the world, and where its pivot sits on it (from its centre,
    // image y down).
    const glm::vec2 size = glm::vec2(frame->srcRect.w, frame->srcRect.h) *
                           transform.scale2D() / pixelsPerUnit;
    const glm::vec2 pivot(sprite.anchor.x - 0.5f, 0.5f - sprite.anchor.y);
    const glm::vec2 nudge =
        glm::vec2(sprite.renderOffset.x, -sprite.renderOffset.y) /
        pixelsPerUnit;
    const glm::vec2 centre = -pivot * size + nudge;

    MeshInstance instance;
    if (m_facing == Facing::Flat)
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
      instance.model[0] = glm::vec4(standing(x * size.x), 0.0f);
      instance.model[1] = glm::vec4(standing(y * size.y), 0.0f);
      instance.model[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
      instance.model[3] =
          glm::vec4(origin + standing(x * centre.x + y * centre.y), 1.0f);
    }
    else
    {
      instance.model[0] = glm::vec4(across * size.x, 0.0f);
      instance.model[1] = glm::vec4(up * size.y, 0.0f);
      instance.model[2] = glm::vec4(-toward, 0.0f);
      instance.model[3] = glm::vec4(
          transform.position + across * centre.x + up * centre.y, 1.0f);
    }
    if (entity.hasComponent<SpriteTint>())
    {
      const auto& tint = entity.getComponent<SpriteTint>();
      instance.tint = glm::vec4(tint.color, tint.alpha);
    }
    const glm::vec2 texels(sheet->width, sheet->height);
    instance.uvRect = glm::vec4(frame->srcRect.x / texels.x,
                                frame->srcRect.y / texels.y,
                                frame->srcRect.w / texels.x,
                                frame->srcRect.h / texels.y);

    auto [at, added] = byMaterial.emplace(look.id, m_commands.size());
    if (added)
    {
      MeshDrawCommand command;
      if (m_softMaterials.contains(look.id))
        command.pass = RenderPass3D::Transparent;
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
