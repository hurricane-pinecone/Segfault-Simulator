#pragma once

#include "engine/runtime/rendering/uiDraw.h"
#include "glm/glm/ext/vector_int2.hpp"

namespace sfs
{

/**
 * A picture the GPU draws into (a camera feed: see CameraFeedComponent),
 * shown by a UI Image (ui::Image(texture)) without its pixels ever coming
 * back to the CPU. The renderer holds the picture itself, by its texture's
 * id, made the first time it's drawn or shown and let go once it's neither
 * for a while. A new picture in it lays nothing out again (a resize lays out
 * the views showing it). Keep it alive while it's shown.
 */
class RenderTexture
{
public:
  explicit RenderTexture(glm::ivec2 size);
  RenderTexture(const RenderTexture&) = delete;
  RenderTexture& operator=(const RenderTexture&) = delete;

  glm::ivec2 size() const { return m_texture.size; }
  /** Another size (at least 1 x 1): a new picture, from the next frame. */
  void resize(glm::ivec2 size);

  /** What a UI draws from (UiTexture::gpu). */
  const UiTexture& texture() const { return m_texture; }

private:
  UiTexture m_texture;
};

} // namespace sfs
