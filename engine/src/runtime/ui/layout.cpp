#include "engine/runtime/ui/layout.h"

#include "glm/glm/common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace sfs::ui
{

namespace
{

using Kind = Node::Kind;

// A toggle's switch, in points.
constexpr glm::vec2 kSwitch{36.0f, 20.0f};
// A disclosure group's chevron, and the gap after it, in points.
constexpr float kChevron = 10.0f;
constexpr float kChevronGap = 8.0f;
// How long a disclosure group takes to open or close, in seconds.
constexpr float kDisclosureTime = 0.2f;
// How quickly a scroll view eases to where it's scrolled to (per second).
constexpr float kScrollEase = 16.0f;
// How many kinds of view there are.
constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Disclosure) + 1;

// Mixing values into a hash (splitmix's finaliser, then combined).
uint64_t mixed(uint64_t hash, uint64_t value)
{
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  value ^= value >> 31;
  return hash ^ (value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2));
}
uint64_t mixed(uint64_t hash, float value)
{
  return mixed(hash, static_cast<uint64_t>(std::bit_cast<uint32_t>(value)));
}
uint64_t mixed(uint64_t hash, std::string_view text)
{
  return mixed(
      hash, static_cast<uint64_t>(std::hash<std::string_view>()(text)));
}
uint64_t mixed(uint64_t hash, Color color)
{
  return mixed(mixed(mixed(mixed(hash, color.r), color.g), color.b), color.a);
}
uint64_t mixed(uint64_t hash, Rect rect)
{
  return mixed(
      mixed(mixed(mixed(hash, rect.x), rect.y), rect.width), rect.height);
}

// Ids: the body's, and a step down from a view to one inside it.
constexpr Id kRoot = 0x5F5u;
Id step(Id path, char tag) { return mixed(path, static_cast<uint64_t>(tag)); }
// A stack's child: by its id(), or its kind and how many of that kind came
// before it in the stack (so inserting a Text doesn't move a scroll view's
// state).
Id childPath(Id path, const Node& child, uint32_t sameKind)
{
  if (!child.id.empty())
    return mixed(mixed(path, uint64_t{'#'}), std::string_view(child.id));
  return mixed(mixed(path, static_cast<uint64_t>(child.kind) + 0x100u),
               static_cast<uint64_t>(sameKind));
}
// A scroll view's or disclosure group's key in the State.
Id stateKey(const Node& node, Id path)
{
  return node.id.empty() ? path : idKey(node.id);
}

bool scrollsAlong(const Node& node, int axis)
{
  const auto axes = static_cast<uint8_t>(node.axes);
  return (axes & (axis == 0 ? static_cast<uint8_t>(Axis::Horizontal)
                            : static_cast<uint8_t>(Axis::Vertical))) != 0;
}

float smooth(float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Everywhere: what's laid out without a scroll view round it is cut to.
constexpr Rect kEverywhere{-1.0e9f, -1.0e9f, 2.0e9f, 2.0e9f};

bool inside(const Rect& rect, const Rect& clip)
{
  return rect.x >= clip.x && rect.y >= clip.y &&
         rect.x + rect.width <= clip.x + clip.width &&
         rect.y + rect.height <= clip.y + clip.height;
}

glm::vec4 toVec(Color color, float opacity)
{
  return {color.r, color.g, color.b, color.a * opacity};
}

uint64_t themeHash(const Theme& theme)
{
  uint64_t hash = mixed(uint64_t{0x7E}, std::string_view(theme.font));
  for (const Color& color : {theme.text,
                             theme.button,
                             theme.buttonHovered,
                             theme.buttonPressed,
                             theme.buttonText,
                             theme.accent,
                             theme.track,
                             theme.scrollThumb,
                             theme.scrollThumbHovered})
    hash = mixed(hash, color);
  return mixed(hash, theme.buttonRadius);
}

// What a node inherits from those around it.
struct Env
{
  std::string font;
  std::size_t fontKey = 0; // the font's hash (for remembering sizes)
  float fontSize = 20.0f;  // points
  Color color;
  float opacity = 1.0f;
  bool disabled = false;
  bool mask = false;    // images as masks in `color` (tint())
  bool nearest = false; // images' pixels sharp (Interpolation::Nearest)
  uint64_t hash = 0;    // all of it (rehash())

  void rehash()
  {
    hash = mixed(
        mixed(mixed(mixed(mixed(uint64_t{fontKey}, fontSize), color), opacity),
              static_cast<uint64_t>(disabled) |
                  static_cast<uint64_t>(mask) << 1 |
                  static_cast<uint64_t>(nearest) << 2),
        uint64_t{0xE4});
  }
};

// A pointer-sensitive id a view's look read, and what it read: over it (1),
// holding it (2).
struct Sense
{
  Id id = 0;
  uint8_t bits = 0;
};

// A view's drawing kept: where in its frame's quads, hits and senses it is,
// the node's place in the tree then (to move its hits' sources by), and
// where the views inside it that were kept start in its frame's `recorded`.
struct Entry
{
  uint32_t quad = 0, quads = 0;
  uint32_t hit = 0, hits = 0;
  uint32_t sense = 0, senses = 0;
  uint32_t source = 0;
  uint32_t inside = 0;
};

struct Measure
{
  glm::vec2 size{0.0f};
};

} // namespace

// One frame's layout of a layer, kept: what it drew, and each view's part.
struct Generation
{
  std::shared_ptr<std::vector<UiQuad>> quads =
      std::make_shared<std::vector<UiQuad>>();
  std::vector<Hit> hits;
  std::vector<Sense> senses;
  std::unordered_map<uint64_t, Entry> entries; // by place key
  std::vector<uint64_t> recorded; // entries' keys, those inside a view first
  std::unordered_map<uint64_t, Measure> measures; // by measure key
  uint64_t stamp = 0;                             // State::changes, laid out
  bool readState = false;                         // the layer read the State
  bool waiting = false; // something in it waits on a picture
};

struct Cache::Data
{
  Generation frames[2];
  int current = 0;
  uint64_t layerKey = 0;  // the body's hash and global, last laid out (0: none)
  uint64_t globalKey = 0; // the theme, scale, screen and text generation
  std::vector<const UiTexture*> textures; // glyph textures its quads use
  std::shared_ptr<const std::vector<UiQuad>> shown =
      std::make_shared<const std::vector<UiQuad>>();
  Stats stats;
};

namespace
{

class Layout
{
public:
  Layout(float scale,
         const Theme& theme,
         TextShaper& text,
         const Pointer& pointer,
         std::vector<UiQuad>& quads,
         std::vector<Hit>& hits,
         ImageSource* images,
         State* state = nullptr)
      : m_scale(scale), m_theme(theme), m_text(text), m_pointer(pointer),
        m_quads(quads), m_hits(hits), m_images(images), m_state(state),
        m_measures(&m_ownMeasures)
  {
  }

  // Kept from frame to frame: last frame's layout (`last`) to use again
  // what's unchanged from, and this one's (`next`) to note it all in.
  void keep(Generation& last, Generation& next)
  {
    m_last = &last;
    m_next = &next;
    m_measures = &next.measures;
  }

  // Each scroll view's and disclosure group's key in the state (as place()
  // makes them), for measuring (which doesn't know where a view is).
  void findKeys(const Node& node, Id path)
  {
    if (!node.keepsState())
      return;
    if (node.kind == Kind::Scroll || node.kind == Kind::Disclosure)
      m_keys.emplace(&node, stateKey(node, path));
    switch (node.kind)
    {
    case Kind::Group:
    case Kind::VStack:
    case Kind::HStack:
    case Kind::ZStack:
    {
      uint32_t sameKind[kKinds] = {};
      forEachChild(
          node,
          0,
          [&](const Node& child, uint32_t)
          {
            findKeys(
                child,
                childPath(path,
                          child,
                          sameKind[static_cast<std::size_t>(child.kind)]++));
          });
      break;
    }
    case Kind::Button:
    case Kind::Toggle:
      // (A styled button's looks are all in the label's place.)
      for (const View& child : node.children)
        findKeys(*child.node(), step(path, 'l'));
      break;
    case Kind::Padding:
    case Kind::Background:
    case Kind::Frame:
    case Kind::Scroll:
      if (!node.children.empty())
        findKeys(*node.children[0].node(),
                 step(path,
                      node.kind == Kind::Padding      ? 'p'
                      : node.kind == Kind::Background ? 'b'
                      : node.kind == Kind::Frame      ? 'f'
                                                      : 's'));
      break;
    case Kind::Disclosure:
      if (node.children.size() == 3)
      {
        findKeys(*node.children[0].node(), step(path, 't'));
        findKeys(*node.children[1].node(), step(path, 'c'));
        findKeys(*node.children[2].node(), step(path, 'a'));
      }
      break;
    default:
      break;
    }
  }

  // A body's size on a screen `size` big: its own, or the screen's along a
  // way it stretches.
  glm::vec2 fitted(const Node& node, glm::vec2 size, const Env& env)
  {
    glm::vec2 natural = measure(node, size, env);
    if (flexible(node, true))
      natural.x = size.x;
    if (flexible(node, false))
      natural.y = size.y;
    return glm::min(natural, size);
  }

  Env rootEnv() const
  {
    Env env;
    env.font = m_theme.font;
    env.fontKey = std::hash<std::string>()(env.font);
    env.color = m_theme.text;
    env.rehash();
    return env;
  }

  // A node's surroundings, with what it sets itself.
  static Env envOf(const Node& node, const Env& outer)
  {
    Env env = outer;
    if (!node.font && !node.color && node.opacity == 1.0f && !node.disabled &&
        !node.mask && !node.interpolation)
      return env;
    if (node.font)
    {
      if (!node.font->file.empty() && node.font->file != env.font)
      {
        env.font = node.font->file;
        env.fontKey = std::hash<std::string>()(env.font);
      }
      env.fontSize = node.font->size;
    }
    if (node.color)
      env.color = *node.color;
    env.opacity *= node.opacity;
    env.disabled = env.disabled || node.disabled;
    env.mask = env.mask || node.mask;
    if (node.interpolation)
      env.nearest = *node.interpolation == Interpolation::Nearest;
    env.rehash();
    return env;
  }

  // A stack's children, with ForEach's groups opened out and nothing left
  // out: `visit(child, its place in the tree)`, `index` the stack's.
  template <typename Visit>
  static void forEachChild(const Node& node, uint32_t index, Visit&& visit)
  {
    uint32_t at = index + 1;
    for (const View& each : node.children)
    {
      const Node& child = *each.node();
      if (child.kind == Kind::Group)
        forEachChild(child, at, visit);
      else if (child.kind != Kind::Empty)
        visit(child, at);
      at += child.count();
    }
  }

  // Whether it takes all the room it's given, across (horizontal) or down:
  // worked out once a node (it's the same whatever it's laid out in).
  static bool flexible(const Node& node, bool horizontal)
  {
    const uint8_t known = horizontal ? 1 : 4, yes = horizontal ? 2 : 8;
    if ((node.m_layoutBits & known) == 0)
      node.m_layoutBits = static_cast<uint8_t>(
          node.m_layoutBits | known | (isFlexible(node, horizontal) ? yes : 0));
    return (node.m_layoutBits & yes) != 0;
  }

  static bool isFlexible(const Node& node, bool horizontal)
  {
    switch (node.kind)
    {
    case Kind::Spacer:
    case Kind::ZStack:
      return true;
    case Kind::Disclosure:
      return horizontal; // as wide as it's given
    case Kind::Frame:
    {
      const float max = horizontal ? node.maxWidth : node.maxHeight;
      const float fixed = horizontal ? node.width : node.height;
      if (std::isinf(max))
        return true;
      return fixed <= 0.0f && max <= 0.0f && !node.children.empty() &&
             flexible(*node.children[0].node(), horizontal);
    }
    case Kind::Padding:
    case Kind::Background:
      return !node.children.empty() &&
             flexible(*node.children[0].node(), horizontal);
    case Kind::VStack:
    case Kind::HStack:
    case Kind::Group:
    {
      // A spacer stretches only along its own stack.
      const bool along = (node.kind == Kind::HStack) == horizontal;
      bool any = false;
      forEachChild(node,
                   0,
                   [&](const Node& child, uint32_t)
                   {
                     any = any || ((along || child.kind != Kind::Spacer) &&
                                   flexible(child, horizontal));
                   });
      return any;
    }
    default:
      return false;
    }
  }

  // Whether it shrinks in a stack that's short of room: a scroll view going
  // along it.
  static bool givesWay(const Node& node, bool horizontal)
  {
    return node.kind == Kind::Scroll && scrollsAlong(node, horizontal ? 0 : 1);
  }

  // --- measuring
  // --------------------------------------------------------------

  // A node's size in an offer: measured over and over (by each parent placing
  // it, then by it placing its children), and the same each time, so kept:
  // by its hash (from frame to frame, if the layout's kept), or for one with
  // State in it (a section's height eases), by the node, this layout only.
  glm::vec2 measure(const Node& node, glm::vec2 offer, const Env& outer)
  {
    if (node.kind == Kind::Empty)
      return {0.0f, 0.0f};
    const bool stateful = node.keepsState();
    const uint64_t key =
        mixed(mixed(mixed(mixed(stateful ? reinterpret_cast<uintptr_t>(&node)
                                         : node.hash(),
                                offer.x),
                          offer.y),
                    outer.fontSize),
              uint64_t{outer.fontKey});
    std::unordered_map<uint64_t, Measure>& measures =
        stateful ? m_ownMeasures : *m_measures;
    if (const auto found = measures.find(key); found != measures.end())
      return found->second.size;
    if (m_last && !stateful)
      if (const auto found = m_last->measures.find(key);
          found != m_last->measures.end())
      {
        measures.emplace(key, found->second);
        return found->second.size;
      }
    const bool waiting = m_waiting;
    m_waiting = false;
    const glm::vec2 size = measureNode(node, offer, outer);
    if (!m_waiting)
      measures.emplace(key, Measure{size});
    m_waiting = m_waiting || waiting;
    return size;
  }

  glm::vec2 measureNode(const Node& node, glm::vec2 offer, const Env& outer)
  {
    const Env env = envOf(node, outer);
    const float s = m_scale;
    switch (node.kind)
    {
    case Kind::Empty:
      return {0.0f, 0.0f};
    case Kind::Group:
      return measureStack(node, offer, env, false);
    case Kind::Text:
      return textSize(node.text, env);
    case Kind::Image:
      return imageSize(node, offer);
    case Kind::Button:
    {
      // A plain one's its label; a custom one its normal look.
      if (node.buttonStyle == ButtonStyle::Kind::Plain &&
          !node.children.empty())
        return measure(*node.children[0].node(), offer, env);
      if (node.buttonStyle == ButtonStyle::Kind::Custom &&
          node.children.size() == 5)
        return measure(*node.children[1].node(), offer, env);
      const glm::vec2 pad = buttonPadding();
      const glm::vec2 label = node.children.empty()
                                  ? glm::vec2(0.0f)
                                  : measure(*node.children[0].node(),
                                            offer - 2.0f * pad,
                                            labelEnv(env));
      return {
          label.x + 2.0f * pad.x, std::max(label.y + 2.0f * pad.y, 34.0f * s)};
    }
    case Kind::Progress:
      return {std::min(120.0f * s, offer.x), 10.0f * s};
    case Kind::Slider:
      return {std::min(160.0f * s, offer.x), 22.0f * s};
    case Kind::Toggle:
    {
      // The switch, a gap, and its label.
      const float lead = (kSwitch.x + 8.0f) * s;
      const glm::vec2 label =
          node.children.empty()
              ? glm::vec2(0.0f)
              : measure(
                    *node.children[0].node(), {offer.x - lead, offer.y}, env);
      return {lead + label.x, std::max(label.y, 24.0f * s)};
    }
    case Kind::Spacer:
      return glm::vec2(node.value * s);
    case Kind::VStack:
      return measureStack(node, offer, env, false);
    case Kind::HStack:
      return measureStack(node, offer, env, true);
    case Kind::ZStack:
    {
      glm::vec2 most(0.0f);
      forEachChild(node,
                   0,
                   [&](const Node& child, uint32_t)
                   { most = glm::max(most, measure(child, offer, env)); });
      return {std::isfinite(offer.x) ? offer.x : most.x,
              std::isfinite(offer.y) ? offer.y : most.y};
    }
    case Kind::Padding:
    {
      const glm::vec2 inset((node.insets.leading + node.insets.trailing) * s,
                            (node.insets.top + node.insets.bottom) * s);
      const glm::vec2 inner =
          node.children.empty()
              ? glm::vec2(0.0f)
              : measure(*node.children[0].node(), offer - inset, env);
      return inner + inset;
    }
    case Kind::Background:
      return node.children.empty()
                 ? glm::vec2(0.0f)
                 : measure(*node.children[0].node(), offer, env);
    case Kind::Frame:
      return frameSize(node, offer, env);
    case Kind::Scroll:
    {
      // The content's, up to the room it's offered.
      if (node.children.empty())
        return {0.0f, 0.0f};
      const glm::vec2 content =
          measure(*node.children[0].node(), contentOffer(node, offer), env);
      return glm::min(content, offer);
    }
    case Kind::Disclosure:
      return disclosureSize(node, offer, env);
    }
    return {0.0f, 0.0f};
  }

  // What a scroll view offers its content: as much as it likes along the
  // ways it scrolls.
  static glm::vec2 contentOffer(const Node& node, glm::vec2 offer)
  {
    for (int axis = 0; axis < 2; ++axis)
      if (scrollsAlong(node, axis))
        offer[axis] = infinity;
    return offer;
  }

  // --- disclosure groups
  // -------------------------------------------------------

  struct Openness
  {
    bool open = false;
    float shown = 0.0f; // how much of its content shows, 0 to 1 (eased)
    Id key = 0;
    State::Disclosure* state = nullptr;
  };

  // Whether a disclosure group's open, and how far it's opened (moved on by
  // State::advance()).
  Openness openness(const Node& node, Id key)
  {
    Openness out;
    const bool given = node.value > 0.5f;
    if (!m_state)
    {
      out.open = given;
      out.shown = given ? 1.0f : 0.0f;
      return out;
    }
    out.key = key;
    auto [entry, fresh] = m_state->disclosures.try_emplace(key);
    State::Disclosure& state = entry->second;
    out.state = &state;
    const uint64_t group =
        node.group.empty() ? 0 : mixed(uint64_t{0x67}, node.group);
    state.bound = node.bound;
    state.group = group;
    if (node.bound && state.want != given)
    {
      state.want = given;
      ++m_state->changes; // eases there from the next frame
    }
    if (fresh)
    {
      ++m_state->changes;
      state.open = given;
      state.want = given;
      if (group != 0 && given && !node.bound)
        m_state->openInGroup.try_emplace(group, key);
    }
    out.open = m_state->opening(key, state);
    if (fresh)
      state.progress = out.open ? 1.0f : 0.0f;
    out.shown = smooth(state.progress);
    return out;
  }

  Openness openness(const Node& node)
  {
    const auto found = m_keys.find(&node);
    return openness(node, found != m_keys.end() ? found->second : 0);
  }

  // A disclosure group's header: its label's and accessory's sizes, and its
  // own.
  struct Header
  {
    glm::vec2 label{0.0f};
    glm::vec2 accessory{0.0f};
    glm::vec2 size{0.0f};
  };

  Header headerOf(const Node& node, glm::vec2 offer, const Env& env)
  {
    const float s = m_scale;
    Header header;
    if (node.children.size() < 3)
      return header;
    const float lead = (kChevron + kChevronGap) * s;
    header.accessory = measure(*node.children[2].node(), offer, env);
    const float gap = header.accessory.x > 0.0f ? 8.0f * s : 0.0f;
    header.label = measure(*node.children[0].node(),
                           {offer.x - lead - gap - header.accessory.x, offer.y},
                           env);
    header.size = {
        lead + header.label.x + gap + header.accessory.x,
        std::max({header.label.y, header.accessory.y, kChevron * s}) +
            6.0f * s};
    return header;
  }

  glm::vec2 disclosureSize(const Node& node, glm::vec2 offer, const Env& env)
  {
    const Header header = headerOf(node, offer, env);
    glm::vec2 size = header.size;
    const Openness open = openness(node);
    if (open.shown > 0.0f && node.children.size() == 3)
    {
      const float gap = node.spacing * m_scale;
      // (Its natural height, which it eases to.)
      const glm::vec2 content =
          measure(*node.children[1].node(), {offer.x, infinity}, env);
      size.x = std::max(size.x, content.x);
      size.y += (gap + content.y) * open.shown;
    }
    return size;
  }

  glm::vec2 frameSize(const Node& node, glm::vec2 offer, const Env& env)
  {
    const float s = m_scale;
    // What the child's offered: the fixed size, or the room, kept within the
    // frame's bounds.
    const auto bound = [&](float room, float fixed, float min, float max)
    {
      if (fixed > 0.0f)
        return fixed * s;
      float value = room;
      if (max > 0.0f && std::isfinite(max))
        value = std::min(value, max * s);
      if (min > 0.0f)
        value = std::max(value, min * s);
      return value;
    };
    const glm::vec2 inner(
        bound(offer.x, node.width, node.minWidth, node.maxWidth),
        bound(offer.y, node.height, node.minHeight, node.maxHeight));
    const glm::vec2 child = node.children.empty()
                                ? glm::vec2(0.0f)
                                : measure(*node.children[0].node(), inner, env);
    // Its own size: fixed, all the room (infinite max), or the child's, kept
    // within its bounds.
    const auto size =
        [&](float room, float childSize, float fixed, float min, float max)
    {
      if (fixed > 0.0f)
        return fixed * s;
      float value = std::isinf(max) && std::isfinite(room) ? room : childSize;
      if (max > 0.0f && std::isfinite(max))
        value = std::min(value, max * s);
      if (min > 0.0f)
        value = std::max(value, min * s);
      return value;
    };
    return {
        size(offer.x, child.x, node.width, node.minWidth, node.maxWidth),
        size(offer.y, child.y, node.height, node.minHeight, node.maxHeight)};
  }

  glm::vec2 measureStack(const Node& node,
                         glm::vec2 offer,
                         const Env& env,
                         bool horizontal)
  {
    const int along = horizontal ? 0 : 1, across = 1 - along;
    float total = 0.0f, widest = 0.0f, yielding = 0.0f;
    bool grows = false, fills = false;
    int count = 0;
    forEachChild(node,
                 0,
                 [&](const Node& child, uint32_t)
                 {
                   const glm::vec2 size = measure(child, offer, env);
                   total += size[along];
                   if (givesWay(child, horizontal))
                     yielding += size[along];
                   widest = std::max(widest, size[across]);
                   grows = grows || flexible(child, horizontal);
                   // (A spacer only stretches along the stack.)
                   fills = fills || (child.kind != Kind::Spacer &&
                                     flexible(child, !horizontal));
                   ++count;
                 });
    if (count > 0)
      total += node.spacing * m_scale * static_cast<float>(count - 1);
    // Short of room: its scroll views give theirs up first.
    if (std::isfinite(offer[along]) && total > offer[along])
      total -= std::min(total - offer[along], yielding);
    glm::vec2 size;
    size[along] = grows && std::isfinite(offer[along])
                      ? std::max(offer[along], total)
                      : total;
    size[across] =
        fills && std::isfinite(offer[across]) ? offer[across] : widest;
    return size;
  }

  // An image's picture; none yet (a mesh still loading): the views round it
  // aren't kept, so it's drawn when it comes. The game's own pixels (a
  // Picture) are a texture of their own, whole.
  const SourceImage* imageOf(const Node& node)
  {
    if (node.picture || node.rendered)
    {
      m_picture.texture =
          node.picture ? &node.picture->texture() : &node.rendered->texture();
      m_picture.levels = 1;
      m_picture.level[0] = {glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),
                            {node.pictureWidth, node.pictureHeight}};
      return &m_picture;
    }
    if (!m_images || node.placeholder)
      return nullptr;
    const SourceImage* image = m_images->image(node.text, node.meshIcon);
    if (!image)
      m_waiting = true;
    return image;
  }

  // An image's own size (a mesh's icon or a placeholder 32 points), shrunk
  // to fit what it's offered (grown too, if it's resizable), keeping its
  // shape.
  glm::vec2 imageSize(const Node& node, glm::vec2 offer)
  {
    const SourceImage* image = imageOf(node);
    glm::vec2 size = node.meshIcon || node.placeholder
                         ? glm::vec2(32.0f * m_scale)
                     : image ? glm::vec2(image->size()) * m_scale
                             : glm::vec2(0.0f);
    if (size.x <= 0.0f || size.y <= 0.0f)
      return glm::vec2(0.0f);
    const float across = std::isfinite(offer.x) ? offer.x / size.x : infinity;
    const float down = std::isfinite(offer.y) ? offer.y / size.y : infinity;
    float fit = std::min(across, down);
    if (!node.resizable || !std::isfinite(fit))
      fit = std::min(fit, 1.0f);
    return size * std::max(fit, 0.0f);
  }

  void drawImage(const Node& node, Rect rect, const Env& env)
  {
    if (node.placeholder)
    {
      // A square in the middle of its rect, faintly the text's colour.
      const float side = std::min(rect.width, rect.height);
      const Rect square{rect.x + (rect.width - side) * 0.5f,
                        rect.y + (rect.height - side) * 0.5f,
                        side,
                        side};
      Color faint = env.color;
      faint.a *= 0.3f;
      box(square, toVec(faint, env.opacity), side * 0.22f);
      return;
    }
    const SourceImage* image = imageOf(node);
    if (!image || !image->texture || image->size().x <= 0 ||
        image->size().y <= 0)
      return;
    // Its shape kept, in the middle of its rect, on whole pixels (so a small
    // icon drawn from a level its size is crisp), from the level nearest the
    // size it's drawn.
    const glm::vec2 own(image->size());
    const float fit = std::min(rect.width / own.x, rect.height / own.y);
    const glm::vec2 size = glm::max(glm::round(own * fit), glm::vec2(1.0f));
    UiQuad quad;
    quad.kind = env.mask ? UiQuad::Mask : UiQuad::Image;
    quad.nearest = env.nearest;
    quad.texture = image->texture;
    quad.rect = {std::round(rect.x + (rect.width - size.x) * 0.5f),
                 std::round(rect.y + (rect.height - size.y) * 0.5f),
                 size.x,
                 size.y};
    quad.uv = env.nearest ? image->level[0].uv : image->levelFor(size.x).uv;
    quad.color = env.mask ? toVec(env.color, env.opacity)
                          : glm::vec4(1.0f, 1.0f, 1.0f, env.opacity);
    m_quads.push_back(quad);
  }

  glm::vec2 textSize(const std::string& text, const Env& env)
  {
    const float pixels = env.fontSize * m_scale;
    const float line = m_text.lineHeight(env.font, pixels);
    float widest = 0.0f;
    int lines = 0;
    forEachLine(text,
                [&](std::string_view part)
                {
                  widest =
                      std::max(widest, m_text.width(env.font, pixels, part));
                  ++lines;
                });
    return {widest, line * static_cast<float>(std::max(lines, 1))};
  }

  template <typename Visit>
  static void forEachLine(std::string_view text, Visit&& visit)
  {
    std::size_t start = 0;
    while (true)
    {
      const std::size_t end = text.find('\n', start);
      visit(text.substr(
          start, end == std::string_view::npos ? end : end - start));
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
  }

  glm::vec2 buttonPadding() const { return glm::vec2(14.0f, 7.0f) * m_scale; }

  // A button's label: in the button's text colour, unless it sets its own.
  Env labelEnv(const Env& env) const
  {
    Env label = env;
    label.color = m_theme.buttonText;
    label.rehash();
    return label;
  }

  // --- placing
  // ----------------------------------------------------------------

  // Placed `rect`, at `path`, `index` its place in the tree (Hit::source).
  // Kept from frame to frame, a view inside what it's cut to is looked for
  // in last frame's layout first: the same node (by hash), place, rect,
  // surroundings and clip, and nothing it read since changed, its drawing is
  // used again as it was. Any kind of view, so long as its look reads the
  // pointer only through hovered() and pressed().
  void
  place(const Node& node, Rect rect, const Env& outer, Id path, uint32_t index)
  {
    // Scrolled out of sight: not drawn, laid out or clickable.
    if (!rect.overlaps(m_clip) && rect.width > 0.0f && rect.height > 0.0f)
      return;
    if (m_placed)
      m_placed->push_back({rect,
                           node.kind,
                           m_depth,
                           node.kind == Kind::Text || node.kind == Kind::Image
                               ? node.text
                               : std::string()});
    ++m_depth;
    if (m_next && inside(rect, m_clip))
      placeKept(node, rect, outer, path, index);
    else
    {
      ++m_stats.placed;
      placeNode(node, rect, outer, path, index);
    }
    --m_depth;
  }

  std::vector<Placed>* m_placed = nullptr;
  Cache::Stats m_stats;
  bool m_waiting = false; // a picture's still to come

private:
  SourceImage m_picture; // a Picture's, as imageOf() gives it
  int m_depth = 0;

  // Whether `entry`, last frame's, still draws as it did.
  bool usable(const Entry& entry, bool readState) const
  {
    if (readState && (!m_state || m_state->changes != m_last->stamp))
      return false;
    for (uint32_t i = entry.sense; i < entry.sense + entry.senses; ++i)
      if (senseOf(m_last->senses[i].id) != m_last->senses[i].bits)
        return false;
    return true;
  }

  void placeKept(const Node& node,
                 Rect rect,
                 const Env& outer,
                 Id path,
                 uint32_t index)
  {
    const uint64_t key =
        mixed(mixed(mixed(mixed(node.hash(), path), rect), outer.hash), m_clip);
    if (const auto found = m_last->entries.find(key);
        found != m_last->entries.end() &&
        usable(found->second, node.keepsState()))
    {
      reuse(found->second, key, index);
      return;
    }
    const auto quad = static_cast<uint32_t>(m_quads.size());
    const auto hit = static_cast<uint32_t>(m_hits.size());
    const auto sense = static_cast<uint32_t>(m_next->senses.size());
    const auto inside = static_cast<uint32_t>(m_next->recorded.size());
    const bool waiting = m_waiting;
    m_waiting = false;
    ++m_stats.placed;
    placeNode(node, rect, outer, path, index);
    if (!m_waiting)
    {
      m_next->entries[key] =
          Entry{quad,
                static_cast<uint32_t>(m_quads.size()) - quad,
                hit,
                static_cast<uint32_t>(m_hits.size()) - hit,
                sense,
                static_cast<uint32_t>(m_next->senses.size()) - sense,
                index,
                inside};
      m_next->recorded.push_back(key);
    }
    m_waiting = m_waiting || waiting;
  }

  // Last frame's drawing of a view used again: its quads, hits (their
  // sources moved to where it is in the tree now) and what it read of the
  // pointer, noted as this frame's.
  void reuse(const Entry& entry, uint64_t key, uint32_t index)
  {
    ++m_stats.reused;
    // Where it goes in this frame's lists, and how far that moves it.
    const auto quad = static_cast<uint32_t>(m_quads.size());
    const auto hit = static_cast<uint32_t>(m_hits.size());
    const auto sense = static_cast<uint32_t>(m_next->senses.size());
    const auto inside = static_cast<uint32_t>(m_next->recorded.size());
    const std::vector<UiQuad>& quads = *m_last->quads;
    m_quads.insert(m_quads.end(),
                   quads.begin() + entry.quad,
                   quads.begin() + entry.quad + entry.quads);
    for (uint32_t i = entry.hit; i < entry.hit + entry.hits; ++i)
    {
      m_hits.push_back(m_last->hits[i]);
      m_hits.back().source = m_hits.back().source - entry.source + index;
    }
    m_next->senses.insert(m_next->senses.end(),
                          m_last->senses.begin() + entry.sense,
                          m_last->senses.begin() + entry.sense + entry.senses);
    // It, and the views kept inside it (moved with it), kept for next frame.
    const auto moved = [&](Entry kept)
    {
      kept.quad = kept.quad - entry.quad + quad;
      kept.hit = kept.hit - entry.hit + hit;
      kept.sense = kept.sense - entry.sense + sense;
      kept.source = kept.source - entry.source + index;
      kept.inside = kept.inside - entry.inside + inside;
      return kept;
    };
    for (uint32_t i = entry.inside; i < m_last->recorded.size(); ++i)
    {
      const uint64_t each = m_last->recorded[i];
      if (each == key)
        break;
      if (const auto found = m_last->entries.find(each);
          found != m_last->entries.end())
      {
        m_next->entries[each] = moved(found->second);
        m_next->recorded.push_back(each);
      }
    }
    m_next->entries[key] = moved(entry);
    m_next->recorded.push_back(key);
  }

  // What the pointer is to `id`: over it (1), holding it (2).
  uint8_t senseOf(Id id) const
  {
    return static_cast<uint8_t>((m_pointer.hovered == id ? 1 : 0) |
                                (m_pointer.pressed == id ? 2 : 0));
  }
  // Whether the pointer's over `id`, or holding it: the only ways a view's
  // look may read the pointer (so a kept view is drawn again when it
  // changes).
  bool hovered(Id id)
  {
    noteSense(id);
    return m_pointer.hovered == id;
  }
  bool pressed(Id id)
  {
    noteSense(id);
    return m_pointer.pressed == id;
  }
  void noteSense(Id id)
  {
    if (m_next)
      m_next->senses.push_back({id, senseOf(id)});
  }

  void placeNode(const Node& node,
                 Rect rect,
                 const Env& outer,
                 Id path,
                 uint32_t index)
  {
    const Env env = envOf(node, outer);
    if (!node.help.empty())
      addHit(path, rect, index).help = node.help;

    switch (node.kind)
    {
    case Kind::Empty:
    case Kind::Spacer:
      break;
    case Kind::Group:
    case Kind::VStack:
      placeStack(node, rect, env, path, index, false);
      break;
    case Kind::HStack:
      placeStack(node, rect, env, path, index, true);
      break;
    case Kind::ZStack:
    {
      uint32_t sameKind[kKinds] = {};
      forEachChild(
          node,
          index,
          [&](const Node& child, uint32_t at)
          {
            glm::vec2 size = measure(child, {rect.width, rect.height}, env);
            if (flexible(child, true))
              size.x = rect.width;
            if (flexible(child, false))
              size.y = rect.height;
            place(child,
                  aligned(rect, size, child.alignment),
                  env,
                  childPath(path,
                            child,
                            sameKind[static_cast<std::size_t>(child.kind)]++),
                  at);
          });
      break;
    }
    case Kind::Text:
      drawText(node.text, rect, env);
      break;
    case Kind::Image:
      drawImage(node, rect, env);
      break;
    case Kind::Button:
      placeButton(node, rect, env, path, index);
      break;
    case Kind::Slider:
      placeSlider(node, rect, env, path, index);
      break;
    case Kind::Toggle:
      placeToggle(node, rect, env, path, index);
      break;
    case Kind::Progress:
    {
      const float radius = rect.height * 0.5f;
      box(rect, toVec(m_theme.track, env.opacity), radius);
      Rect filled = rect;
      filled.width = rect.width * std::clamp(node.value, 0.0f, 1.0f);
      if (filled.width > 0.5f)
        box(filled,
            toVec(m_theme.accent, env.opacity),
            std::min(radius, filled.width * 0.5f));
      break;
    }
    case Kind::Padding:
      if (!node.children.empty())
      {
        const float s = m_scale;
        Rect inner{
            rect.x + node.insets.leading * s,
            rect.y + node.insets.top * s,
            rect.width - (node.insets.leading + node.insets.trailing) * s,
            rect.height - (node.insets.top + node.insets.bottom) * s};
        inner.width = std::max(inner.width, 0.0f);
        inner.height = std::max(inner.height, 0.0f);
        place(*node.children[0].node(), inner, env, step(path, 'p'), index + 1);
      }
      break;
    case Kind::Background:
      drawFill(node.fill, rect, env);
      // Covers the scene: clicks on it are the UI's.
      addHit(path, rect, index);
      if (!node.children.empty())
        place(*node.children[0].node(), rect, env, step(path, 'b'), index + 1);
      break;
    case Kind::Frame:
      if (!node.children.empty())
      {
        const Node& child = *node.children[0].node();
        glm::vec2 size = measure(child, {rect.width, rect.height}, env);
        if (flexible(child, true))
          size.x = rect.width;
        if (flexible(child, false))
          size.y = rect.height;
        size = glm::min(size, glm::vec2(rect.width, rect.height));
        place(child,
              aligned(rect, size, node.alignment),
              env,
              step(path, 'f'),
              index + 1);
      }
      break;
    case Kind::Scroll:
      placeScroll(node, rect, env, path, index);
      break;
    case Kind::Disclosure:
      placeDisclosure(node, rect, env, path, index);
      break;
    }
  }

  Hit& addHit(Id id, Rect rect, uint32_t source, bool enabled = true)
  {
    Hit& hit = m_hits.emplace_back();
    hit.id = id;
    hit.rect = rect;
    hit.enabled = enabled;
    hit.source = source;
    return hit;
  }

  // What's been drawn and made clickable since `firstQuad` and `firstHit`,
  // cut to `clip`: what's wholly outside it dropped (and the views kept
  // since `firstKept` moved to where theirs went; any that lost some of
  // theirs, not kept).
  void clipSince(std::size_t firstQuad,
                 std::size_t firstHit,
                 std::size_t firstKept,
                 Rect clip)
  {
    const glm::vec4 edges(
        clip.x, clip.y, clip.x + clip.width, clip.y + clip.height);
    // How many were kept before each (to move kept views' ranges by).
    m_keptQuads.assign(1, 0);
    std::size_t to = firstQuad;
    for (std::size_t i = firstQuad; i < m_quads.size(); ++i)
    {
      UiQuad& quad = m_quads[i];
      quad.clip = {std::max(quad.clip.x, edges.x),
                   std::max(quad.clip.y, edges.y),
                   std::min(quad.clip.z, edges.z),
                   std::min(quad.clip.w, edges.w)};
      const Rect rect{quad.rect.x, quad.rect.y, quad.rect.z, quad.rect.w};
      const bool outside = quad.clip.z <= quad.clip.x ||
                           quad.clip.w <= quad.clip.y ||
                           !rect.overlaps({quad.clip.x,
                                           quad.clip.y,
                                           quad.clip.z - quad.clip.x,
                                           quad.clip.w - quad.clip.y});
      if (!outside)
        m_quads[to++] = quad;
      m_keptQuads.push_back(static_cast<uint32_t>(to - firstQuad));
    }
    const bool droppedQuads = to != m_quads.size();
    m_quads.resize(to);
    m_keptHits.assign(1, 0);
    to = firstHit;
    for (std::size_t i = firstHit; i < m_hits.size(); ++i)
    {
      Hit& hit = m_hits[i];
      hit.rect = hit.rect.intersect(clip);
      if (hit.rect.width > 0.0f && hit.rect.height > 0.0f)
      {
        if (to != i)
          m_hits[to] = std::move(hit);
        ++to;
      }
      m_keptHits.push_back(static_cast<uint32_t>(to - firstHit));
    }
    const bool droppedHits = to != m_hits.size();
    m_hits.erase(
        m_hits.begin() + static_cast<std::ptrdiff_t>(to), m_hits.end());
    if (!m_next || (!droppedQuads && !droppedHits))
      return;
    for (std::size_t r = firstKept; r < m_next->recorded.size(); ++r)
    {
      const auto found = m_next->entries.find(m_next->recorded[r]);
      if (found == m_next->entries.end())
        continue;
      Entry& entry = found->second;
      const uint32_t quadFrom = m_keptQuads[entry.quad - firstQuad];
      const uint32_t quadTo = m_keptQuads[entry.quad + entry.quads - firstQuad];
      const uint32_t hitFrom = m_keptHits[entry.hit - firstHit];
      const uint32_t hitTo = m_keptHits[entry.hit + entry.hits - firstHit];
      if (quadTo - quadFrom != entry.quads || hitTo - hitFrom != entry.hits)
      {
        m_next->entries.erase(found);
        continue;
      }
      entry.quad = static_cast<uint32_t>(firstQuad) + quadFrom;
      entry.hit = static_cast<uint32_t>(firstHit) + hitFrom;
    }
  }

  // --- scroll views
  // ------------------------------------------------------------

  void placeScroll(const Node& node,
                   Rect rect,
                   const Env& env,
                   Id path,
                   uint32_t index)
  {
    if (node.children.empty())
      return;
    const Node& content = *node.children[0].node();
    const glm::vec2 view(rect.width, rect.height);
    glm::vec2 size = measure(content, contentOffer(node, view), env);
    glm::vec2 furthest(0.0f);
    for (int axis = 0; axis < 2; ++axis)
    {
      // Content smaller than the view (or not scrolled that way) and
      // stretching: the view's size.
      if (flexible(content, axis == 0) &&
          (!scrollsAlong(node, axis) || size[axis] < view[axis]))
        size[axis] = view[axis];
      if (!scrollsAlong(node, axis))
        size[axis] = std::min(size[axis], view[axis]);
      else
        furthest[axis] = std::max(size[axis] - view[axis], 0.0f);
    }

    // Where it's scrolled to: kept in the state (eased toward where it's
    // going by State::advance()).
    State::Scroll local;
    State::Scroll* state = &local;
    const int main = scrollsAlong(node, 1) ? 1 : 0;
    const float first = std::max(node.scrollOffset, 0.0f) * m_scale;
    if (m_state)
    {
      auto [entry, fresh] = m_state->scrolls.try_emplace(stateKey(node, path));
      state = &entry->second;
      if (fresh)
      {
        ++m_state->changes;
        state->offset[main] = state->target[main] = first;
      }
    }
    else
      state->offset[main] = state->target[main] = first;
    if (m_state && state->furthest != furthest)
      ++m_state->changes;
    state->furthest = furthest;
    state->target = glm::clamp(state->target, glm::vec2(0.0f), furthest);
    state->offset = glm::clamp(state->offset, glm::vec2(0.0f), furthest);
    const glm::vec2 offset = glm::round(state->offset);

    const Rect clip = m_clip.intersect(rect);
    const Rect outer = m_clip;
    const std::size_t firstQuad = m_quads.size(), firstHit = m_hits.size(),
                      firstKept = m_next ? m_next->recorded.size() : 0;
    // It covers the scene, and the wheel over it scrolls it (its content's
    // hits in front of it).
    Hit& hit = addHit(path, rect, index);
    if (m_state)
      hit.scroll = [all = m_state, state, axes = node.axes](glm::vec2 delta)
      {
        // Scrolling only sideways: the wheel goes along it.
        if (axes == Axis::Horizontal && delta.x == 0.0f)
          delta = {delta.y, 0.0f};
        if (axes == Axis::Horizontal)
          delta.y = 0.0f;
        if (axes == Axis::Vertical)
          delta.x = 0.0f;
        const glm::vec2 to = scrolled(state->target, delta, state->furthest);
        if (glm::all(
                glm::lessThan(glm::abs(to - state->target), glm::vec2(0.01f))))
          return false;
        state->target = to;
        ++all->changes;
        return true;
      };
    m_clip = clip;
    place(content,
          {rect.x - offset.x, rect.y - offset.y, size.x, size.y},
          env,
          step(path, 's'),
          index + 1);
    clipSince(firstQuad, firstHit, firstKept, clip);
    m_clip = outer;

    // Its bars, over the content, where it's bigger than the view.
    const bool both = furthest.x > 0.5f && furthest.y > 0.5f;
    for (int axis = 0; axis < 2; ++axis)
      if (furthest[axis] > 0.5f)
        scrollBar(
            rect, view[axis], size[axis], axis, both, *state, path, index);
  }

  void scrollBar(Rect rect,
                 float view,
                 float content,
                 int axis,
                 bool both,
                 State::Scroll& state,
                 Id path,
                 uint32_t index)
  {
    const float s = m_scale;
    const float inset = 3.0f * s;
    const Id id = step(path, axis == 1 ? 'v' : 'h');
    const bool held = pressed(id);
    const bool over = held || hovered(id);
    const float thick = (over ? 6.0f : 4.0f) * s;
    const float reach = 12.0f * s; // how wide it can be grabbed
    // Along the trailing edge (or the bottom), short of the other bar.
    const float start = (axis == 1 ? rect.y : rect.x) + inset;
    const float length =
        std::max((axis == 1 ? rect.height : rect.width) - 2.0f * inset -
                     (both ? thick + inset : 0.0f),
                 0.0f);
    const glm::vec2 thumb =
        scrollThumb(length, view, content, state.offset[axis], 20.0f * s);
    const auto across = [&](float width)
    {
      return axis == 1 ? rect.x + rect.width - inset - width
                       : rect.y + rect.height - inset - width;
    };
    const auto bar = [&](float from, float along, float width)
    {
      return axis == 1 ? Rect{across(width), from, width, along}
                       : Rect{from, across(width), along, width};
    };
    if (over)
      box(bar(start, length, thick),
          toVec(m_theme.scrollThumb, 0.35f),
          thick * 0.5f);
    box(bar(start + thumb.x, thumb.y, thick),
        toVec(over ? m_theme.scrollThumbHovered : m_theme.scrollThumb, 1.0f),
        thick * 0.5f);

    Hit& hit = addHit(id, bar(start, length, reach + inset), index);
    if (m_state)
    {
      State::Scroll* scroll = &state;
      const float page = view * 0.9f;
      // Pressed on the thumb: held there; above or below it: a page that way.
      hit.press =
          [all = m_state, scroll, axis, start, thumb, page](glm::vec2 at)
      {
        ++all->changes;
        const float along = at[axis] - start;
        if (along >= thumb.x && along <= thumb.x + thumb.y)
          scroll->grab = along - thumb.x;
        else
        {
          scroll->grab = -1.0f;
          glm::vec2 delta(0.0f);
          delta[axis] = along < thumb.x ? -page : page;
          scroll->target = scrolled(scroll->target, delta, scroll->furthest);
        }
      };
      hit.drag = [all = m_state, scroll, axis, start, length, thumb](
                     const Node&, glm::vec2 at)
      {
        if (scroll->grab < 0.0f)
          return;
        ++all->changes;
        const float room = std::max(length - thumb.y, 1.0f);
        const float t =
            std::clamp((at[axis] - start - scroll->grab) / room, 0.0f, 1.0f);
        scroll->target[axis] = scroll->offset[axis] =
            t * scroll->furthest[axis];
      };
    }
  }

  // --- disclosure groups, placed
  // -----------------------------------------------

  void placeDisclosure(const Node& node,
                       Rect rect,
                       const Env& env,
                       Id path,
                       uint32_t index)
  {
    if (node.children.size() < 3)
      return;
    const float s = m_scale;
    const Header header = headerOf(node, {rect.width, rect.height}, env);
    const Openness open = openness(node, stateKey(node, path));
    const Rect top{rect.x, rect.y, rect.width, header.size.y};

    // The header: clicked to open or close it.
    const bool enabled = !env.disabled;
    const Id id = step(path, 'h');
    Hit& hit = addHit(id, top, index, enabled);
    if (enabled && node.bound && node.change)
      hit.action = [](const Node& source)
      {
        if (source.change)
          source.change(source.value > 0.5f ? 0.0f : 1.0f);
      };
    else if (enabled && !node.bound && m_state && open.state)
      hit.action =
          [all = m_state, state = open.state, key = open.key](const Node&)
      {
        if (state->group == 0)
          state->open = !state->open;
        else
        {
          Id& inGroup = all->openInGroup[state->group];
          inGroup = inGroup == key ? 0 : key;
        }
        ++all->changes;
      };
    const bool over = hit.action && hovered(id);
    const float opacity = env.opacity * (enabled ? 1.0f : 0.45f);
    if (over)
    {
      Color tint = env.color;
      tint.a *= 0.08f;
      box({top.x - 4.0f * s, top.y, top.width + 8.0f * s, top.height},
          toVec(tint, opacity),
          6.0f * s);
    }

    // The chevron, turned down as it opens.
    const float chevron = kChevron * s;
    UiQuad mark;
    mark.kind = UiQuad::Chevron;
    mark.rect = {
        top.x, top.y + (top.height - chevron) * 0.5f, chevron, chevron};
    // (In its title's own colour, if it has one.)
    const Node& title = *node.children[0].node();
    mark.color = toVec(title.color ? *title.color : env.color, opacity);
    mark.radius = open.shown * std::numbers::pi_v<float> * 0.5f;
    mark.border = 1.75f * s;
    m_quads.push_back(mark);

    // Its label after it, its accessory at the far end.
    const uint32_t labelAt = index + 1;
    const uint32_t contentAt = labelAt + node.children[0].node()->count();
    const uint32_t accessoryAt = contentAt + node.children[1].node()->count();
    Env labelEnv = env;
    labelEnv.opacity = opacity;
    labelEnv.rehash();
    const float lead = (kChevron + kChevronGap) * s;
    place(*node.children[0].node(),
          aligned({top.x + lead, top.y, header.label.x, top.height},
                  header.label,
                  Alignment::Leading),
          labelEnv,
          step(path, 't'),
          labelAt);
    if (header.accessory.x > 0.0f)
      place(*node.children[2].node(),
            aligned({top.x + top.width - header.accessory.x,
                     top.y,
                     header.accessory.x,
                     top.height},
                    header.accessory,
                    Alignment::Trailing),
            env,
            step(path, 'a'),
            accessoryAt);

    // Its content below, as much as shows while it opens or closes.
    if (open.shown <= 0.0f)
      return;
    const Node& content = *node.children[1].node();
    const float gap = node.spacing * s;
    const float below = top.y + top.height + gap;
    glm::vec2 size = measure(content, {rect.width, infinity}, env);
    if (flexible(content, true))
      size.x = rect.width;
    size.x = std::min(size.x, rect.width);
    const Rect area{rect.x, below, size.x, size.y};
    if (open.shown >= 1.0f)
    {
      place(content, area, env, step(path, 'c'), contentAt);
      return;
    }
    const Rect clip = m_clip.intersect(
        {rect.x - 8.0f * s,
         top.y + top.height,
         rect.width + 16.0f * s,
         std::max(rect.y + rect.height - (top.y + top.height), 0.0f)});
    const Rect outer = m_clip;
    const std::size_t firstQuad = m_quads.size(), firstHit = m_hits.size(),
                      firstKept = m_next ? m_next->recorded.size() : 0;
    m_clip = clip;
    place(content, area, env, step(path, 'c'), contentAt);
    clipSince(firstQuad, firstHit, firstKept, clip);
    m_clip = outer;
  }

  void placeStack(const Node& node,
                  Rect rect,
                  const Env& env,
                  Id path,
                  uint32_t index,
                  bool horizontal)
  {
    // Its children (in a list shared down the tree, this stack's at the end
    // of it while it's placed).
    const std::size_t first = m_children.size();
    forEachChild(node,
                 index,
                 [&](const Node& child, uint32_t at) {
                   m_children.push_back({&child, at, glm::vec2(0.0f)});
                 });
    const std::size_t count = m_children.size() - first;
    if (count == 0)
      return;
    const int along = horizontal ? 0 : 1, across = 1 - along;
    const glm::vec2 room(rect.width, rect.height);

    // Each child's natural size; the flexible ones share what's left over.
    float fixed = node.spacing * m_scale * static_cast<float>(count - 1);
    int growing = 0;
    for (std::size_t i = first; i < first + count; ++i)
    {
      m_children[i].size = measure(*m_children[i].node, room, env);
      if (flexible(*m_children[i].node, horizontal))
        ++growing;
      else
        fixed += m_children[i].size[along];
    }
    // Short of room: its scroll views give theirs up first.
    for (std::size_t i = first; i < first + count && fixed > room[along]; ++i)
      if (givesWay(*m_children[i].node, horizontal))
      {
        const float cut =
            std::min(fixed - room[along], m_children[i].size[along]);
        m_children[i].size[along] -= cut;
        fixed -= cut;
      }
    const float share = growing > 0 ? std::max(room[along] - fixed, 0.0f) /
                                          static_cast<float>(growing)
                                    : 0.0f;

    float cursor = horizontal ? rect.x : rect.y;
    uint32_t sameKind[kKinds] = {};
    for (std::size_t i = first; i < first + count; ++i)
    {
      // (Copied: placing the child adds to the list.)
      const Child each{
          m_children[i].node, m_children[i].at, m_children[i].size};
      const Node& child = *each.node;
      glm::vec2 size = each.size;
      if (flexible(child, horizontal))
        size[along] = std::max(
            share, child.kind == Kind::Spacer ? child.value * m_scale : 0.0f);
      if (child.kind != Kind::Spacer && flexible(child, !horizontal))
        size[across] = room[across];
      size[across] = std::min(size[across], room[across]);

      // Lined up across the stack by its alignment.
      const float start = horizontal ? rect.y : rect.x;
      float offset = 0.0f;
      const float spare = room[across] - size[across];
      if (lineUp(node.stackAlignment, horizontal) == 1)
        offset = spare * 0.5f;
      else if (lineUp(node.stackAlignment, horizontal) == 2)
        offset = spare;
      Rect placed = horizontal ? Rect{cursor, start + offset, size.x, size.y}
                               : Rect{start + offset, cursor, size.x, size.y};
      place(child,
            placed,
            env,
            childPath(
                path, child, sameKind[static_cast<std::size_t>(child.kind)]++),
            each.at);
      cursor += size[along] + node.spacing * m_scale;
    }
    m_children.resize(first);
  }

  // Where a stack lines its children up across it: 0 start, 1 middle, 2 end.
  static int lineUp(Alignment alignment, bool horizontal)
  {
    switch (alignment)
    {
    case Alignment::TopLeading:
      return 0;
    case Alignment::Top:
      return horizontal ? 0 : 1;
    case Alignment::TopTrailing:
      return horizontal ? 0 : 2;
    case Alignment::Leading:
      return horizontal ? 1 : 0;
    case Alignment::Center:
      return 1;
    case Alignment::Trailing:
      return horizontal ? 1 : 2;
    case Alignment::BottomLeading:
      return horizontal ? 2 : 0;
    case Alignment::Bottom:
      return horizontal ? 2 : 1;
    case Alignment::BottomTrailing:
      return 2;
    }
    return 1;
  }

  static Rect aligned(Rect rect, glm::vec2 size, Alignment alignment)
  {
    const int column =
        static_cast<int>(alignment) % 3; // leading, middle, trailing
    const int row = static_cast<int>(alignment) / 3; // top, middle, bottom
    const float x =
        rect.x + (rect.width - size.x) * 0.5f * static_cast<float>(column);
    const float y =
        rect.y + (rect.height - size.y) * 0.5f * static_cast<float>(row);
    return {x, y, size.x, size.y};
  }

  void placeButton(const Node& node,
                   Rect rect,
                   const Env& env,
                   Id path,
                   uint32_t index)
  {
    const bool enabled = !env.disabled;
    Hit& hit = addHit(path, rect, index, enabled);
    hit.help = node.help;
    if (node.action)
      hit.action = [](const Node& source)
      {
        if (source.action)
          source.action();
      };
    // Its look (a label, a style's face) is placed over it, and what that
    // makes hit (a background covering the scene, a help) would be found
    // first, in front of it, and take its clicks: so it's made hit again, in
    // front of its look, once that's placed.
    const std::size_t own = m_hits.size() - 1;
    struct InFront
    {
      std::vector<Hit>& hits;
      std::size_t own;
      ~InFront()
      {
        if (hits.size() > own + 1)
        {
          Hit copy = hits[own];
          hits.push_back(std::move(copy));
        }
      }
    } inFront{m_hits, own};
    const bool over = enabled && hovered(path);
    const bool down = over && pressed(path);
    if (node.buttonStyle == ButtonStyle::Kind::Plain)
    {
      // Its label is the button: dimmed pressed, or disabled.
      if (node.children.empty())
        return;
      Env label = env;
      label.opacity *= !enabled ? 0.45f : down ? 0.6f : 1.0f;
      label.rehash();
      place(*node.children[0].node(), rect, label, step(path, 'l'), index + 1);
      return;
    }
    if (node.buttonStyle == ButtonStyle::Kind::Custom &&
        node.children.size() == 5)
    {
      // The look for its state (normal, hovered, pressed, disabled: its
      // children after the label), in the middle of its rect.
      const std::size_t look = !enabled ? 4 : down ? 3 : over ? 2 : 1;
      uint32_t at = index + 1;
      for (std::size_t i = 0; i < look; ++i)
        at += node.children[i].node()->count();
      const Node& face = *node.children[look].node();
      const glm::vec2 size = measure(face, {rect.width, rect.height}, env);
      place(face,
            aligned(rect,
                    glm::min(size, glm::vec2(rect.width, rect.height)),
                    Alignment::Center),
            env,
            step(path, 'l'),
            at);
      return;
    }
    const Color fill = down   ? m_theme.buttonPressed
                       : over ? m_theme.buttonHovered
                              : m_theme.button;
    const float opacity = env.opacity * (enabled ? 1.0f : 0.45f);
    const float radius = m_theme.buttonRadius * m_scale;
    // A soft shadow under it, less while pressed down.
    if (!down)
      shadow(rect, radius, 3.0f * m_scale, opacity * 0.5f);
    box(rect, toVec(fill, opacity), radius);

    if (node.children.empty())
      return;
    Env label = labelEnv(env);
    label.opacity = opacity;
    label.rehash();
    const Node& child = *node.children[0].node();
    const glm::vec2 pad = buttonPadding();
    const glm::vec2 size = measure(
        child, {rect.width - 2.0f * pad.x, rect.height - 2.0f * pad.y}, label);
    Rect labelRect = aligned(rect, size, Alignment::Center);
    if (down)
      labelRect.y += 1.0f * m_scale;
    place(child, labelRect, label, step(path, 'l'), index + 1);
  }

  // A slider's and a switch's track: the theme's, made out a little on a dark
  // panel by the text's colour.
  Color trackColor() const
  {
    const Color text = m_theme.text;
    return {text.r, text.g, text.b, std::max(m_theme.track.a * 0.6f, 0.2f)};
  }

  void placeSlider(const Node& node,
                   Rect rect,
                   const Env& env,
                   Id path,
                   uint32_t index)
  {
    const float s = m_scale;
    const bool enabled = !env.disabled && node.change;
    // The knob's centre runs from one end of the track to the other, a knob's
    // radius in from the slider's edges.
    const float knob = 16.0f * s;
    const float start = rect.x + knob * 0.5f;
    const float end = std::max(rect.x + rect.width - knob * 0.5f, start);
    const float span = node.high - node.low;
    const float fraction =
        span != 0.0f ? std::clamp((node.value - node.low) / span, 0.0f, 1.0f)
                     : 0.0f;

    Hit& hit = addHit(path, rect, index, enabled);
    hit.help = node.help;
    if (enabled)
      hit.drag = [start, end](const Node& source, glm::vec2 at)
      {
        if (!source.change)
          return;
        const float low = source.low, high = source.high;
        const float t =
            end > start ? std::clamp((at.x - start) / (end - start), 0.0f, 1.0f)
                        : 0.0f;
        float value = low + t * (high - low);
        if (source.step > 0.0f)
          value = low + std::round((value - low) / source.step) * source.step;
        source.change(
            std::clamp(value, std::min(low, high), std::max(low, high)));
      };
    const bool down = enabled && pressed(path);
    const bool over = enabled && (down || hovered(path));
    const float opacity = env.opacity * (enabled ? 1.0f : 0.45f);

    // The track, filled up to the knob.
    const float middle = rect.y + rect.height * 0.5f;
    const Rect track{start, middle - 3.0f * s, end - start, 6.0f * s};
    box(track, toVec(trackColor(), opacity), 3.0f * s);
    const float at = start + (end - start) * fraction;
    if (at - start > 0.5f)
      box({start, track.y, at - start, track.height},
          toVec(m_theme.accent, opacity),
          3.0f * s);
    const Rect handle{at - knob * 0.5f, middle - knob * 0.5f, knob, knob};
    shadow(handle, knob * 0.5f, 3.0f * s, opacity * 0.5f);
    const Color fill = down   ? m_theme.buttonPressed
                       : over ? m_theme.buttonHovered
                              : m_theme.button;
    box(handle, toVec(fill, opacity), knob * 0.5f);
  }

  void placeToggle(const Node& node,
                   Rect rect,
                   const Env& env,
                   Id path,
                   uint32_t index)
  {
    const float s = m_scale;
    const bool enabled = !env.disabled && node.change;
    const bool on = node.value > 0.5f;
    Hit& hit = addHit(path, rect, index, enabled);
    hit.help = node.help;
    if (enabled)
      hit.action = [](const Node& source)
      {
        if (source.change)
          source.change(source.value > 0.5f ? 0.0f : 1.0f);
      };
    const bool over = enabled && hovered(path);
    const float opacity = env.opacity * (enabled ? 1.0f : 0.45f);

    // The switch: a pill, the accent's colour while on, its knob at that end.
    const glm::vec2 size = kSwitch * s;
    const Rect pill{
        rect.x, rect.y + (rect.height - size.y) * 0.5f, size.x, size.y};
    box(pill,
        toVec(on ? m_theme.accent : trackColor(), opacity),
        size.y * 0.5f);
    const float knob = size.y - 4.0f * s;
    const Rect handle{
        on ? pill.x + pill.width - 2.0f * s - knob : pill.x + 2.0f * s,
        pill.y + 2.0f * s,
        knob,
        knob};
    box(handle,
        toVec(over ? m_theme.buttonHovered : m_theme.button, opacity),
        knob * 0.5f);

    if (node.children.empty())
      return;
    Env label = env;
    label.opacity = opacity;
    label.rehash();
    const Node& child = *node.children[0].node();
    const float lead = (kSwitch.x + 8.0f) * s;
    const Rect room{
        rect.x + lead, rect.y, std::max(rect.width - lead, 0.0f), rect.height};
    const glm::vec2 labelSize =
        measure(child, {room.width, room.height}, label);
    place(child,
          aligned(room, labelSize, Alignment::Leading),
          label,
          step(path, 'l'),
          index + 1);
  }

  // --- drawing
  // ----------------------------------------------------------------

  void box(Rect rect, glm::vec4 color, float radius, float softness = 0.0f)
  {
    UiQuad quad;
    quad.rect = {rect.x, rect.y, rect.width, rect.height};
    quad.color = color;
    quad.radius = radius;
    quad.softness = softness;
    m_quads.push_back(quad);
  }

  void shadow(Rect rect, float radius, float spread, float alpha)
  {
    Rect below{rect.x - spread * 0.5f,
               rect.y + spread * 0.4f,
               rect.width + spread,
               rect.height + spread};
    box(below,
        {0.0f, 0.0f, 0.0f, 0.35f * alpha},
        radius + spread * 0.5f,
        spread * 1.5f);
  }

  void drawFill(const Fill& fill, Rect rect, const Env& env)
  {
    const float radius = fill.cornerRadius * m_scale;
    if (fill.shadow > 0.0f)
      shadow(rect, radius, fill.shadow * m_scale, env.opacity * fill.color.a);
    if (fill.color.a <= 0.0f && fill.borderWidth <= 0.0f)
      return;
    UiQuad quad;
    quad.rect = {rect.x, rect.y, rect.width, rect.height};
    quad.color = toVec(fill.color, env.opacity);
    quad.radius = radius;
    quad.border = fill.borderWidth * m_scale;
    quad.borderColor = toVec(fill.borderColor, env.opacity);
    m_quads.push_back(quad);
  }

  void drawText(const std::string& text, Rect rect, const Env& env)
  {
    const float pixels = env.fontSize * m_scale;
    const float line = m_text.lineHeight(env.font, pixels);
    float y = rect.y;
    forEachLine(text,
                [&](std::string_view part)
                {
                  m_text.draw(env.font,
                              pixels,
                              part,
                              {std::round(rect.x), std::round(y)},
                              toVec(env.color, env.opacity),
                              m_quads);
                  y += line;
                });
  }

  struct Child
  {
    const Node* node;
    uint32_t at;
    glm::vec2 size;
  };

  float m_scale;
  const Theme& m_theme;
  TextShaper& m_text;
  const Pointer& m_pointer; // read through hovered() and pressed() only
  std::vector<UiQuad>& m_quads;
  std::vector<Hit>& m_hits;
  ImageSource* m_images;
  State* m_state;
  std::unordered_map<const Node*, Id> m_keys; // see findKeys()
  // Sizes measured: this layout's own (and a kept one's, for views with
  // state), and where they go (a kept layout's: the generation's).
  std::unordered_map<uint64_t, Measure> m_ownMeasures;
  std::unordered_map<uint64_t, Measure>* m_measures;
  Generation* m_last = nullptr;  // kept: last frame's
  Generation* m_next = nullptr;  // and this one's
  std::vector<Child> m_children; // stacks' children being placed
  std::vector<uint32_t> m_keptQuads, m_keptHits; // clipSince()'s
  Rect m_clip = kEverywhere; // what's being laid out is cut to
};

} // namespace

Id idKey(std::string_view id)
{
  return mixed(mixed(uint64_t{0x1D}, uint64_t{'#'}), id);
}

const Node* nodeAt(const Node& root, uint32_t index)
{
  const Node* node = &root;
  while (index > 0)
  {
    --index; // past `node`, to its children
    const Node* inside = nullptr;
    for (const View& child : node->children)
    {
      const uint32_t count = child.node()->count();
      if (index < count)
      {
        inside = child.node().get();
        break;
      }
      index -= count;
    }
    if (!inside)
      return nullptr;
    node = inside;
  }
  return node;
}

bool State::opening(Id key, const Disclosure& disclosure) const
{
  if (disclosure.bound)
    return disclosure.want;
  if (disclosure.group != 0)
  {
    const auto open = openInGroup.find(disclosure.group);
    return open != openInGroup.end() && open->second == key;
  }
  return disclosure.open;
}

void State::advance(float seconds)
{
  seconds = std::max(seconds, 0.0f);
  for (auto& [key, scroll] : scrolls)
    if (scroll.offset != scroll.target)
    {
      ++changes; // still easing
      const float ease = 1.0f - std::exp(-kScrollEase * seconds);
      scroll.offset += (scroll.target - scroll.offset) * ease;
      for (int axis = 0; axis < 2; ++axis)
        if (std::abs(scroll.target[axis] - scroll.offset[axis]) < 0.5f)
          scroll.offset[axis] = scroll.target[axis];
    }
  const float step = seconds / kDisclosureTime;
  for (auto& [key, disclosure] : disclosures)
  {
    const float was = disclosure.progress;
    disclosure.progress = opening(key, disclosure)
                              ? std::min(disclosure.progress + step, 1.0f)
                              : std::max(disclosure.progress - step, 0.0f);
    if (disclosure.progress != was)
      ++changes; // still easing
  }
}

Cache::Cache() : m_data(std::make_unique<Data>()) {}
Cache::~Cache() = default;

const std::shared_ptr<const std::vector<UiQuad>>& Cache::quads() const
{
  return m_data->shown;
}

const std::vector<Hit>& Cache::hits() const
{
  return m_data->frames[m_data->current].hits;
}

const Cache::Stats& Cache::stats() const { return m_data->stats; }

void Cache::clear()
{
  for (Generation& frame : m_data->frames)
  {
    frame.entries.clear();
    frame.measures.clear();
  }
  m_data->layerKey = 0;
}

namespace
{

// The body laid out on the screen, as a ZStack's child is.
void layBody(Layout& laying, const Node& node, glm::vec2 size)
{
  const Env env = laying.rootEnv();
  const glm::vec2 natural = laying.fitted(node, size, env);
  const int column = static_cast<int>(node.alignment) % 3;
  const int row = static_cast<int>(node.alignment) / 3;
  const Rect rect{(size.x - natural.x) * 0.5f * static_cast<float>(column),
                  (size.y - natural.y) * 0.5f * static_cast<float>(row),
                  natural.x,
                  natural.y};
  laying.place(node, rect, env, kRoot, 0);
}

} // namespace

void layout(const View& root,
            glm::vec2 size,
            float scale,
            const Theme& theme,
            TextShaper& text,
            const Pointer& pointer,
            std::vector<UiQuad>& quads,
            std::vector<Hit>& hits,
            ImageSource* images,
            std::vector<Placed>* placed,
            State* state)
{
  Layout laying(scale, theme, text, pointer, quads, hits, images, state);
  laying.m_placed = placed;
  const Node& node = *root.node();
  if (state)
    laying.findKeys(node, kRoot);
  layBody(laying, node, size);
}

void layout(const View& root,
            glm::vec2 size,
            float scale,
            const Theme& theme,
            TextShaper& text,
            const Pointer& pointer,
            State& state,
            Cache& cache,
            ImageSource* images,
            std::vector<Placed>* placed)
{
  Cache::Data& data = cache.data();
  data.stats = {};
  const Node& node = *root.node();

  // What everything's drawn with: changed, nothing kept is any use.
  const uint64_t global =
      mixed(mixed(mixed(mixed(mixed(themeHash(theme), scale), size.x), size.y),
                  text.generation()),
            uint64_t{0x61});
  if (global != data.globalKey)
  {
    cache.clear();
    data.globalKey = global;
  }

  // The whole layer as it was: nothing to do (its fonts kept).
  Generation& last = data.frames[data.current];
  const uint64_t layerKey = mixed(node.hash(), global);
  if (!placed && layerKey == data.layerKey && !last.waiting &&
      (!last.readState || state.changes == last.stamp))
  {
    bool same = true;
    for (const Sense& sense : last.senses)
      same = same && static_cast<uint8_t>(
                         (pointer.hovered == sense.id ? 1 : 0) |
                         (pointer.pressed == sense.id ? 2 : 0)) == sense.bits;
    if (same)
    {
      data.stats.kept = true;
      for (const UiTexture* texture : data.textures)
        text.keep(texture);
      return;
    }
  }

  // Laid out again into the other generation, using what's unchanged from
  // this one.
  data.current ^= 1;
  Generation& next = data.frames[data.current];
  if (next.quads.use_count() > 1)
    next.quads = std::make_shared<std::vector<UiQuad>>();
  next.quads->clear();
  next.hits.clear();
  next.senses.clear();
  next.entries.clear();
  next.recorded.clear();
  next.measures.clear();
  const uint64_t generation = text.generation();
  Layout laying(
      scale, theme, text, pointer, *next.quads, next.hits, images, &state);
  laying.m_placed = placed;
  if (!placed)
    laying.keep(last, next);
  laying.findKeys(node, kRoot);
  layBody(laying, node, size);

  data.stats.placed = laying.m_stats.placed;
  data.stats.reused = laying.m_stats.reused;
  next.stamp = state.changes;
  next.readState = node.keepsState();
  next.waiting = laying.m_waiting;
  // Glyphs' textures grew while it was drawn: drawn again next frame (some
  // of its quads are already wrong).
  data.layerKey = placed || text.generation() != generation ? 0 : layerKey;
  if (data.layerKey == 0)
    next.entries.clear();
  last.entries.clear();
  last.measures.clear();
  data.textures.clear();
  for (const UiQuad& quad : *next.quads)
    if (quad.kind == UiQuad::Glyph && quad.texture &&
        (data.textures.empty() || data.textures.back() != quad.texture) &&
        std::find(data.textures.begin(), data.textures.end(), quad.texture) ==
            data.textures.end())
      data.textures.push_back(quad.texture);
  data.shown = next.quads;
}

glm::vec2 fit(const View& root,
              glm::vec2 offer,
              float scale,
              const Theme& theme,
              TextShaper& text,
              ImageSource* images)
{
  std::vector<UiQuad> quads;
  std::vector<Hit> hits;
  Layout laying(scale, theme, text, Pointer(), quads, hits, images);
  return laying.fitted(*root.node(), offer, laying.rootEnv());
}

const SourceImage::Level& SourceImage::levelFor(float pixels) const
{
  int at = 0;
  while (at + 1 < std::min(levels, kMostLevels) &&
         static_cast<float>(level[at + 1].size.x) >= pixels - 0.01f)
    ++at;
  return level[at];
}

Rect Rect::intersect(const Rect& other) const
{
  const float left = std::max(x, other.x);
  const float top = std::max(y, other.y);
  const float right = std::min(x + width, other.x + other.width);
  const float bottom = std::min(y + height, other.y + other.height);
  return {
      left, top, std::max(right - left, 0.0f), std::max(bottom - top, 0.0f)};
}

glm::vec2 scrolled(glm::vec2 from, glm::vec2 delta, glm::vec2 furthest)
{
  return glm::clamp(
      from + delta, glm::vec2(0.0f), glm::max(furthest, glm::vec2(0.0f)));
}

glm::vec2 scrollThumb(float track,
                      float view,
                      float content,
                      float offset,
                      float shortest)
{
  if (track <= 0.0f || content <= view || content <= 0.0f)
    return {0.0f, std::max(track, 0.0f)};
  const float length =
      std::clamp(track * view / content, std::min(shortest, track), track);
  const float t = std::clamp(offset / (content - view), 0.0f, 1.0f);
  return {(track - length) * t, length};
}

const char* kindName(Node::Kind kind)
{
  switch (kind)
  {
  case Node::Kind::Empty:
    return "Empty";
  case Node::Kind::Text:
    return "Text";
  case Node::Kind::Image:
    return "Image";
  case Node::Kind::Button:
    return "Button";
  case Node::Kind::Progress:
    return "ProgressBar";
  case Node::Kind::Slider:
    return "Slider";
  case Node::Kind::Toggle:
    return "Toggle";
  case Node::Kind::Spacer:
    return "Spacer";
  case Node::Kind::VStack:
    return "VStack";
  case Node::Kind::HStack:
    return "HStack";
  case Node::Kind::ZStack:
    return "ZStack";
  case Node::Kind::Group:
    return "ForEach";
  case Node::Kind::Padding:
    return "padding";
  case Node::Kind::Frame:
    return "frame";
  case Node::Kind::Background:
    return "background";
  case Node::Kind::Scroll:
    return "ScrollView";
  case Node::Kind::Disclosure:
    return "DisclosureGroup";
  }
  return "?";
}

void tooltip(std::string_view text,
             glm::vec2 pointer,
             glm::vec2 size,
             float scale,
             const Theme& theme,
             TextShaper& shaper,
             std::vector<UiQuad>& quads)
{
  const float pixels = 16.0f * scale;
  const float line = shaper.lineHeight(theme.font, pixels);
  const glm::vec2 pad = glm::vec2(8.0f, 5.0f) * scale;
  const glm::vec2 box(shaper.width(theme.font, pixels, text) + 2.0f * pad.x,
                      line + 2.0f * pad.y);
  // Below and right of the pointer, kept on the screen.
  glm::vec2 at = pointer + glm::vec2(14.0f, 20.0f) * scale;
  at = glm::clamp(at, glm::vec2(0.0f), glm::max(size - box, glm::vec2(0.0f)));
  UiQuad quad;
  quad.rect = {at.x, at.y, box.x, box.y};
  quad.color = {0.1f, 0.09f, 0.08f, 0.94f};
  quad.radius = 6.0f * scale;
  quads.push_back(quad);
  shaper.draw(theme.font,
              pixels,
              text,
              glm::round(at + pad),
              {theme.text.r, theme.text.g, theme.text.b, 1.0f},
              quads);
}

} // namespace sfs::ui
