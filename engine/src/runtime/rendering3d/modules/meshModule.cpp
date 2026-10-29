#include "engine/runtime/rendering3d/modules/meshModule.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering3d/meshLibrary.h"

namespace sfs
{

void MeshModule::computeCommands(const RenderContext3D& context)
{
  flush();
  m_commandIndex.clear();

  if (!m_registry || !context.meshes)
    return;

  for (const Entity& entity :
       m_registry->view<Transform3DComponent, MeshRendererComponent>())
  {
    const auto& renderer = entity.getComponent<MeshRendererComponent>();
    const Aabb* localBounds = context.meshes->bounds(renderer.mesh);
    if (!localBounds)
      continue; // invalid handle, or a mesh from another library

    const glm::mat4 model = hierarchy::worldMatrix(entity);
    const Aabb worldBounds = localBounds->transformed(model);

    const bool visible = context.frustum.intersects(worldBounds);
    const bool castsShadow = renderer.castsShadow && context.shadowsEnabled &&
                             context.shadowFrustum.intersects(worldBounds);
    bool castsLightShadow = false;
    if (renderer.castsShadow && context.shadowedLights)
      for (const PointLight3D& light : *context.shadowedLights)
        if (worldBounds.intersectsSphere(light.position, light.radius))
        {
          castsLightShadow = true;
          break;
        }
    if (!visible && !castsShadow && !castsLightShadow)
      continue;

    // One command per mesh and set of materials: those draw as one instanced
    // batch.
    m_materials.clear();
    for (const std::string& name : renderer.materials)
      m_materials.push_back(context.meshes->material(name));
    while (!m_materials.empty() && !m_materials.back().valid())
      m_materials.pop_back(); // trailing "own material" slots change nothing
    std::size_t key = std::hash<uint32_t>()(renderer.mesh.id);
    for (const MaterialHandle material : m_materials)
      key = key * 1000003u ^ material.id;
    const auto matches = [&](const MeshDrawCommand& command) {
      return command.mesh == renderer.mesh && command.materials == m_materials;
    };

    auto [first, last] = m_commandIndex.equal_range(key);
    while (first != last && !matches(m_commands[first->second]))
      ++first;
    const std::size_t index = first != last ? first->second : m_commands.size();
    if (first == last)
    {
      m_commandIndex.emplace(key, index);
      m_commands.push_back(
          MeshDrawCommand{.mesh = renderer.mesh, .materials = m_materials});
    }

    MeshDrawCommand& command = m_commands[index];
    const MeshInstance instance{model, renderer.tint};
    if (visible)
      command.instances.push_back(instance);
    if (castsShadow)
      command.shadowInstances.push_back(instance);
    if (castsLightShadow)
      command.lightShadowInstances.push_back(instance);
  }
}

} // namespace sfs
