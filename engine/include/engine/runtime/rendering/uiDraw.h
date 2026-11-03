#pragma once

#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace sfs
{

/**
 * An image the UI draws from (a font's glyphs, an icon): RGBA8 pixels, rows
 * from the top. `revision` changes whenever the pixels do (a font adding
 * glyphs), so a renderer knows to upload them again. Its owner keeps it alive
 * while it's drawn.
 */
struct UiTexture
{
  uint32_t id = 0; // unique while it lives
  glm::ivec2 size{0, 0};
  std::vector<uint8_t> pixels;
  uint32_t revision = 0;
};

/**
 * One rectangle of UI, in pixels from the target's top left: a rounded box
 * (its colour, corners, edge softness), part of a texture (text, an image)
 * tinted by its colour, or a chevron. Only what's inside its clip is drawn
 * (a scroll view's content).
 */
struct UiQuad
{
  enum Kind : uint32_t
  {
    Box = 0,   // rounded rectangle
    Glyph = 1, // the texture's alpha, as coverage, in its colour
    Image = 2, // the texture's colours, tinted
    // A '>' in its colour, its stroke `border` wide, turned `radius` radians
    // clockwise (a quarter turn: pointing down).
    Chevron = 3,
  };

  glm::vec4 rect{0.0f};        // x, y, width, height
  glm::vec4 uv{0.0f};          // texture: u, v, width, height (0 to 1)
  glm::vec4 color{1.0f};       // as seen on screen (sRGB), and alpha
  float radius = 0.0f;         // Box: its corners'
  float softness = 0.0f;       // Box: how far its edge fades (a shadow's blur)
  float border = 0.0f;         // Box: a ring this wide inside its edge...
  glm::vec4 borderColor{0.0f}; // ...in this colour (sRGB) and alpha
  Kind kind = Box;
  const UiTexture* texture = nullptr; // Glyph and Image
  // Where it may draw: left, top, right, bottom (pixels); everywhere by
  // default.
  glm::vec4 clip{-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f};
};

/**
 * A frame's UI, back to front: drawn over everything else. Each part in
 * order (a UI layer's quads, kept from frame to frame while it's unchanged,
 * so shared rather than copied), then `quads`.
 */
struct UiDrawList
{
  std::vector<UiQuad> quads;
  std::vector<std::shared_ptr<const std::vector<UiQuad>>> parts;

  /** Each quad, in the order they're drawn. */
  template <typename Visit>
  void forEach(Visit&& visit) const
  {
    for (const auto& part : parts)
      for (const UiQuad& quad : *part)
        visit(quad);
    for (const UiQuad& quad : quads)
      visit(quad);
  }
};

} // namespace sfs
