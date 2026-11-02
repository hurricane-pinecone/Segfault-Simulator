#include "engine/runtime/rendering/modules/meshModule.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering/meshLibrary.h"

namespace sfs
{

void MeshModule::computeCommands(const RenderContext& context)
{
  flush();
  m_commandIndex.clear();

  if (!m_registry || !context.meshes)
    return;

  // Without making a list of them: this runs every frame.
  m_registry->each<TransformComponent, MeshRendererComponent>(
      [&](const Entity& entity,
          const TransformComponent&,
          const MeshRendererComponent& renderer)
      { addEntity(context, entity, renderer); });
}

void MeshModule::addEntity(const RenderContext& context,
                           const Entity& entity,
                           const MeshRendererComponent& renderer)
{
  const Aabb* localBounds = context.meshes->bounds(renderer.mesh);
  if (!localBounds)
    return; // invalid handle, or a mesh from another library
  // A skinned mesh an animator poses: its own deformed copy, culled as posed.
  MeshHandle drawn = renderer.mesh;
  const MeshLibrary::SkinPose* skin =
      context.meshes->hasSkinPoses()
          ? context.meshes->skinPose(renderer.mesh, entity.getId())
          : nullptr;
  if (skin)
  {
    if (skin->gpu.valid())
      drawn = skin->gpu;
    if (!skin->bounds.empty())
      localBounds = &skin->bounds;
  }

  const glm::mat4 model = hierarchy::worldMatrix(entity);
  const Aabb worldBounds = localBounds->transformed(model);

  const bool visible = context.frustum.intersects(worldBounds);
  const bool castsShadow = renderer.castsShadow && context.shadowsEnabled &&
                           context.shadowFrustum.intersects(worldBounds);
  bool castsLightShadow = false;
  if (renderer.castsShadow && context.shadowedLights)
    for (const PointLight& light : *context.shadowedLights)
    {
      const glm::vec4 reach = lightBounds(light); // a spot's: round its cone
      if (worldBounds.intersectsSphere(glm::vec3(reach), reach.w))
      {
        castsLightShadow = true;
        break;
      }
    }
  if (!visible && !castsShadow && !castsLightShadow)
    return;
  if (skin)
    context.meshes->markSkinDrawn(renderer.mesh, entity.getId());

  // One command per mesh and set of materials: those draw as one instanced
  // batch.
  m_materials.clear();
  for (const std::string& name : renderer.materials)
    m_materials.push_back(context.meshes->material(name));
  while (!m_materials.empty() && !m_materials.back().valid())
    m_materials.pop_back(); // trailing "own material" slots change nothing
  std::size_t key = std::hash<uint32_t>()(drawn.id);
  for (const MaterialHandle material : m_materials)
    key = key * 1000003u ^ material.id;
  const auto matches = [&](const MeshDrawCommand& command)
  { return command.mesh == drawn && command.materials == m_materials; };

  auto [first, last] = m_commandIndex.equal_range(key);
  while (first != last && !matches(m_commands[first->second]))
    ++first;
  const std::size_t index = first != last ? first->second : m_commands.size();
  if (first == last)
  {
    m_commandIndex.emplace(key, index);
    m_commands.push_back(
        MeshDrawCommand{.mesh = drawn, .materials = m_materials});
  }

  MeshDrawCommand& command = m_commands[index];
  MeshInstance instance{model, renderer.tint};
  instance.emission =
      glm::vec4(renderer.emissionColor * renderer.emission, 0.0f);
  if (visible)
    command.instances.push_back(instance);
  if (castsShadow)
    command.shadowInstances.push_back(instance);
  if (castsLightShadow)
    command.lightShadowInstances.push_back(instance);
}

} // namespace sfs
