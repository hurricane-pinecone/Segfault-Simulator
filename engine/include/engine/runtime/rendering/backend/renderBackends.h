#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

/**
 * The render backends a program can run on, by name: the engine's ("webgpu",
 * see registerWebGpuBackend(), which runs on Vulkan, Direct3D 12, Metal or,
 * on machines without them, OpenGL) and any of a program's own. A game's
 * manifest names the one it prefers; if that one can't start (no driver for
 * it), Game tries the others in the order they were added.
 */
class RenderBackends
{
public:
  using Factory = std::function<std::unique_ptr<IRenderBackend>()>;

  /** Offer a backend (replacing any of the same name). */
  static void add(std::string name, Factory make);

  /** A new backend of that name; null if there's none. */
  static std::unique_ptr<IRenderBackend> make(std::string_view name);

  /** Every backend's name, in the order added. */
  static std::vector<std::string> names();
};

} // namespace sfs
