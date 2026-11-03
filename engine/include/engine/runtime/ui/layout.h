#pragma once

#include "engine/runtime/rendering/uiDraw.h"
#include "engine/runtime/ui/ui.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
  /** Whether any of it is inside `other`. */
  bool overlaps(const Rect& other) const
  {
    return x < other.x + other.width && other.x < x + width &&
           y < other.y + other.height && other.y < y + height;
  }
  /** The part of it inside `other` (no size if none). */
  Rect intersect(const Rect& other) const;
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
  /**
   * Counted up whenever glyph quads drawn before may no longer be right (a
   * texture grew, so its coordinates moved, or a font was let go): kept
   * layouts (Cache) are drawn again.
   */
  virtual uint64_t generation() const { return 0; }
  /** Quads drawn from `texture` are still shown (kept from a past frame). */
  virtual void keep(const UiTexture* texture) { (void)texture; }
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
  // A scroll view's bar: its thumb, and the thumb hovered or held.
  Color scrollThumb = Color::rgb(250, 244, 230, 0.35f);
  Color scrollThumbHovered = Color::rgb(250, 244, 230, 0.6f);
};

/**
 * A view's identity in its layer: a hash of its place in the tree (or of its
 * id(), View::id()). 0: none.
 */
using Id = uint64_t;

/** The State key of a scroll view or disclosure group given `id` with id(). */
Id idKey(std::string_view id);

/**
 * Something on the screen the pointer can be over: a button (`action`), a
 * view with help, or just UI covering the scene (clicks there aren't the
 * game's).
 *
 * What a hit does with what the game gave its view (a button's action, a
 * slider's change) it takes from its view's node, passed in: the node at
 * `source` in the layer's newest tree (nodeAt()), so a layout kept from an
 * earlier frame calls today's functions. The rest (a scroll view's) works on
 * the layer's State.
 */
struct Hit
{
  Id id = 0;
  Rect rect;
  // Clicked (pressed and let go over it): a button's action, a toggle's or a
  // section's change. Empty: it can't be clicked.
  std::function<void(const Node& source)> action;
  bool enabled = true;
  std::string help;
  // A slider's: called with the pointer's position (pixels) as it's pressed
  // on it, and on each frame it's held down after, wherever it goes.
  std::function<void(const Node& source, glm::vec2 at)> drag;
  // Called once with the pointer's position as it's pressed on it, before
  // drag (a scroll bar's: grab its thumb, or page).
  std::function<void(glm::vec2)> press;
  // A scroll view's: scroll it by `delta` pixels (+y further down its
  // content); false if it can't go that way (the wheel goes on to one it's
  // in).
  std::function<bool(glm::vec2)> scroll;
  // The view it's for: its place in the tree, counted from the body (0) in
  // pre-order (Node::count()).
  uint32_t source = 0;
};

/** The node `index` places from `root` in pre-order (Hit::source); null if
 * there's none. */
const Node* nodeAt(const Node& root, uint32_t index);

/**
 * What views keep from frame to frame, by their place in the tree (or id):
 * scroll views' offsets and disclosure groups' open or closed. A layer's,
 * kept by UiModule. advance() moves easing on once a frame; layout() reads it
 * (and notes how far each scroll view can go, and what's seen for the first
 * time); hits' scroll, press, drag and action change it. Nothing else does.
 */
struct State
{
  struct Scroll
  {
    glm::vec2 offset{0.0f};   // where it's shown scrolled to (pixels)
    glm::vec2 target{0.0f};   // where it's easing to
    glm::vec2 furthest{0.0f}; // how far it can go (content less view)
    float grab = -1.0f;       // where along the thumb it's held (<0: not held)
  };
  struct Disclosure
  {
    bool open = false;     // its own (not bound, not in an accordion)
    bool want = false;     // a bound one's: the game's, as last laid out
    bool bound = false;    // open as the game says (`want`)
    uint64_t group = 0;    // its accordion's (a hash; 0: none)
    float progress = 0.0f; // 0 closed to 1 open, eased between
  };

  /**
   * Easing moved on `seconds` (scroll views toward where they're going,
   * sections opening or closing): once a frame, before the layout.
   */
  void advance(float seconds);
  /** Whether a disclosure group's open (or opening) now. */
  bool opening(Id key, const Disclosure& disclosure) const;

  /**
   * Counted up whenever anything in it changes: scrolled (the wheel, the
   * bar), a section opened or closed, a first sight, or easing still under
   * way. Unchanged (and the body the same), a layer lays out as it did.
   */
  uint64_t changes = 0;
  std::unordered_map<Id, Scroll> scrolls;
  std::unordered_map<Id, Disclosure> disclosures;
  // Each accordion() group's (by its hash) open disclosure group (0: none).
  std::unordered_map<uint64_t, Id> openInGroup;
};

/**
 * How far a scroll view goes for `delta` (pixels; the wheel's, or a page)
 * from `from`, kept between 0 and `furthest`.
 */
glm::vec2 scrolled(glm::vec2 from, glm::vec2 delta, glm::vec2 furthest);

/**
 * A scroll bar's thumb along a track `track` long, for a view `view` long on
 * content `content` long scrolled `offset`: its start (from the track's) and
 * length, at least `shortest`.
 */
glm::vec2 scrollThumb(float track,
                      float view,
                      float content,
                      float offset,
                      float shortest);

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
  Id hovered = 0; // a Hit's id
  Id pressed = 0; // a button (or slider) held down
};

/**
 * Lay `root` out on a screen `size` pixels, `scale` pixels a point: placed by
 * its align() on the screen. Its quads go on `quads`, back to front, and what
 * the pointer can be over on `hits`, front last; every view's rect on
 * `placed` (if given), parents before their children. `state` keeps scroll
 * offsets and open sections from one frame to the next (none: each starts as
 * it's made, and nothing eases); advance it (State::advance()) before each.
 * Everything's laid out (for a layer kept from frame to frame, see Cache).
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
            std::vector<Placed>* placed = nullptr,
            State* state = nullptr);

/**
 * What a layer keeps from frame to frame, so it's laid out and drawn again
 * only where it's changed: its last quads and hits, and what each view in it
 * drew, by its node's hash (Node::hash()), its place and its rect. Any view
 * kind is kept this way, by the layout itself.
 *
 * A view's drawing is used again when the same node (by hash) is at the same
 * place, in the same rect, inherits the same font, colour and opacity, and is
 * cut to the same clip, and nothing it read has changed since: the layer's
 * State (if it read it), whether the pointer's over or holding one of its
 * hits (if it looked), and pictures it was still waiting for. A whole layer
 * the same as last frame isn't laid out at all.
 */
class Cache
{
public:
  Cache();
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  /** The last layout's quads, back to front (kept: shared, not copied). */
  const std::shared_ptr<const std::vector<UiQuad>>& quads() const;
  /** The last layout's hits, front last. */
  const std::vector<Hit>& hits() const;
  /** Forgets everything: the next layout lays it all out. */
  void clear();

  /** What the last layout() did (for tests and timing). */
  struct Stats
  {
    bool kept = false; // the whole layer as it was: nothing laid out
    int placed = 0;    // views placed (laid out and drawn) again
    int reused = 0;    // views whose drawing was used again
  };
  const Stats& stats() const;

  struct Data;
  Data& data() { return *m_data; }

private:
  std::unique_ptr<Data> m_data;
};

/**
 * layout(), kept from frame to frame in `cache`: only what's changed since
 * the last call is laid out and drawn again (see Cache), and its quads and
 * hits are the cache's (Cache::quads(), hits()). `state` must have been
 * advanced (State::advance()) first. With `placed`, everything's laid out
 * (where every view went is noted there).
 */
void layout(const View& root,
            glm::vec2 size,
            float scale,
            const Theme& theme,
            TextShaper& text,
            const Pointer& pointer,
            State& state,
            Cache& cache,
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
