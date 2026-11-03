#include "engine/runtime/ui/ui.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>

namespace sfs::ui
{

namespace
{

std::shared_ptr<Node> make(Node::Kind kind)
{
  auto node = std::make_shared<Node>();
  node->kind = kind;
  return node;
}

// `view` wrapped in a new node of `kind` (padding, frame, background).
std::shared_ptr<Node> wrap(const View& view, Node::Kind kind)
{
  auto node = make(kind);
  node->children.push_back(view);
  return node;
}

// A copy of `view`'s node, to change an attribute of.
std::shared_ptr<Node> copy(const View& view)
{
  auto node = std::make_shared<Node>(*view.node());
  node->m_count = 0; // its hash worked out again
  return node;
}

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
uint64_t mixed(uint64_t hash, const std::string& text)
{
  return mixed(
      hash, static_cast<uint64_t>(std::hash<std::string_view>()(text)));
}
uint64_t mixed(uint64_t hash, Color color)
{
  return mixed(mixed(mixed(mixed(hash, color.r), color.g), color.b), color.a);
}

// The background nearest the top: `view` itself, or one under the padding,
// frame or other modifiers wrapping it, made again with `change` applied.
template <typename Change>
View withBackground(const View& view, Change&& change)
{
  const Node& node = *view.node();
  if (node.kind == Node::Kind::Background)
  {
    auto changed = copy(view);
    change(changed->fill);
    return View(changed);
  }
  if ((node.kind == Node::Kind::Padding || node.kind == Node::Kind::Frame) &&
      !node.children.empty())
  {
    auto changed = copy(view);
    changed->children[0] = withBackground(node.children[0], change);
    return View(changed);
  }
  // None yet: one, clear, to hold it.
  auto added = wrap(view, Node::Kind::Background);
  change(added->fill);
  return View(added);
}

std::vector<View> listOf(std::initializer_list<View> views)
{
  return std::vector<View>(views.begin(), views.end());
}

View stack(Node::Kind kind, std::vector<View> children, Alignment alignment)
{
  auto node = make(kind);
  node->children = std::move(children);
  node->stackAlignment = alignment;
  return View(node);
}

} // namespace

Fill Panel()
{
  // A warm, dark, see-through card with soft corners and a soft shadow.
  Fill fill;
  fill.color = Color::rgb(38, 33, 28, 0.86f);
  fill.cornerRadius = 12.0f;
  fill.borderColor = Color::rgb(255, 240, 210, 0.12f);
  fill.borderWidth = 1.0f;
  fill.shadow = 10.0f;
  return fill;
}

View::View()
{
  // Nothing: one node shared by every empty view (it never changes).
  static const std::shared_ptr<const Node> empty = make(Node::Kind::Empty);
  m_node = empty;
}

std::shared_ptr<Node> View::owned() &&
{
  if (m_node.use_count() == 1)
  {
    // Made non-const (make()), and no one else sees it.
    auto node = std::const_pointer_cast<Node>(std::move(m_node));
    node->m_count = 0;
    return node;
  }
  return copy(*this);
}

View View::group(std::vector<View> views)
{
  auto node = make(Node::Kind::Group);
  node->children = std::move(views);
  return View(node);
}

View View::padding(float points) const { return padding(Insets::all(points)); }

View View::padding(Insets insets) const
{
  auto node = wrap(*this, Node::Kind::Padding);
  node->insets = insets;
  return View(node);
}

View View::frame(float width, float height) const
{
  auto node = wrap(*this, Node::Kind::Frame);
  node->width = width;
  node->height = height;
  return View(node);
}

View View::maxWidth(float points) const
{
  auto node = wrap(*this, Node::Kind::Frame);
  node->maxWidth = points;
  return View(node);
}

View View::minWidth(float points) const
{
  auto node = wrap(*this, Node::Kind::Frame);
  node->minWidth = points;
  return View(node);
}

View View::maxHeight(float points) const
{
  auto node = wrap(*this, Node::Kind::Frame);
  node->maxHeight = points;
  return View(node);
}

View View::minHeight(float points) const
{
  auto node = wrap(*this, Node::Kind::Frame);
  node->minHeight = points;
  return View(node);
}

View View::align(Alignment alignment) const&
{
  return View(*this).align(alignment);
}

View View::align(Alignment alignment) &&
{
  auto node = std::move(*this).owned();
  node->alignment = alignment;
  return View(std::move(node));
}

View View::spacing(float points) const& { return View(*this).spacing(points); }

View View::spacing(float points) &&
{
  auto node = std::move(*this).owned();
  node->spacing = points;
  return View(std::move(node));
}

View View::background(Color color) const
{
  Fill fill;
  fill.color = color;
  return background(fill);
}

View View::background(Fill fill) const
{
  auto node = wrap(*this, Node::Kind::Background);
  node->fill = fill;
  return View(node);
}

View View::cornerRadius(float points) const
{
  return withBackground(*this, [&](Fill& fill) { fill.cornerRadius = points; });
}

View View::border(Color color, float width) const
{
  return withBackground(*this,
                        [&](Fill& fill)
                        {
                          fill.borderColor = color;
                          fill.borderWidth = width;
                        });
}

View View::color(Color color) const& { return View(*this).color(color); }

View View::color(Color color) &&
{
  auto node = std::move(*this).owned();
  node->color = color;
  return View(std::move(node));
}

View View::font(Font font) const& { return View(*this).font(std::move(font)); }

View View::font(Font font) &&
{
  auto node = std::move(*this).owned();
  node->font = std::move(font);
  return View(std::move(node));
}

View View::opacity(float alpha) const& { return View(*this).opacity(alpha); }

View View::opacity(float alpha) &&
{
  auto node = std::move(*this).owned();
  node->opacity *= alpha;
  return View(std::move(node));
}

View View::disabled(bool disabled) const&
{
  return View(*this).disabled(disabled);
}

View View::disabled(bool disabled) &&
{
  auto node = std::move(*this).owned();
  node->disabled = node->disabled || disabled;
  return View(std::move(node));
}

View View::help(std::string text) const&
{
  return View(*this).help(std::move(text));
}

View View::help(std::string text) &&
{
  auto node = std::move(*this).owned();
  node->help = std::move(text);
  return View(std::move(node));
}

View View::id(std::string id) const& { return View(*this).id(std::move(id)); }

View View::id(std::string id) &&
{
  auto node = std::move(*this).owned();
  node->id = std::move(id);
  return View(std::move(node));
}

View View::scrollOffset(float points) const&
{
  return View(*this).scrollOffset(points);
}

View View::scrollOffset(float points) &&
{
  auto node = std::move(*this).owned();
  node->scrollOffset = std::max(points, 0.0f);
  return View(std::move(node));
}

View View::accessory(View view) const&
{
  return View(*this).accessory(std::move(view));
}

View View::accessory(View view) &&
{
  auto node = std::move(*this).owned();
  if (node->kind == Node::Kind::Disclosure && node->children.size() == 3)
    node->children[2] = std::move(view);
  return View(std::move(node));
}

View View::accordion(std::string group) const&
{
  return View(*this).accordion(std::move(group));
}

View View::accordion(std::string group) &&
{
  auto node = std::move(*this).owned();
  node->group = std::move(group);
  return View(std::move(node));
}

View Text(std::string text)
{
  auto node = make(Node::Kind::Text);
  node->text = std::move(text);
  return View(node);
}

Image::Image(std::string file)
    : View(
          [&]
          {
            auto node = make(Node::Kind::Image);
            node->text = std::move(file);
            return node;
          }())
{
}

View Image::mesh(std::string name)
{
  auto node = make(Node::Kind::Image);
  node->text = std::move(name);
  node->meshIcon = true;
  return View(node);
}

View Image::mesh(MeshHandle mesh)
{
  // By its id ('#' can't start a mesh's name); the module finds it.
  return Image::mesh("#" + std::to_string(mesh.id));
}

View Image::placeholder()
{
  auto node = make(Node::Kind::Image);
  node->placeholder = true;
  return View(node);
}

View Button(std::string label, std::function<void()> action)
{
  return Button(Text(std::move(label)), std::move(action));
}

View Button(View label, std::function<void()> action)
{
  auto node = make(Node::Kind::Button);
  node->children.push_back(std::move(label));
  node->action = std::move(action);
  return View(node);
}

View ProgressBar(float fraction)
{
  auto node = make(Node::Kind::Progress);
  node->value = fraction;
  return View(node);
}

View Slider(float value,
            std::function<void(float)> change,
            float low,
            float high,
            float step)
{
  auto node = make(Node::Kind::Slider);
  node->value = value;
  node->change = std::move(change);
  node->low = low;
  node->high = high;
  node->step = step;
  return View(node);
}

View Toggle(std::string label, bool on, std::function<void(bool)> change)
{
  auto node = make(Node::Kind::Toggle);
  node->children.push_back(Text(std::move(label)));
  node->value = on ? 1.0f : 0.0f;
  if (change)
    node->change = [change = std::move(change)](float to)
    { change(to > 0.5f); };
  return View(node);
}

View ScrollView(View content, Axis axes)
{
  auto node = make(Node::Kind::Scroll);
  node->children.push_back(std::move(content));
  node->axes = axes;
  return View(node);
}

View DisclosureGroup(std::string title, View content, bool open)
{
  return DisclosureGroup(Text(std::move(title)), std::move(content), open);
}

View DisclosureGroup(View label, View content, bool open)
{
  auto node = make(Node::Kind::Disclosure);
  node->children = {std::move(label), std::move(content), View()};
  node->value = open ? 1.0f : 0.0f;
  node->spacing = 6.0f;
  return View(node);
}

View DisclosureGroup(std::string title,
                     bool open,
                     std::function<void(bool)> change,
                     View content)
{
  auto node = make(Node::Kind::Disclosure);
  node->children = {Text(std::move(title)), std::move(content), View()};
  node->value = open ? 1.0f : 0.0f;
  node->bound = true;
  node->spacing = 6.0f;
  if (change)
    node->change = [change = std::move(change)](float to)
    { change(to > 0.5f); };
  return View(node);
}

View Spacer(float minLength)
{
  auto node = make(Node::Kind::Spacer);
  node->value = minLength;
  return View(node);
}

View Empty() { return View(); }

VStack::VStack(std::initializer_list<View> children) : VStack(listOf(children))
{
}

VStack::VStack(Alignment alignment, std::initializer_list<View> children)
    : VStack(listOf(children), alignment)
{
}

VStack::VStack(std::vector<View> children, Alignment alignment)
    : View(stack(Node::Kind::VStack, std::move(children), alignment))
{
}

HStack::HStack(std::initializer_list<View> children) : HStack(listOf(children))
{
}

HStack::HStack(Alignment alignment, std::initializer_list<View> children)
    : HStack(listOf(children), alignment)
{
}

HStack::HStack(std::vector<View> children, Alignment alignment)
    : View(stack(Node::Kind::HStack, std::move(children), alignment))
{
}

ZStack::ZStack(std::initializer_list<View> children) : ZStack(listOf(children))
{
}

ZStack::ZStack(std::vector<View> children)
    : View(stack(Node::Kind::ZStack, std::move(children), Alignment::Center))
{
}

// --- hashes ------------------------------------------------------------------

// Every field of a Node goes in its hash (functions as whether they're set):
// one added without going in it would leave a layer drawn as it was when only
// that field changed. Its size is checked where it's known (64-bit
// libstdc++), as a reminder.
#if defined(__GLIBCXX__) && UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(sizeof(Node) == 448,
              "Node changed: put the new field in Node::hash(), then update "
              "this size");
#endif

namespace
{

// A quick hash of a node's words (once a node, so cheap rather than strong:
// one multiply a word, then mixed well at the end).
class Words
{
public:
  void add(uint64_t word)
  {
    m_hash = (m_hash ^ word) * 0x9E3779B97F4A7C15ull;
    m_hash ^= m_hash >> 29;
  }
  // Two floats as one word.
  void add(float a, float b)
  {
    add(static_cast<uint64_t>(std::bit_cast<uint32_t>(a)) |
        static_cast<uint64_t>(std::bit_cast<uint32_t>(b)) << 32);
  }
  void add(Color color)
  {
    add(color.r, color.g);
    add(color.b, color.a);
  }
  // Text (empty, as most of a node's is, costs nothing to hash).
  void add(const std::string& text)
  {
    add(text.empty()
            ? uint64_t{0x51}
            : static_cast<uint64_t>(std::hash<std::string_view>()(text)));
  }
  uint64_t value() const { return mixed(0x5F5u, m_hash); }

private:
  uint64_t m_hash = 0xCBF29CE484222325ull;
};

} // namespace

uint64_t Node::hash() const
{
  if (m_count != 0)
    return m_hash;
  Words h;
  uint32_t total = 1;
  bool state = kind == Kind::Scroll || kind == Kind::Disclosure;
  h.add(static_cast<uint64_t>(kind) |
        static_cast<uint64_t>(children.size()) << 8 |
        static_cast<uint64_t>(meshIcon) << 40 |
        static_cast<uint64_t>(placeholder) << 41 |
        static_cast<uint64_t>(static_cast<bool>(action)) << 42 |
        static_cast<uint64_t>(static_cast<bool>(change)) << 43 |
        static_cast<uint64_t>(bound) << 44 |
        static_cast<uint64_t>(disabled) << 45 |
        static_cast<uint64_t>(font.has_value()) << 46 |
        static_cast<uint64_t>(color.has_value()) << 47 |
        static_cast<uint64_t>(stackAlignment) << 48 |
        static_cast<uint64_t>(alignment) << 52 |
        static_cast<uint64_t>(axes) << 56);
  for (const View& child : children)
  {
    const Node& c = *child.node();
    h.add(c.hash());
    total += c.count();
    state = state || c.keepsState();
  }
  h.add(text);
  h.add(value, low);
  h.add(high, step);
  h.add(spacing, opacity);
  h.add(insets.top, insets.leading);
  h.add(insets.bottom, insets.trailing);
  h.add(width, height);
  h.add(minWidth, maxWidth);
  h.add(minHeight, maxHeight);
  h.add(scrollOffset, fill.cornerRadius);
  h.add(fill.borderWidth, fill.shadow);
  h.add(fill.color);
  h.add(fill.borderColor);
  h.add(group);
  if (font)
  {
    h.add(font->file);
    h.add(font->size, 0.0f);
  }
  if (color)
    h.add(*color);
  h.add(id);
  h.add(help);
  m_hash = h.value();
  m_keepsState = state;
  m_count = total;
  return m_hash;
}

uint32_t Node::count() const
{
  hash();
  return m_count;
}

bool Node::keepsState() const
{
  hash();
  return m_keepsState;
}

} // namespace sfs::ui
