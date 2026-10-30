#include "engine/runtime/ui/ui.h"

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
  return std::make_shared<Node>(*view.node());
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

View::View() : m_node(make(Node::Kind::Empty)) {}

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

View View::align(Alignment alignment) const
{
  auto node = copy(*this);
  node->alignment = alignment;
  return View(node);
}

View View::spacing(float points) const
{
  auto node = copy(*this);
  node->spacing = points;
  return View(node);
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

View View::color(Color color) const
{
  auto node = copy(*this);
  node->color = color;
  return View(node);
}

View View::font(Font font) const
{
  auto node = copy(*this);
  node->font = std::move(font);
  return View(node);
}

View View::opacity(float alpha) const
{
  auto node = copy(*this);
  node->opacity *= alpha;
  return View(node);
}

View View::disabled(bool disabled) const
{
  auto node = copy(*this);
  node->disabled = node->disabled || disabled;
  return View(node);
}

View View::help(std::string text) const
{
  auto node = copy(*this);
  node->help = std::move(text);
  return View(node);
}

View View::id(std::string id) const
{
  auto node = copy(*this);
  node->id = std::move(id);
  return View(node);
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

} // namespace sfs::ui
