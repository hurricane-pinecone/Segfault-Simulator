#pragma once

#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>

namespace sfs
{

/**
 * A picture a backend draws into offscreen (a scene, a preview), shown in a
 * tool's panel through ImGui: ImGui::Image(ImTextureID(target.imguiTexture()),
 * ...). Made by IRenderBackend::createRenderTarget(), and only used with the
 * backend that made it.
 */
class IRenderTarget
{
public:
  virtual ~IRenderTarget() = default;

  /** Resize to `size` pixels (kept if it's the same); 0 or less frees it. */
  virtual void resize(glm::ivec2 size) = 0;
  virtual glm::ivec2 size() const = 0;
  /** Whether it has pixels to draw into (a size above 0). */
  virtual bool valid() const = 0;
  /** What ImGui shows it as (its ImTextureID). */
  virtual uint64_t imguiTexture() const = 0;
};

/**
 * A fixed picture made from pixels (an asset's thumbnail), shown through
 * ImGui. Made by IRenderBackend::createImage().
 */
class IImage
{
public:
  virtual ~IImage() = default;

  virtual glm::ivec2 size() const = 0;
  /** What ImGui shows it as (its ImTextureID). */
  virtual uint64_t imguiTexture() const = 0;
};

} // namespace sfs
