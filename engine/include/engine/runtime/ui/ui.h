#pragma once

#include "engine/core/rendering/mesh.h"

#include <concepts>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A game's UI, described the way SwiftUI does: a tree of small values (text,
// buttons, stacks) changed by modifiers, rebuilt every frame from the game's
// state by a ui::Layer's body(). The engine lays it out, draws it over the
// scene, and calls buttons' actions when they're clicked.
//
//   return VStack{
//     Text("Day 3").font(Font::title()),
//     HStack{
//       Button("Till", [this] { till(); }),
//       Button("Sow", [this] { sow(); }).disabled(!canSow),
//     }.spacing(8),
//   }.padding(12).background(Panel()).align(Alignment::TopLeading);
//
// Sizes are in points: pixels at 1080 lines high, scaled with the window (see
// UiModule), so a UI looks the same on any screen.
namespace sfs::ui
{

/** A colour, its channels 0 to 1 as they look on screen (sRGB), and alpha. */
struct Color
{
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;

  /** From 0-255 channels, as a colour picker gives them. */
  static constexpr Color rgb(int r, int g, int b, float a = 1.0f)
  {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
  }
  /** The same colour, `alpha` as see-through. */
  constexpr Color opacity(float alpha) const { return {r, g, b, a * alpha}; }

  static constexpr Color white() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
  static constexpr Color black() { return {0.0f, 0.0f, 0.0f, 1.0f}; }
  static constexpr Color clear() { return {0.0f, 0.0f, 0.0f, 0.0f}; }

  bool operator==(const Color&) const = default;
};

/**
 * A typeface and size. `file` is a font asset ("asset:<id>"), a path in the
 * project's assets folder, or one of the engine's ("assets/fonts/m6x11.ttf");
 * empty: the layer's default (UiModule::setDefaultFont()).
 */
struct Font
{
  std::string file;
  float size = 20.0f;

  static Font body() { return {{}, 20.0f}; }
  static Font caption() { return {{}, 15.0f}; }
  static Font headline() { return {{}, 24.0f}; }
  static Font title() { return {{}, 32.0f}; }
  static Font named(std::string file, float size)
  {
    return {std::move(file), size};
  }
  /** The same typeface at another size. */
  Font sized(float points) const { return {file, points}; }

  bool operator==(const Font&) const = default;
};

/** Where something sits in the space it's given (or the screen). */
enum class Alignment : uint8_t
{
  TopLeading,
  Top,
  TopTrailing,
  Leading,
  Center,
  Trailing,
  BottomLeading,
  Bottom,
  BottomTrailing,
};

/** Space round the edges of something, in points. */
struct Insets
{
  float top = 0.0f;
  float leading = 0.0f;
  float bottom = 0.0f;
  float trailing = 0.0f;

  static Insets all(float points) { return {points, points, points, points}; }
  /** `vertical` above and below, `horizontal` either side. */
  static Insets symmetric(float vertical, float horizontal)
  {
    return {vertical, horizontal, vertical, horizontal};
  }
};

/** How a background is filled: colour, rounded corners, border and shadow. */
struct Fill
{
  Color color = Color::clear();
  float cornerRadius = 0.0f;
  Color borderColor = Color::clear();
  float borderWidth = 0.0f;
  float shadow = 0.0f; // how far a soft shadow spreads below it (0: none)
};

/** The look of a panel: a soft, rounded, see-through dark card. */
Fill Panel();

/** "As much as there is": a frame's width or height that takes all it can. */
inline constexpr float infinity = std::numeric_limits<float>::infinity();

struct Node;

/**
 * A piece of UI: a value, cheap to copy, changed by modifiers that each return
 * a new one. Build them with the functions and stacks below.
 */
class View;

/**
 * A view of the game's own: a struct with `View body() const`, made from its
 * fields (like SwiftUI's `struct Badge: View`). Keep them in a UI header
 * (Badge.ui.h), which the script compiler skips:
 *
 *   struct Badge
 *   {
 *     std::string text;
 *     sfs::ui::View body() const { return
 * Text(text).padding(6).background(...); }
 *   };
 *   HStack{ Badge{.text = "New"}, Text("Wheat") }
 */
template <typename T>
concept ViewStruct = requires(const T& view) {
  {
    view.body()
  } -> std::convertible_to<View>;
} && !std::derived_from<T, View>;

class View
{
public:
  View();
  explicit View(std::shared_ptr<const Node> node) : m_node(std::move(node)) {}
  /** A view struct (ViewStruct) as the view its body() makes. */
  template <ViewStruct T>
  View(const T& view) : View(view.body())
  {
  }

  // --- layout ---------------------------------------------------------------

  /** Space round it (12 points each side by default). */
  View padding(float points = 12.0f) const;
  View padding(Insets insets) const;
  /** A fixed width and/or height (0: as it would be). */
  View frame(float width, float height = 0.0f) const;
  /** As wide as it can be (infinity), or at most / at least so wide. */
  View maxWidth(float points) const;
  View minWidth(float points) const;
  View maxHeight(float points) const;
  View minHeight(float points) const;
  /** Where it sits in a ZStack (or on the screen, a layer's body). */
  View align(Alignment alignment) const;
  /** A stack's gap between its children. */
  View spacing(float points) const;

  // --- look -----------------------------------------------------------------

  /** Filled behind, the size of it (with its padding, if padded first). */
  View background(Color color) const;
  View background(Fill fill) const;
  /** Its background's corners rounded (the last background added). */
  View cornerRadius(float points) const;
  /** A line round its background. */
  View border(Color color, float width = 1.0f) const;
  /** Text and icons inside it. */
  View color(Color color) const;
  View font(Font font) const;
  /** How see-through it, and everything in it, is (1: solid). */
  View opacity(float alpha) const;

  // --- behaviour ------------------------------------------------------------

  /** Buttons in it can't be pressed (and look it). */
  View disabled(bool disabled = true) const;
  /** Shown when hovered for a moment. */
  View help(std::string text) const;
  /**
   * What tells it apart from frame to frame, in place of where it is in the
   * tree: give one to a button in a list that changes, so it stays itself.
   */
  View id(std::string id) const;

  const std::shared_ptr<const Node>& node() const { return m_node; }

  /** Several views as one, in their stack's place (what ForEach makes). */
  static View group(std::vector<View> views);

private:
  std::shared_ptr<const Node> m_node;
};

/** A line of text (or several, split at '\n'). */
View Text(std::string text);

/** A button: `label`'s text (or any view), calling `action` when clicked. */
View Button(std::string label, std::function<void()> action);
View Button(View label, std::function<void()> action);

/**
 * A picture: an image file ("asset:<id>", or a path in the project's folder:
 * PNG, JPG...), at its own size in points (a pixel each), or a mesh drawn as
 * an icon (Image::mesh). Framed smaller, it shrinks to fit, keeping its shape:
 * Image::mesh("farm:wheat").frame(24, 24).
 */
class Image : public View
{
public:
  explicit Image(std::string file);
  /**
   * A mesh (by its name in the scene's MeshLibrary, e.g. "farm:wheat") as an
   * icon: from a three-quarter view, lit, on nothing; 32 points by default.
   */
  static View mesh(std::string name);
  /** A mesh by its handle (a scriptable object's MeshHandle field). */
  static View mesh(MeshHandle mesh);
  /**
   * A soft box where a picture will be (the UI Preview's stand-in for an
   * image it hasn't got): 32 points, like a mesh icon, in the text's colour.
   */
  static View placeholder();
};

/** A bar filled `fraction` (0 to 1) of the way, 120 points wide by default. */
View ProgressBar(float fraction);

/** Takes up whatever room its stack has spare, pushing the rest apart. */
View Spacer(float minLength = 0.0f);

/** Nothing at all (e.g. for a branch with nothing to show). */
View Empty();

/**
 * Children one below another, lined up by `alignment` (Leading, Center or
 * Trailing): VStack{...}, or VStack(Alignment::Leading, {...}).
 */
class VStack : public View
{
public:
  VStack(std::initializer_list<View> children);
  VStack(Alignment alignment, std::initializer_list<View> children);
  VStack(std::vector<View> children, Alignment alignment = Alignment::Center);
};

/**
 * Children side by side, lined up by `alignment` (Top, Center or Bottom):
 * HStack{...}, or HStack(Alignment::Top, {...}).
 */
class HStack : public View
{
public:
  HStack(std::initializer_list<View> children);
  HStack(Alignment alignment, std::initializer_list<View> children);
  HStack(std::vector<View> children, Alignment alignment = Alignment::Center);
};

/** Children on top of one another, each placed by its align(); it fills the
 * space it's given. */
class ZStack : public View
{
public:
  ZStack(std::initializer_list<View> children);
  explicit ZStack(std::vector<View> children);
};

/** A stack's children made from a list: `make(item)` for each. */
template <typename Range, typename Make>
View ForEach(const Range& items, Make&& make)
{
  std::vector<View> views;
  for (const auto& item : items)
    views.push_back(make(item));
  return View::group(std::move(views));
}

// --- the tree (for the layout and its tests) ---------------------------------

/** What a View is: one node of the tree, with its children. */
struct Node
{
  enum class Kind : uint8_t
  {
    Empty,
    Text,
    Image,
    Button,
    Progress,
    Spacer,
    VStack,
    HStack,
    ZStack,
    Group, // ForEach's: its children go in its parent's place
    Padding,
    Frame,
    Background,
  };

  Kind kind = Kind::Empty;
  std::vector<View> children;

  std::string text;             // Text's; Image's file or mesh
  bool meshIcon = false;        // Image's: `text` is a mesh's name
  bool placeholder = false;     // Image's: a stand-in box (Image::placeholder)
  std::function<void()> action; // Button's
  float value = 0.0f;           // ProgressBar's fraction, Spacer's minimum

  float spacing = 8.0f;                         // stacks'
  Alignment stackAlignment = Alignment::Center; // stacks' line-up
  Insets insets;                                // Padding's
  float width = 0.0f, height = 0.0f;            // Frame's, 0: unset
  float minWidth = 0.0f, maxWidth = 0.0f;       // Frame's, 0: unset
  float minHeight = 0.0f, maxHeight = 0.0f;
  Fill fill; // Background's

  // Inherited by everything inside, unless set again there.
  std::optional<Font> font;
  std::optional<Color> color;
  float opacity = 1.0f;
  bool disabled = false;

  Alignment alignment = Alignment::Center; // within a ZStack, or the screen
  std::string id;                          // see View::id()
  std::string help;
};

} // namespace sfs::ui
