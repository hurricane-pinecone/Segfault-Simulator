#include "engine/runtime/facilities/graphics.h"

#include "absentBackend.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/rendering/renderStats.h"
#include "engine/runtime/rendering/renderSystem.h"

#include <glm/glm/ext/vector_int2.hpp>

#include <atomic>
#include <memory>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

RenderModuleHost<RenderContext>* Graphics::backend() const
{
  if (!m_host)
    warnAbsent(s_warned, "graphics", "RenderSystem");

  return m_host;
}

Graphics Graphics::in(Registry* registry)
{
  RenderSystem* render =
      registry ? registry->tryGetSystem<RenderSystem>() : nullptr;
  return Graphics(render);
}

const RenderStats& Graphics::stats() const
{
  static const RenderStats none;
  RenderModuleHost<RenderContext>* host = backend();
  return host ? static_cast<RenderSystem*>(host)->stats() : none;
}

void Graphics::updateTexture(const std::shared_ptr<const TextureData>& texture,
                             glm::ivec2 from,
                             glm::ivec2 to) const
{
  if (RenderModuleHost<RenderContext>* host = backend())
    static_cast<RenderSystem*>(host)->renderer().updateTexture(
        texture, from, to);
}

} // namespace sfs::facility
