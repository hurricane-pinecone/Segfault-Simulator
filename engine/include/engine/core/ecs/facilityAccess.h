#pragma once

// The engine's facilities, declared for the classes that hand them out:
// System, Service, GameObject and Scene each declare an accessor per facility
// (physics(), navigation(), ...), and each facility's own header
// (engine/runtime/facilities/) defines its class and those accessors. Core
// never calls an accessor, so it builds without the runtime.

namespace sfs
{

class AssetStore;
class Registry;

namespace facility
{
class Animation;
class Assets;
class Cameras;
class DebugDraw;
class Graphics;
class Meshes;
class Navigation;
class Physics3D;
class Prefabs;
} // namespace facility

namespace detail
{

/**
 * The engine's way to what a registry's host gave it, for the facilities
 * built on it. Engine-internal: game code never uses it (the engine-internal
 * lint).
 */
struct HostAccess
{
  /**
   * The asset store the registry's scene loads through.
   * @param registry the registry
   * @return its scene's asset store; null for a registry with no scene (a
   *         tool or test)
   */
  static AssetStore* assetStore(const Registry& registry);
};

} // namespace detail

} // namespace sfs
