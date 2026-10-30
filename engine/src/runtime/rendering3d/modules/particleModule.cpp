#include "engine/runtime/rendering3d/modules/particleModule.h"

#include "engine/core/particles/decal.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/particles/colliderParticleCollision.h"
#include "engine/runtime/particles/terrainParticleCollision.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/modules/imageQuads.h"
#include "glm/glm/geometric.hpp"

#include <cmath>

namespace sfs
{

ParticleModule::ParticleModule(AssetStore& assets) : m_assets(assets) {}

ParticleModule::~ParticleModule() = default;

void ParticleModule::init(const ModuleInit& init)
{
  m_registry = init.registry;
  setRegistry(init.registry);
}

void ParticleModule::enableStains(IDecalSink& decals,
                                  const ITerrainSurfaceSource* terrain)
{
  setDecalSink(&decals);
  if (terrain)
    m_collision = std::make_unique<TerrainParticleCollision>(terrain);
  else
    m_collision = std::make_unique<ColliderParticleCollision>(m_registry);
  setCollisionSource(m_collision.get());
}

void ParticleModule::computeCommands(const RenderContext3D& context)
{
  flush();
  if (!context.meshes || !context.camera)
    return;
  MeshLibrary& meshes = *context.meshes;
  if (!m_quad.valid())
    m_quad = imageQuad(meshes);

  const Space2D* space = context.space2D;
  // Quads face the camera: across the view, and up it.
  const glm::vec3 view = context.camera->viewDirection();
  const glm::vec3 across = context.camera->groundRight();
  const glm::vec3 up = glm::cross(across, view);

  std::vector<ParticleSpriteBatch> batches;
  sprites(batches);
  for (const ParticleSpriteBatch& batch : batches)
  {
    // Screen-space effects (simulated in pixels) aren't drawn here.
    if (batch.screenSpace || !batch.textureId)
      continue;
    const bool glow = batch.blend == BlendMode::Additive;
    const std::string name =
        "particles:" + *batch.textureId + (glow ? ":add" : ":blend");
    auto [at, added] = m_materials.emplace(name, MaterialHandle{});
    if (added)
      at->second = meshes.setMaterial(
          name,
          imageMaterial(m_assets,
                        *batch.textureId,
                        {},
                        glow ? AlphaMode::Additive : AlphaMode::Blend,
                        true));

    MeshDrawCommand command;
    command.pass = RenderPass3D::Transparent;
    command.mesh = m_quad;
    command.materials = {at->second};
    command.instances.reserve(batch.sprites.size());
    const float scale = space ? space->scale() : 1.0f;
    for (const ParticleSprite& sprite : batch.sprites)
    {
      const glm::vec3 centre =
          space ? space->place(sprite.position, sprite.height)
                : glm::vec3(sprite.position, sprite.height);
      const float size = sprite.size * scale;
      const float c = std::cos(sprite.rotation), s = std::sin(sprite.rotation);
      // In a 2D game the camera looks straight at the plane: the quad lies in
      // it, turned by the particle's rotation.
      const glm::vec3 x = space ? glm::vec3(c, s, 0.0f) : c * across + s * up;
      const glm::vec3 y = space ? glm::vec3(-s, c, 0.0f) : -s * across + c * up;
      MeshInstance instance;
      // In an isometric game, standing up from the plane as sprites do.
      const float lean = space ? space->standingLean() : 0.0f;
      instance.model[0] =
          glm::vec4(x * size + glm::vec3(0, 0, x.y * size * lean), 0.0f);
      instance.model[1] =
          glm::vec4(y * size + glm::vec3(0, 0, y.y * size * lean), 0.0f);
      instance.model[2] = glm::vec4(glm::cross(x, y), 0.0f);
      instance.model[3] = glm::vec4(centre, 1.0f);
      instance.tint = sprite.color;
      instance.uvRect = sprite.uvRect;
      command.instances.push_back(instance);
    }
    m_commands.push_back(std::move(command));
  }
}

std::vector<ModuleSetting> ParticleModule::settings(const RenderContext3D&)
{
  return {settings::text("Live particles",
                         [this]
                         { return std::to_string(liveParticleCount()); })};
}

} // namespace sfs
