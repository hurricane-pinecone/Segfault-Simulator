#include "engine/runtime/facilities/debugDraw.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/debugLines.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/rendering/renderSystem.h"

#include <optional>

namespace sfs::facility
{

void DebugDraw::forceColliders(std::optional<bool> on) const
{
  if (m_render)
    m_render->forceDrawColliders(on);
}

DebugDraw DebugDraw::in(Registry* registry)
{
  if (!debugFeatures::enabled() || !registry)
    return DebugDraw(nullptr, nullptr);

  RenderSystem* render = registry->tryGetSystem<RenderSystem>();
  return DebugDraw(render ? &render->debugLines() : nullptr, render);
}

} // namespace sfs::facility
