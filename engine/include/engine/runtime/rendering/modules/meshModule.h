#pragma once

#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sfs
{

class Registry;

/**
 * Render module that draws every entity with a TransformComponent and a
 * MeshRendererComponent. Entities sharing a mesh merge into one
 * MeshDrawCommand (one instanced draw). Each instance is culled twice: against
 * the camera frustum for the lit pass, and against the sun's shadow volume for
 * the shadow pass.
 */
class MeshModule : public CommandModule<RenderContext, MeshDrawCommand>
{
public:
  void init(const ModuleInit& m) override { m_registry = m.registry; }

  void computeCommands(const RenderContext& context) override;

private:
  Registry* m_registry = nullptr;

  // Hash of (mesh, materials) -> index into m_commands, rebuilt each frame
  // (kept to reuse its buckets).
  std::unordered_multimap<std::size_t, std::size_t> m_commandIndex;
  std::vector<MaterialHandle> m_materials; // scratch: one entity's
};

} // namespace sfs
