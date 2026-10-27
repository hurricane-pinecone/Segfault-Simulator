#include "engine/runtime/rendering3d/modules/meshModule.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/registry.h"
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

    const glm::mat4 model = entity.getComponent<Transform3DComponent>().matrix();
    const Aabb worldBounds = localBounds->transformed(model);

    const bool visible = context.frustum.intersects(worldBounds);
    const bool castsShadow = renderer.castsShadow && context.shadowsEnabled &&
                             context.shadowFrustum.intersects(worldBounds);
    if (!visible && !castsShadow)
      continue;

    const auto [it, inserted] =
        m_commandIndex.try_emplace(renderer.mesh.id, m_commands.size());
    if (inserted)
      m_commands.push_back(MeshDrawCommand{.mesh = renderer.mesh});

    MeshDrawCommand& command = m_commands[it->second];
    const MeshInstance instance{model, renderer.tint};
    if (visible)
      command.instances.push_back(instance);
    if (castsShadow)
      command.shadowInstances.push_back(instance);
  }
}

} // namespace sfs
