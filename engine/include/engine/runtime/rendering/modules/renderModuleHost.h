#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include <memory>
#include <typeindex>
#include <utility>
#include <vector>

namespace sfs
{

class IDecalSink;

/**
 * Mixin that lets a render system compose render modules. Registration is the
 * enable: a feature is on iff its module type is registered. The host
 * constructs + owns modules, init()s them with the host's dependencies, and
 * exposes them for the render system to drive (update/emit) and for tooling to
 * inspect (settings).
 *
 * Templated on the render context the modules consume, so the same machinery
 * serves any render system -- the isometric heightfield path today, a flat-2D
 * path later -- while keeping host and module types matched at compile time: a
 * host for one context cannot hold a module written against another.
 */
template <typename TContext>
class RenderModuleHost
{
public:
  using Module = IRenderModule<TContext>;

  virtual ~RenderModuleHost() = default;

  /**
   * Compose a render module into the frame. The host constructs + owns the
   * module, init()s it with the host's dependencies, and returns it for
   * configuration. A module type may be registered only once; re-registering
   * returns the existing instance.
   *
   * @return a reference to the registered module.
   */
  template <class T, class... A>
  T& withModule(A&&... args)
  {
    if (T* existing = module<T>())
    {
      LOG_ERROR("withModule: module type already registered; "
                "returning the existing instance");
      return *existing;
    }

    auto module = std::make_unique<T>(std::forward<A>(args)...);
    module->init(moduleInit());
    module->setHost(*this);

    T& ref = *module;
    m_modules.emplace_back(std::type_index(typeid(T)), std::move(module));
    return ref;
  }

  /** Compose several default-constructed modules at once. */
  template <class... Ts>
  void withModules()
  {
    (withModule<Ts>(), ...);
  }

  /**
   * @return whether any registered module renders the terrain surface itself
   * (block geometry, voxels), so the host suppresses billboard tiles. Keeps the
   * render system from hard-coding which concrete module owns terrain geometry.
   */
  bool terrainGeometryActive() const
  {
    for (const auto& [type, module] : m_modules)
      if (module->providesTerrainGeometry())
        return true;
    return false;
  }

  /** @return whether a module of type T is registered. */
  template <class T>
  bool hasModule() const
  {
    const std::type_index key(typeid(T));

    for (const auto& [type, module] : m_modules)
      if (type == key)
        return true;

    return false;
  }

  /** @return the registered module of type T, or nullptr if absent. */
  template <class T>
  T* module()
  {
    const std::type_index key(typeid(T));

    for (const auto& [type, module] : m_modules)
      if (type == key)
        return static_cast<T*>(module.get());

    return nullptr;
  }

  /** Unregister (and destroy) the module of type T, disabling its feature. */
  template <class T>
  void removeModule()
  {
    const std::type_index key(typeid(T));

    for (auto it = m_modules.begin(); it != m_modules.end(); ++it)
    {
      if (it->first == key)
      {
        m_modules.erase(it);
        return;
      }
    }
  }

  /**
   * The host's decal sink, if it has one -- where particles stamp persistent
   * marks. A module (e.g. Particles) pulls this in its enableStains() so a game
   * never wires the sink by hand. Null on a host with no decal surface.
   */
  virtual IDecalSink* decalSink() { return nullptr; }

protected:
  /** Dependencies passed to each module's init(); supplied by the host. */
  virtual ModuleInit moduleInit() = 0;

  /** Advance every registered module's per-frame simulation. */
  void updateModules(double deltaTime)
  {
    for (auto& [type, module] : m_modules)
      module->update(deltaTime);
  }

  // Registered modules, owned by the host and kept in registration order.
  std::vector<std::pair<std::type_index, std::unique_ptr<Module>>> m_modules;
};

} // namespace sfs
