#pragma once

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/renderTexture.h"
#include "engine/runtime/ui/picture.h"

#include <concepts>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// A game's UI, described the way SwiftUI does: a tree of small values (text,
// buttons, stacks) changed by modifiers, made from the game's state by a
// ui::Layer's body(). The engine lays it out, draws it over the scene, and
// calls buttons' actions when they're clicked. It's retained: what a layer
// drew is kept, and only what's changed since (by the tree's hash) is laid
// out and drawn again (ui::Cache).
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

/** How a picture's pixels are drawn bigger or smaller than they are. */
enum class Interpolation : uint8_t
{
  Linear,  // blended between (smooth: photos, icons)
  Nearest, // each pixel a sharp square (a yield map, pixel art)
};

struct Node;
class ButtonStyle;

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
  View align(Alignment alignment) const&;
  View align(Alignment alignment) &&;
  /** A stack's gap between its children. */
  View spacing(float points) const&;
  View spacing(float points) &&;

  // --- look -----------------------------------------------------------------

  /** Filled behind, the size of it (with its padding, if padded first). */
  View background(Color color) const;
  View background(Fill fill) const;
  /** Its background's corners rounded (the last background added). */
  View cornerRadius(float points) const;
  /** A line round its background. */
  View border(Color color, float width = 1.0f) const;
  /** Text and icons inside it. */
  View color(Color color) const&;
  View color(Color color) &&;
  View font(Font font) const&;
  View font(Font font) &&;
  /** How see-through it, and everything in it, is (1: solid). */
  View opacity(float alpha) const&;
  View opacity(float alpha) &&;
  /**
   * Images in it drawn as masks (their alpha, in one colour): monochrome
   * icons, a lamp's tell-tale lit or unlit. In `color`, or with none, in the
   * colour they inherit (color()).
   */
  View tint(Color color) const&;
  View tint(Color color) &&;
  View tint() const&;
  View tint() &&;
  /** Images in it drawn smooth (Linear, the default) or sharp (Nearest). */
  View interpolation(Interpolation interpolation) const&;
  View interpolation(Interpolation interpolation) &&;
  /**
   * An Image that grows to fill what it's offered too (keeping its shape),
   * not only shrinks: Image("map.png").resizable().frame(300, 200). On an
   * Image only.
   */
  View resizable(bool resizable = true) const&;
  View resizable(bool resizable = true) &&;

  // --- behaviour ------------------------------------------------------------

  /**
   * How a button looks (see ButtonStyle): on a Button, its own; on a view
   * round buttons, every button in it without a style of its own.
   */
  View buttonStyle(const ButtonStyle& style) const;

  /** Buttons in it can't be pressed (and look it). */
  View disabled(bool disabled = true) const&;
  View disabled(bool disabled = true) &&;
  /** Shown when hovered for a moment. */
  View help(std::string text) const&;
  View help(std::string text) &&;
  /**
   * What tells it apart from frame to frame, in place of where it is in the
   * tree: give one to a button in a list that changes, so it stays itself.
   */
  View id(std::string id) const&;
  View id(std::string id) &&;

  // --- scroll views and disclosure groups -----------------------------------

  /**
   * Where a ScrollView's scrolled to the first frame it's shown: points down
   * (along, if it only scrolls sideways); after that, where it's scrolled.
   */
  View scrollOffset(float points) const&;
  View scrollOffset(float points) &&;
  /** A DisclosureGroup's header: `view` at its far end (e.g. a button). */
  View accessory(View view) const&;
  View accessory(View view) &&;
  /**
   * A DisclosureGroup in an accordion: opening it closes the others in the
   * layer with the same `group` (not ones bound to the game's own state).
   */
  View accordion(std::string group) const&;
  View accordion(std::string group) &&;

  const std::shared_ptr<const Node>& node() const { return m_node; }

  /** Several views as one, in their stack's place (what ForEach makes). */
  static View group(std::vector<View> views);

private:
  // Its node to change: itself while nothing else holds it (a view being
  // built: Text(...).font(...) changes one node in place), else a copy.
  std::shared_ptr<Node> owned() &&;

  std::shared_ptr<const Node> m_node;
};

/** A line of text (or several, split at '\n'). */
View Text(std::string text);

/** A button: `label`'s text (or any view), calling `action` when clicked. */
View Button(std::string label, std::function<void()> action);
View Button(View label, std::function<void()> action);

/** What a button style makes a button's look from. */
struct ButtonConfiguration
{
  View label;            // what the button was given (its text, or view)
  bool hovered = false;  // the pointer's over it
  bool pressed = false;  // held down on it (the pointer over it)
  bool disabled = false; // .disabled(), on it or round it
};

/**
 * How a button looks, like SwiftUI's ButtonStyle: the theme's face
 * (automatic()), none (plain(): the label is the button), or the game's: a
 * function, or a struct with `View makeBody(const ButtonConfiguration&)
 * const`, making its look from its label and state:
 *
 *   struct KeyStyle
 *   {
 *     View makeBody(const ButtonConfiguration& c) const
 *     {
 *       return c.label.padding(8).background(
 *           Fill{.color = c.pressed ? dark : c.hovered ? light : normal});
 *     }
 *   };
 *   Button("RUN", run).buttonStyle(KeyStyle{})
 *
 * A style's called when it's given (buttonStyle()), once for each state
 * (normal, hovered, pressed, disabled), so what it makes is part of the tree
 * and kept like any view; the pointer coming over a button lays out only
 * that button again. The button is the size of its normal look: keep the
 * looks the same size.
 */
class ButtonStyle
{
public:
  enum class Kind : uint8_t
  {
    Automatic, // the theme's face
    Plain,     // no face
    Custom,    // the game's
  };

  /** The theme's: a soft, rounded face, lighter hovered, darker pressed. */
  static ButtonStyle automatic() { return ButtonStyle(Kind::Automatic); }
  /** No face: the label is the button, dimmed while it's pressed. */
  static ButtonStyle plain() { return ButtonStyle(Kind::Plain); }
  /** A face filled `normal`, `hovered` or `pressed`, round the label padded. */
  static ButtonStyle filled(Fill normal,
                            Fill hovered,
                            Fill pressed,
                            Insets padding = Insets::symmetric(7.0f, 14.0f));

  template <typename F>
    requires std::is_invocable_r_v<View, const F&, const ButtonConfiguration&>
  ButtonStyle(F makeBody) : m_kind(Kind::Custom), m_make(std::move(makeBody))
  {
  }
  template <typename S>
    requires(!std::same_as<S, ButtonStyle>) &&
                requires(const S& style, const ButtonConfiguration& c) {
                  {
                    style.makeBody(c)
                  } -> std::convertible_to<View>;
                }
  ButtonStyle(S style)
      : m_kind(Kind::Custom),
        m_make([style = std::move(style)](const ButtonConfiguration& c)
               { return View(style.makeBody(c)); })
  {
  }

  Kind kind() const { return m_kind; }
  /** Its look for `configuration` (a custom style's; others: the label). */
  View makeBody(const ButtonConfiguration& configuration) const;

private:
  explicit ButtonStyle(Kind kind) : m_kind(kind) {}

  Kind m_kind = Kind::Automatic;
  std::function<View(const ButtonConfiguration&)> m_make;
};

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
   * Pixels the game draws (a ui::Picture), at their size in points (a pixel
   * each): a change to them is shown without laying anything out again.
   */
  explicit Image(std::shared_ptr<const Picture> picture);
  /**
   * A picture the GPU draws (a camera feed: RenderSystem::cameraFeed()), at
   * its size in points: each new picture is shown without laying anything
   * out again.
   */
  explicit Image(std::shared_ptr<const RenderTexture> texture);
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

/**
 * A slider: a knob along a track, from `low` to `high`, at `value`. Dragging
 * the knob (or pressing anywhere on the track) calls `change` with the value
 * under the pointer, in steps of `step` (0: any). 160 points wide by default.
 */
View Slider(float value,
            std::function<void(float)> change,
            float low = 0.0f,
            float high = 1.0f,
            float step = 0.0f);

/** A switch, on or off, with `label` beside it: a click calls `change` with
 * the other. */
View Toggle(std::string label, bool on, std::function<void(bool)> change);

/** Which ways a ScrollView scrolls. */
enum class Axis : uint8_t
{
  Vertical = 1,
  Horizontal = 2,
  Both = 3,
};

/**
 * Its content, scrolled: as big as the content up to the room it's offered,
 * and when the content's bigger, it shows part of it and scrolls (the mouse
 * wheel or a touchpad over it, eased; a thin bar down its edge to drag, or
 * click above or below the bar's thumb to page). What's outside it isn't
 * drawn, laid out or clickable. Where it's scrolled to is kept frame to frame
 * by where it is in the tree (or its id()). In a stack short of room it
 * gives up its room first (a panel's fixed header, then a scroll view: it fits
 * the screen). Inside another scroll view going the same way, give it a
 * frame() or maxHeight() to scroll in. The wheel goes to the innermost
 * scroll view under the pointer that can still go that way.
 *
 *   ScrollView(VStack{...}).maxHeight(600).scrollOffset(0)
 */
View ScrollView(View content, Axis axes = Axis::Vertical);

/**
 * A section that opens and closes: a header row (a chevron, `title`, and its
 * accessory() at the far end) and `content` below it while it's open. A
 * click on the header opens or closes it, its height eased over a moment.
 * It starts open with `open`, and remembers frame to frame by where it is in
 * the tree (or its id()); accordion() closes the others in a group. Closed,
 * its content isn't laid out at all. It's as wide as it's given.
 *
 *   DisclosureGroup("WEATHER", VStack{...}, true).accessory(Button(...))
 */
View DisclosureGroup(std::string title, View content, bool open = false);
View DisclosureGroup(View label, View content, bool open = false);
/** One open while the game says so: `change` called with the other when its
 * header's clicked. */
View DisclosureGroup(std::string title,
                     bool open,
                     std::function<void(bool)> change,
                     View content);

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
    Slider,
    Toggle,
    Spacer,
    VStack,
    HStack,
    ZStack,
    Group, // ForEach's: its children go in its parent's place
    Padding,
    Frame,
    Background,
    Scroll,     // ScrollView: children[0] the content
    Disclosure, // DisclosureGroup: children label, content, accessory
  };

  Kind kind = Kind::Empty;
  std::vector<View> children;

  std::string text;         // Text's; Image's file or mesh
  bool meshIcon = false;    // Image's: `text` is a mesh's name
  bool placeholder = false; // Image's: a stand-in box (Image::placeholder)
  bool resizable = false;   // Image's: grows to fit, too
  std::shared_ptr<const Picture> picture;  // Image's: the game's pixels...
  int pictureWidth = 0, pictureHeight = 0; // ...and their size, when made
  std::shared_ptr<const RenderTexture> rendered; // Image's: or the GPU's
  // A Button's look (ButtonStyle::Kind). A custom one's children: its label,
  // then its looks normal, hovered, pressed and disabled.
  ButtonStyle::Kind buttonStyle = ButtonStyle::Kind::Automatic;
  std::function<void()> action; // Button's
  float value = 0.0f; // ProgressBar's fraction, Spacer's minimum, Slider's
                      // value, Toggle's 1 (on) or 0, DisclosureGroup's
                      // 1 (open, or open at first) or 0
  float low = 0.0f, high = 1.0f, step = 0.0f; // Slider's range and steps
  std::function<void(float)> change; // Slider's (its new value), Toggle's,
                                     // a bound DisclosureGroup's

  float spacing = 8.0f;                         // stacks'
  Alignment stackAlignment = Alignment::Center; // stacks' line-up
  Insets insets;                                // Padding's
  float width = 0.0f, height = 0.0f;            // Frame's, 0: unset
  float minWidth = 0.0f, maxWidth = 0.0f;       // Frame's, 0: unset
  float minHeight = 0.0f, maxHeight = 0.0f;
  Fill fill; // Background's

  Axis axes = Axis::Vertical; // ScrollView's
  float scrollOffset = -1.0f; // ScrollView's first offset, points (<0: 0)
  bool bound = false;         // DisclosureGroup's open is `value`, the game's
  std::string group;          // DisclosureGroup's accordion

  // Inherited by everything inside, unless set again there.
  std::optional<Font> font;
  std::optional<Color> color;
  float opacity = 1.0f;
  bool disabled = false;
  bool mask = false; // images drawn as masks in the colour (tint())
  std::optional<Interpolation> interpolation;

  Alignment alignment = Alignment::Center; // within a ZStack, or the screen
  std::string id;                          // see View::id()
  std::string help;

  /**
   * What it is, as a number: a hash of every field but the functions (only
   * whether each is set) and of its children's. Trees that draw the same have
   * the same hash, so a layer whose tree hashes as last frame's isn't laid
   * out again (ui::Cache). Worked out once, when first asked (a node doesn't
   * change once it's in a View).
   */
  uint64_t hash() const;
  /** How many nodes there are in it, itself too (a hit's Hit::source). */
  uint32_t count() const;
  /** Whether it, or anything in it, keeps State (a scroll view, a section). */
  bool keepsState() const;

  // Worked out by hash() (0: not yet), and the layout's notes on it.
  mutable uint64_t m_hash = 0;
  mutable uint32_t m_count = 0;
  mutable bool m_keepsState = false;
  mutable uint8_t m_layoutBits = 0;
};

} // namespace sfs::ui
