#pragma once

#include "engine/runtime/rendering3d/uiDraw.h"
#include "engine/runtime/ui/ui.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Laying a ui::View tree out on the screen: how big each piece is, where it
// goes, what it draws and what can be clicked. Everything here is in pixels;
// a view's points are multiplied by the layout's scale.
namespace sfs::ui
{

/** A rectangle in pixels from the screen's top left. */
struct Rect
{
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  bool contains(glm::vec2 point) const
  {
    return point.x >= x && point.y >= y && point.x < x + width &&
           point.y < y + height;
  }
};

/**
 * Measures and draws lines of text, in pixels: a font atlas (FontAtlas), or
 * a stand-in in tests. `font` is a Font's file (resolved, never empty).
 */
class TextShaper
{
public:
  virtual ~TextShaper() = default;
  /** How far apart lines are. */
  virtual float lineHeight(const std::string& font, float pixels) = 0;
  /** How wide a line is. */
  virtual float
  width(const std::string& font, float pixels, std::string_view line) = 0;
  /** A line's glyphs as quads, its top left at `at`, in `color` (sRGB). */
  virtual void draw(const std::string& font,
                    float pixels,
                    std::string_view line,
                    glm::vec2 at,
                    glm::vec4 color,
                    std::vector<UiQuad>& out) = 0;
};

/**
 * The pictures Image views show: a texture for an image file or a mesh's icon
 * (`mesh`), or null if there's none (yet): then nothing's drawn.
 */
class ImageSource
{
public:
  virtual ~ImageSource() = default;
  virtual const UiTexture* image(const std::string& source, bool mesh) = 0;
};

/** The colours and default font everything's drawn with. */
struct Theme
{
  std::string font = "assets/fonts/m6x11.ttf";
  Color text = Color::rgb(250, 244, 230);
  Color button = Color::rgb(244, 230, 200);
  Color buttonHovered = Color::rgb(255, 245, 222);
  Color buttonPressed = Color::rgb(222, 204, 170);
  Color buttonText = Color::rgb(60, 45, 32);
  Color accent = Color::rgb(140, 196, 98);
  Color track = Color::rgb(0, 0, 0, 0.35f);
  float buttonRadius = 8.0f;
};

/**
 * Something on the screen the pointer can be over: a button (`action`), a
 * view with help, or just UI covering the scene (clicks there aren't the
 * game's).
 */
struct Hit
{
  std::string id;
  Rect rect;
  std::function<void()> action; // a button's (empty: none)
  bool enabled = true;
  std::string help;
};

/** Where a view went (for tools: the editor's UI preview outlines them). */
struct Placed
{
  Rect rect;
  Node::Kind kind = Node::Kind::Empty;
  int depth = 0;     // 0 the body, 1 its children...
  std::string label; // a Text's or Image's text, else empty
};

/** A view kind's name ("VStack", "Padding"...). */
const char* kindName(Node::Kind kind);

/** Where the pointer is, and what it's over and holding (from last frame's
 * hits). */
struct Pointer
{
  glm::vec2 position{-1.0e9f};
  std::string hovered; // a Hit's id
  std::string pressed; // a button held down
};

/**
 * Lay `root` out on a screen `size` pixels, `scale` pixels a point: placed by
 * its align() on the screen. Its quads go on `quads`, back to front, and what
 * the pointer can be over on `hits`, front last; every view's rect on
 * `placed` (if given), parents before their children.
 */
void layout(const View& root,
            glm::vec2 size,
            float scale,
            const Theme& theme,
            TextShaper& text,
            const Pointer& pointer,
            std::vector<UiQuad>& quads,
            std::vector<Hit>& hits,
            ImageSource* images = nullptr,
            std::vector<Placed>* placed = nullptr);

/**
 * How big `root` is laid out in `offer` pixels: its own size, or as much as
 * it's offered along a way it stretches (as layout() places a body).
 */
glm::vec2 fit(const View& root,
              glm::vec2 offer,
              float scale,
              const Theme& theme,
              TextShaper& text,
              ImageSource* images = nullptr);

/** A tooltip's box and text by the pointer, kept on the screen. */
void tooltip(std::string_view text,
             glm::vec2 pointer,
             glm::vec2 size,
             float scale,
             const Theme& theme,
             TextShaper& shaper,
             std::vector<UiQuad>& quads);

} // namespace sfs::ui
