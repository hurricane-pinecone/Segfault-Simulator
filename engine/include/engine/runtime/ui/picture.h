#pragma once

#include "engine/runtime/rendering/uiDraw.h"

#include <cstdint>

namespace sfs::ui
{

struct Color;

/**
 * Pixels the game draws and changes, shown by an Image (Image(picture)): a
 * yield map, a minimap. RGBA8, rows from the top, colours as they look on
 * screen (sRGB). Make one shared (std::make_shared<ui::Picture>(w, h)), keep
 * it, write its pixels and call changed(): the next frame shows them. A
 * change only uploads the pixels again; nothing's laid out again (a resize
 * lays out the views showing it). Keep it alive while it's shown.
 *
 *   m_yield->set(x, y, Color::rgb(90, 160, 60));
 *   m_yield->changed();
 *   ...
 *   Image(m_yield).resizable().interpolation(Interpolation::Nearest)
 */
class Picture
{
public:
  Picture(int width, int height);
  Picture(int width, int height, const Color& fill);
  Picture(const Picture&) = delete;
  Picture& operator=(const Picture&) = delete;

  glm::ivec2 size() const { return m_texture.size; }
  /** Its pixels: size().x * size().y * 4 bytes, R G B A. */
  uint8_t* pixels() { return m_texture.pixels.data(); }
  const uint8_t* pixels() const { return m_texture.pixels.data(); }
  /** One pixel (outside it: nothing). */
  void set(int x, int y, const Color& color);
  /** Every pixel. */
  void fill(const Color& color);
  /** Another size, every pixel clear. */
  void resize(int width, int height);
  /** Its pixels have been written: shown from the next frame. */
  void changed() { ++m_texture.revision; }
  /** Counted up by changed(). */
  uint32_t version() const { return m_texture.revision; }

  /** What the renderer draws (its revision is version()). */
  const UiTexture& texture() const { return m_texture; }

private:
  UiTexture m_texture;
};

} // namespace sfs::ui
