#include "engine/runtime/ui/layout.h"

#include "glm/glm/common.hpp"

#include <algorithm>
#include <cmath>

namespace sfs::ui
{

namespace
{

using Kind = Node::Kind;

// What a node inherits from those around it.
struct Env
{
  std::string font;
  float fontSize = 20.0f; // points
  Color color;
  float opacity = 1.0f;
  bool disabled = false;
};

glm::vec4 toVec(Color color, float opacity)
{
  return {color.r, color.g, color.b, color.a * opacity};
}

class Layout
{
public:
  Layout(float scale,
         const Theme& theme,
         TextShaper& text,
         const Pointer& pointer,
         std::vector<UiQuad>& quads,
         std::vector<Hit>& hits,
         ImageSource* images)
      : m_scale(scale), m_theme(theme), m_text(text), m_pointer(pointer),
        m_quads(quads), m_hits(hits), m_images(images)
  {
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
    env.color = m_theme.text;
    return env;
  }

  // A node's surroundings, with what it sets itself.
  static Env envOf(const Node& node, const Env& outer)
  {
    Env env = outer;
    if (node.font)
    {
      if (!node.font->file.empty())
        env.font = node.font->file;
      env.fontSize = node.font->size;
    }
    if (node.color)
      env.color = *node.color;
    env.opacity *= node.opacity;
    env.disabled = env.disabled || node.disabled;
    return env;
  }

  // A stack's children, with ForEach's groups opened out and nothing left
  // out.
  static std::vector<const Node*> childrenOf(const Node& node)
  {
    std::vector<const Node*> out;
    for (const View& child : node.children)
    {
      const Node& c = *child.node();
      if (c.kind == Kind::Group)
        for (const Node* inner : childrenOf(c))
          out.push_back(inner);
      else if (c.kind != Kind::Empty)
        out.push_back(&c);
    }
    return out;
  }

  // Whether it takes all the room it's given, across (horizontal) or down.
  static bool flexible(const Node& node, bool horizontal)
  {
    switch (node.kind)
    {
    case Kind::Spacer:
    case Kind::ZStack:
      return true;
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
      for (const Node* child : childrenOf(node))
        if ((along || child->kind != Kind::Spacer) &&
            flexible(*child, horizontal))
          return true;
      return false;
    }
    default:
      return false;
    }
  }

  // --- measuring
  // --------------------------------------------------------------

  glm::vec2 measure(const Node& node, glm::vec2 offer, const Env& outer)
  {
    const Env env = envOf(node, outer);
    const float s = m_scale;
    switch (node.kind)
    {
    case Kind::Empty:
    case Kind::Group:
      if (node.kind == Kind::Group)
        return measureStack(node, offer, env, false);
      return {0.0f, 0.0f};
    case Kind::Text:
      return textSize(node.text, env);
    case Kind::Image:
      return imageSize(node, offer);
    case Kind::Button:
    {
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
    case Kind::Spacer:
      return glm::vec2(node.value * s);
    case Kind::VStack:
      return measureStack(node, offer, env, false);
    case Kind::HStack:
      return measureStack(node, offer, env, true);
    case Kind::ZStack:
    {
      glm::vec2 most(0.0f);
      for (const Node* child : childrenOf(node))
        most = glm::max(most, measure(*child, offer, env));
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
    }
    return {0.0f, 0.0f};
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
    const std::vector<const Node*> children = childrenOf(node);
    const int along = horizontal ? 0 : 1, across = 1 - along;
    float total = 0.0f, widest = 0.0f;
    bool grows = false, fills = false;
    for (const Node* child : children)
    {
      const glm::vec2 size = measure(*child, offer, env);
      total += size[along];
      widest = std::max(widest, size[across]);
      grows = grows || flexible(*child, horizontal);
      // (A spacer only stretches along the stack.)
      fills = fills ||
              (child->kind != Kind::Spacer && flexible(*child, !horizontal));
    }
    if (!children.empty())
      total += node.spacing * m_scale * static_cast<float>(children.size() - 1);
    glm::vec2 size;
    size[along] = grows && std::isfinite(offer[along])
                      ? std::max(offer[along], total)
                      : total;
    size[across] =
        fills && std::isfinite(offer[across]) ? offer[across] : widest;
    return size;
  }

  const UiTexture* imageOf(const Node& node)
  {
    return m_images && !node.placeholder
               ? m_images->image(node.text, node.meshIcon)
               : nullptr;
  }

  // An image's own size (a mesh's icon or a placeholder 32 points), shrunk
  // to fit what it's offered, keeping its shape.
  glm::vec2 imageSize(const Node& node, glm::vec2 offer)
  {
    const UiTexture* texture = imageOf(node);
    glm::vec2 size = node.meshIcon || node.placeholder
                         ? glm::vec2(32.0f * m_scale)
                     : texture ? glm::vec2(texture->size) * m_scale
                               : glm::vec2(0.0f);
    float fit = 1.0f;
    if (std::isfinite(offer.x) && size.x > offer.x && size.x > 0.0f)
      fit = std::min(fit, offer.x / size.x);
    if (std::isfinite(offer.y) && size.y > offer.y && size.y > 0.0f)
      fit = std::min(fit, offer.y / size.y);
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
    const UiTexture* texture = imageOf(node);
    if (!texture || texture->size.x <= 0 || texture->size.y <= 0)
      return;
    // Its shape kept, in the middle of its rect.
    const glm::vec2 own(texture->size);
    const float fit = std::min(rect.width / own.x, rect.height / own.y);
    const glm::vec2 size = own * fit;
    UiQuad quad;
    quad.kind = UiQuad::Image;
    quad.texture = texture;
    quad.rect = {rect.x + (rect.width - size.x) * 0.5f,
                 rect.y + (rect.height - size.y) * 0.5f,
                 size.x,
                 size.y};
    quad.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    quad.color = {1.0f, 1.0f, 1.0f, env.opacity};
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
    return label;
  }

  // --- placing
  // ----------------------------------------------------------------

  void
  place(const Node& node, Rect rect, const Env& outer, const std::string& path)
  {
    if (m_placed)
      m_placed->push_back({rect,
                           node.kind,
                           m_depth,
                           node.kind == Kind::Text || node.kind == Kind::Image
                               ? node.text
                               : std::string()});
    ++m_depth;
    placeNode(node, rect, outer, path);
    --m_depth;
  }

  std::vector<Placed>* m_placed = nullptr;

private:
  int m_depth = 0;

  void placeNode(const Node& node,
                 Rect rect,
                 const Env& outer,
                 const std::string& path)
  {
    const Env env = envOf(node, outer);
    if (!node.help.empty())
      m_hits.push_back({path, rect, {}, true, node.help});

    switch (node.kind)
    {
    case Kind::Empty:
    case Kind::Spacer:
      break;
    case Kind::Group:
    case Kind::VStack:
      placeStack(node, rect, env, path, false);
      break;
    case Kind::HStack:
      placeStack(node, rect, env, path, true);
      break;
    case Kind::ZStack:
    {
      int index = 0;
      for (const Node* child : childrenOf(node))
      {
        glm::vec2 size = measure(*child, {rect.width, rect.height}, env);
        if (flexible(*child, true))
          size.x = rect.width;
        if (flexible(*child, false))
          size.y = rect.height;
        place(*child,
              aligned(rect, size, child->alignment),
              env,
              childPath(path, *child, index++));
      }
      break;
    }
    case Kind::Text:
      drawText(node.text, rect, env);
      break;
    case Kind::Image:
      drawImage(node, rect, env);
      break;
    case Kind::Button:
      placeButton(node, rect, env, path);
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
        place(*node.children[0].node(), inner, env, path + "/p");
      }
      break;
    case Kind::Background:
      drawFill(node.fill, rect, env);
      // Covers the scene: clicks on it are the UI's.
      m_hits.push_back({path, rect, {}, true, {}});
      if (!node.children.empty())
        place(*node.children[0].node(), rect, env, path + "/b");
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
        place(child, aligned(rect, size, node.alignment), env, path + "/f");
      }
      break;
    }
  }

  void placeStack(const Node& node,
                  Rect rect,
                  const Env& env,
                  const std::string& path,
                  bool horizontal)
  {
    const std::vector<const Node*> children = childrenOf(node);
    if (children.empty())
      return;
    const int along = horizontal ? 0 : 1, across = 1 - along;
    const glm::vec2 room(rect.width, rect.height);

    // Each child's natural size; the flexible ones share what's left over.
    std::vector<glm::vec2> sizes;
    float fixed =
        node.spacing * m_scale * static_cast<float>(children.size() - 1);
    int growing = 0;
    for (const Node* child : children)
    {
      sizes.push_back(measure(*child, room, env));
      if (flexible(*child, horizontal))
        ++growing;
      else
        fixed += sizes.back()[along];
    }
    const float share = growing > 0 ? std::max(room[along] - fixed, 0.0f) /
                                          static_cast<float>(growing)
                                    : 0.0f;

    float cursor = horizontal ? rect.x : rect.y;
    for (std::size_t i = 0; i < children.size(); ++i)
    {
      const Node& child = *children[i];
      glm::vec2 size = sizes[i];
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
      place(child, placed, env, childPath(path, child, static_cast<int>(i)));
      cursor += size[along] + node.spacing * m_scale;
    }
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

  static std::string
  childPath(const std::string& path, const Node& child, int index)
  {
    return path + "/" +
           (child.id.empty() ? std::to_string(index) : "#" + child.id);
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
                   const std::string& path)
  {
    const bool enabled = !env.disabled;
    m_hits.push_back({path, rect, node.action, enabled, node.help});
    const bool hovered = enabled && m_pointer.hovered == path;
    const bool pressed = enabled && hovered && m_pointer.pressed == path;
    const Color fill = pressed   ? m_theme.buttonPressed
                       : hovered ? m_theme.buttonHovered
                                 : m_theme.button;
    const float opacity = env.opacity * (enabled ? 1.0f : 0.45f);
    const float radius = m_theme.buttonRadius * m_scale;
    // A soft shadow under it, less while pressed down.
    if (!pressed)
      shadow(rect, radius, 3.0f * m_scale, opacity * 0.5f);
    box(rect, toVec(fill, opacity), radius);

    if (node.children.empty())
      return;
    Env label = labelEnv(env);
    label.opacity = opacity;
    const Node& child = *node.children[0].node();
    const glm::vec2 pad = buttonPadding();
    const glm::vec2 size = measure(
        child, {rect.width - 2.0f * pad.x, rect.height - 2.0f * pad.y}, label);
    Rect labelRect = aligned(rect, size, Alignment::Center);
    if (pressed)
      labelRect.y += 1.0f * m_scale;
    place(child, labelRect, label, path + "/l");
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

private:
  float m_scale;
  const Theme& m_theme;
  TextShaper& m_text;
  const Pointer& m_pointer;
  std::vector<UiQuad>& m_quads;
  std::vector<Hit>& m_hits;
  ImageSource* m_images;
};

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
            std::vector<Placed>* placed)
{
  Layout laying(scale, theme, text, pointer, quads, hits, images);
  laying.m_placed = placed;
  const Node& node = *root.node();
  const Env env = laying.rootEnv();
  // The body on the screen, as a ZStack's child is.
  const glm::vec2 natural = laying.fitted(node, size, env);
  const int column = static_cast<int>(node.alignment) % 3;
  const int row = static_cast<int>(node.alignment) / 3;
  const Rect rect{(size.x - natural.x) * 0.5f * static_cast<float>(column),
                  (size.y - natural.y) * 0.5f * static_cast<float>(row),
                  natural.x,
                  natural.y};
  laying.place(node, rect, env, "");
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
