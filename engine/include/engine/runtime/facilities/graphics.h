#pragma once

#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/rendering/renderStats.h"
#include "engine/runtime/sceneManager/scene.h"

#include <glm/glm/ext/vector_int2.hpp>

#include <memory>
#include <utility>

namespace sfs
{

struct TextureData;

namespace facility
{

/**
 * The scene's rendering, as game code uses it (`graphics()` on a System,
 * Service, GameObject or Scene): the render modules (the engine's: grass,
 * deformation, particles, UI...; and a game's own, added here), what the last
 * frame drew, and textures changed as the game runs. A value handle over the
 * scene's RenderSystem, valid for the step that got it: get it where it's
 * used and keep it in no member. A module it gives is valid for this step
 * only too. In a scene that doesn't render its commands do nothing and its
 * queries give empty answers, with one warning a run (available() tells).
 */
class Graphics
{
public:
  /**
   * Adds a render module to the scene's frame: a game's own (its own decals
   * drawn its way). The renderer builds, owns and initialises it; a type is
   * added once (again: the one there, logged).
   * @param args what its constructor takes
   * @return the module, valid for this step; null when the scene doesn't
   *         render
   */
  template <typename TModule, typename... TArgs>
  inline TModule* addModule(TArgs&&... args) const
  {
    RenderModuleHost<RenderContext>* host = backend();
    return host ? &host->template withModule<TModule>(
                      std::forward<TArgs>(args)...)
                : nullptr;
  }

  /** @return whether the scene renders behind this facade */
  inline bool available() const { return m_host != nullptr; }

  /**
   * The rendering of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no RenderSystem
   */
  static Graphics in(Registry* registry);

  /**
   * One of the scene's render modules, by type (GrassModule,
   * DeformationModule, ParticleModule, UiModule, or a game's own), valid for
   * this step only: use it now, never keep it.
   * @return the module; null when it isn't added, or the scene doesn't render
   */
  template <typename TModule>
  inline TModule* module() const
  {
    RenderModuleHost<RenderContext>* host = backend();
    return host ? host->template module<TModule>() : nullptr;
  }

  /** @return what the last frame drew; zeros when the scene doesn't render */
  const RenderStats& stats() const;

  /**
   * Shows a texture's changed pixels (a splat map being painted) on every
   * material using it. Only textures uploaded without mipmaps change.
   * @param texture the texture
   * @param from the changed rectangle's first pixel
   * @param to the pixel past its last
   */
  void updateTexture(const std::shared_ptr<const TextureData>& texture,
                     glm::ivec2 from,
                     glm::ivec2 to) const;

private:
  inline explicit Graphics(RenderModuleHost<RenderContext>* host) : m_host(host)
  {
  }

  // The scene's render modules; null (and warned about once) when it
  // doesn't render.
  RenderModuleHost<RenderContext>* backend() const;

  // The RenderSystem, as the host of its modules.
  RenderModuleHost<RenderContext>* m_host = nullptr;
};

} // namespace facility

inline facility::Graphics System::graphics() const
{
  return facility::Graphics::in(registry);
}

inline facility::Graphics Service::graphics() const
{
  return facility::Graphics::in(m_registry);
}

inline facility::Graphics GameObject::graphics() const
{
  return m_scene ? m_scene->graphics() : facility::Graphics::in(nullptr);
}

inline facility::Graphics Scene::graphics()
{
  return facility::Graphics::in(&m_registry);
}

} // namespace sfs
