#include "../../testHarness.h"

#include <engine/runtime/ui/fontAtlas.h>
#include <engine/runtime/ui/layout.h>
#include <engine/runtime/ui/placeholder.h>
#include <engine/runtime/ui/preview.h>

#include <filesystem>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <variant>
#include <vector>

using namespace sfs;
using namespace sfs::ui;

namespace
{

// Text a stand-in measures: 10 pixels a character (at 20 pixels; in
// proportion otherwise), lines 20 high, a quad a character.
class FakeText : public TextShaper
{
public:
  float lineHeight(const std::string&, float pixels) override { return pixels; }
  float width(const std::string&, float pixels, std::string_view line) override
  {
    return static_cast<float>(line.size()) * pixels * 0.5f;
  }
  void draw(const std::string& font,
            float pixels,
            std::string_view line,
            glm::vec2 at,
            glm::vec4 color,
            std::vector<UiQuad>& out) override
  {
    UiQuad quad;
    quad.kind = UiQuad::Glyph;
    quad.rect = {at.x, at.y, width(font, pixels, line), pixels};
    quad.color = color;
    out.push_back(quad);
    drawn.emplace_back(line);
  }
  std::vector<std::string> drawn;
};

struct Laid
{
  std::vector<UiQuad> quads;
  std::vector<Hit> hits;
  FakeText text;
  View view; // what was laid out (its hits' sources are in it)
};

// Pictures a stand-in has: "crate.png" 64 x 32, every mesh an icon.
class FakeImages : public ImageSource
{
public:
  FakeImages()
  {
    file.size = {64, 32};
    icon.size = {128, 128};
  }
  const UiTexture* image(const std::string& source, bool mesh) override
  {
    if (mesh)
      return &icon;
    return source == "crate.png" ? &file : nullptr;
  }
  UiTexture file;
  UiTexture icon;
};

// A view of the game's own (a ViewStruct).
struct Badge
{
  std::string text;
  int count = 0;
  View body() const
  {
    return HStack{Text(text), Text(std::to_string(count))}.padding(4.0f);
  }
};

// A list of rows, filled in as sfs-codegen would (its Placeholder
// specialisations are written out below).
struct Row
{
  std::string what;
  View icon;
  int count = 3;
};
struct Board
{
  std::vector<Row> rows;
  std::string title = "Jobs"; // already set: kept
  View body() const
  {
    std::vector<View> lines{Text(title)};
    for (const Row& row : rows)
      lines.push_back(HStack{row.icon, Text(row.what)});
    return VStack(std::move(lines));
  }
};
// Can't be made without values.
struct Fixed
{
  explicit Fixed(int value) : value(value) {}
  int value;
  View body() const { return Text(std::to_string(value)); }
};

} // namespace

template <>
struct sfs::ui::Placeholder<Row>
{
  static void fill(Row& value, std::string_view, int item)
  {
    placeholder(value.what, "What", item);
    placeholder(value.icon, "Icon", item);
    placeholder(value.count, "Count", item);
  }
};
template <>
struct sfs::ui::Placeholder<Board>
{
  static void fill(Board& value, std::string_view, int item)
  {
    placeholder(value.rows, "Rows", item);
    placeholder(value.title, "Title", item);
  }
};
template <>
struct sfs::ui::Placeholder<Badge>
{
  static void fill(Badge& value, std::string_view, int item)
  {
    placeholder(value.text, "Text", item);
    placeholder(value.count, "Count", item);
  }
};

namespace
{

Laid lay(const View& view,
         glm::vec2 screen = {1000.0f, 500.0f},
         float scale = 1.0f,
         Pointer pointer = {},
         ImageSource* images = nullptr)
{
  Laid laid;
  laid.view = view;
  layout(view,
         screen,
         scale,
         Theme(),
         laid.text,
         pointer,
         laid.quads,
         laid.hits,
         images);
  return laid;
}

// The quad drawing a line of text.
const UiQuad* textQuad(const Laid& laid, const std::string& line)
{
  for (std::size_t i = 0, glyph = 0; i < laid.quads.size(); ++i)
    if (laid.quads[i].kind == UiQuad::Glyph)
    {
      if (glyph < laid.text.drawn.size() && laid.text.drawn[glyph] == line)
        return &laid.quads[i];
      ++glyph;
    }
  return nullptr;
}

bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

// Laid out keeping `state` (a scroll view's offset, a section's open), as
// UiModule does frame to frame, `seconds` after the last.
Laid layKept(const View& view,
             State& state,
             float seconds = 0.0f,
             Pointer pointer = {})
{
  Laid laid;
  laid.view = view;
  state.advance(seconds);
  layout(view,
         {1000.0f, 500.0f},
         1.0f,
         Theme(),
         laid.text,
         pointer,
         laid.quads,
         laid.hits,
         nullptr,
         nullptr,
         &state);
  return laid;
}

// Ten lines, "line0" to "line9", 20 high each, one under another.
View tenLines()
{
  std::vector<View> lines;
  for (int i = 0; i < 10; ++i)
    lines.push_back(Text("line" + std::to_string(i)));
  return VStack(std::move(lines), Alignment::Leading).spacing(0.0f);
}

// The node a hit's for.
const Node* sourceOf(const Laid& laid, const Hit& hit)
{
  return nodeAt(*laid.view.node(), hit.source);
}

// A hit clicked, or dragged to `at`, as UiModule does: given its node.
void click(const Laid& laid, const Hit* hit)
{
  if (const Node* source = sourceOf(laid, *hit))
    hit->action(*source);
}
void drag(const Laid& laid, const Hit* hit, glm::vec2 at)
{
  if (const Node* source = sourceOf(laid, *hit))
    hit->drag(*source, at);
}

// The first hit that can be clicked made by a view of `kind`.
const Hit* clickable(const Laid& laid, Node::Kind kind)
{
  for (const Hit& hit : laid.hits)
    if (hit.action && sourceOf(laid, hit) && sourceOf(laid, hit)->kind == kind)
      return &hit;
  return nullptr;
}

// A scroll bar's hit (pressed on to grab or page).
const Hit* barHit(const Laid& laid)
{
  for (const Hit& hit : laid.hits)
    if (hit.press)
      return &hit;
  return nullptr;
}

const Hit* scrollHit(const Laid& laid)
{
  const Hit* found = nullptr;
  for (const Hit& hit : laid.hits)
    if (hit.scroll)
      found = &hit; // the innermost: the last
  return found;
}

} // namespace

int main()
{
  TEST("a body should sit on the screen where its align() says")
  {
    const View hello = Text("hello"); // 50 x 20
    Laid centre = lay(hello);
    const UiQuad* middle = textQuad(centre, "hello");
    CHECK(middle && near(middle->rect.x, 475.0f) &&
          near(middle->rect.y, 240.0f));
    Laid corner = lay(hello.align(Alignment::BottomTrailing));
    const UiQuad* bottomRight = textQuad(corner, "hello");
    CHECK(bottomRight && near(bottomRight->rect.x, 950.0f) &&
          near(bottomRight->rect.y, 480.0f));
    // Twice the scale: twice the size, from the same corner.
    Laid big = lay(hello.align(Alignment::TopLeading), {1000.0f, 500.0f}, 2.0f);
    const UiQuad* scaled = textQuad(big, "hello");
    CHECK(scaled && near(scaled->rect.x, 0.0f) &&
          near(scaled->rect.z, 100.0f) && near(scaled->rect.w, 40.0f));
  }

  TEST("stacks should put children one after another, spaced and lined up")
  {
    Laid column = lay(VStack(Alignment::Leading, {Text("ab"), Text("abcd")})
                          .spacing(5.0f)
                          .align(Alignment::TopLeading));
    const UiQuad* first = textQuad(column, "ab");
    const UiQuad* second = textQuad(column, "abcd");
    CHECK(first && second);
    CHECK(near(first->rect.y, 0.0f) && near(second->rect.y, 25.0f)); // 20 + 5
    CHECK(near(first->rect.x, 0.0f) && near(second->rect.x, 0.0f));  // leading

    Laid row = lay(HStack{Text("ab"), Text("abcd")}.spacing(10.0f).align(
        Alignment::TopLeading));
    CHECK(near(textQuad(row, "abcd")->rect.x, 30.0f)); // 20 + 10

    // Centred in a column: the narrow one in the middle of the wide one.
    Laid centred =
        lay(VStack{Text("ab"), Text("abcd")}.align(Alignment::TopLeading));
    CHECK(near(textQuad(centred, "ab")->rect.x, 10.0f));
  }

  TEST("a spacer should push the rest of its stack to the far end")
  {
    // A row as wide as the screen: one at each end.
    Laid row = lay(
        HStack{Text("left"), Spacer(), Text("right")}.maxWidth(infinity).align(
            Alignment::TopLeading));
    CHECK(near(textQuad(row, "left")->rect.x, 0.0f));
    CHECK(near(textQuad(row, "right")->rect.x, 950.0f)); // 1000 - 50

    // Only along its stack: a row with a spacer, its background in the top
    // left of a ZStack, stretches across but stays its own height.
    Laid panel = lay(ZStack{HStack{Text("a"), Spacer(), Text("b")}
                                .background(Color::black())
                                .align(Alignment::TopLeading)});
    CHECK(!panel.quads.empty() && near(panel.quads.front().rect.z, 1000.0f) &&
          near(panel.quads.front().rect.w, 20.0f));
  }

  TEST("padding, frames and backgrounds should size what's inside them")
  {
    // Padded 10 all round: the background 70 x 40 round 50 x 20 text.
    Laid padded = lay(Text("hello")
                          .padding(10.0f)
                          .background(Color::black())
                          .align(Alignment::TopLeading));
    CHECK(!padded.quads.empty());
    const UiQuad& box = padded.quads.front();
    CHECK(box.kind == UiQuad::Box && near(box.rect.z, 70.0f) &&
          near(box.rect.w, 40.0f));
    CHECK(near(textQuad(padded, "hello")->rect.x, 10.0f));

    // A fixed frame, its content in the middle of it.
    Laid framed = lay(Text("hi")
                          .frame(100.0f, 50.0f)
                          .background(Color::white())
                          .align(Alignment::TopLeading));
    CHECK(near(framed.quads.front().rect.z, 100.0f) &&
          near(framed.quads.front().rect.w, 50.0f));
    CHECK(near(textQuad(framed, "hi")->rect.x, 40.0f) &&
          near(textQuad(framed, "hi")->rect.y, 15.0f));

    // As wide as there is.
    Laid wide = lay(Text("hi")
                        .maxWidth(infinity)
                        .background(Color::white())
                        .align(Alignment::TopLeading));
    CHECK(near(wide.quads.front().rect.z, 1000.0f));

    // A background's corners, border and shadow.
    Laid panel =
        lay(Text("x").padding().background(Panel()).cornerRadius(20.0f));
    bool rounded = false;
    for (const UiQuad& quad : panel.quads)
      rounded = rounded || (quad.kind == UiQuad::Box &&
                            near(quad.radius, 20.0f) && quad.border > 0.0f);
    CHECK(rounded);
  }

  TEST("buttons should be clickable where they are, look it when hovered, "
       "and not while disabled")
  {
    int clicks = 0;
    const View ui =
        HStack{
            Button("go", [&] { ++clicks; }),
            Button("no", [&] { ++clicks; }).disabled(),
        }
            .align(Alignment::TopLeading);
    Laid laid = lay(ui);
    std::vector<const Hit*> buttons;
    for (const Hit& hit : laid.hits)
      if (hit.action)
        buttons.push_back(&hit);
    CHECK(buttons.size() == 2u);
    if (buttons.size() == 2u)
    {
      CHECK(buttons[0]->enabled && !buttons[1]->enabled);
      CHECK(buttons[0]->id != buttons[1]->id);
      CHECK(buttons[1]->rect.x > buttons[0]->rect.x + buttons[0]->rect.width);
      click(laid, buttons[0]);
      CHECK(clicks == 1);

      // Hovered: drawn in its hovered colour.
      Pointer pointer;
      pointer.hovered = buttons[0]->id;
      Laid hovered = lay(ui, {1000.0f, 500.0f}, 1.0f, pointer);
      const glm::vec4 hoverColor(Theme().buttonHovered.r,
                                 Theme().buttonHovered.g,
                                 Theme().buttonHovered.b,
                                 1.0f);
      bool found = false;
      for (const UiQuad& quad : hovered.quads)
        found = found ||
                (quad.kind == UiQuad::Box && near(quad.color.r, hoverColor.r) &&
                 near(quad.color.g, hoverColor.g) && near(quad.color.a, 1.0f));
      CHECK(found);
    }
  }

  TEST("ForEach should make a stack's children; ids keep them apart")
  {
    const std::vector<std::string> names = {"one", "two", "three"};
    Laid laid = lay(VStack{
        ForEach(names,
                [](const std::string& name) {
                  return Button(name, [] {}).id(name);
                })}.align(Alignment::TopLeading));
    int buttons = 0;
    bool named = false;
    for (const Hit& hit : laid.hits)
      if (hit.action)
      {
        ++buttons;
        named = named || sourceOf(laid, hit)->id == "two";
      }
    CHECK(buttons == 3 && named);
    CHECK(laid.text.drawn.size() == 3u);
  }

  TEST("colour, font and opacity should pass down to what's inside")
  {
    Laid laid = lay(VStack{Text("a"), Text("b").color(Color::black())}
                        .color(Color::rgb(255, 0, 0))
                        .font(Font::title())
                        .opacity(0.5f));
    const UiQuad* a = textQuad(laid, "a");
    const UiQuad* b = textQuad(laid, "b");
    CHECK(a && b);
    CHECK(near(a->color.r, 1.0f) && near(a->color.g, 0.0f) &&
          near(a->color.a, 0.5f));
    CHECK(near(b->color.r, 0.0f)); // its own
    CHECK(near(a->rect.w, 32.0f)); // the title's size
  }

  TEST("help should make a hover area; progress bars should fill part way")
  {
    Laid laid =
        lay(ProgressBar(0.25f).help("a quarter").align(Alignment::TopLeading));
    bool help = false;
    for (const Hit& hit : laid.hits)
      help = help || hit.help == "a quarter";
    CHECK(help);
    CHECK(laid.quads.size() == 2u); // the track, the fill
    if (laid.quads.size() == 2u)
      CHECK(near(laid.quads[1].rect.z, laid.quads[0].rect.z * 0.25f));
  }

  TEST("images should show at their own size, shrink to fit a frame keeping "
       "their shape, and mesh icons at 32 points")
  {
    FakeImages images;
    const glm::vec2 screen(1000.0f, 500.0f);
    Laid own = lay(Image("crate.png").align(Alignment::TopLeading),
                   screen,
                   1.0f,
                   {},
                   &images);
    CHECK(own.quads.size() == 1u);
    if (own.quads.size() == 1u)
    {
      CHECK(own.quads[0].kind == UiQuad::Image &&
            own.quads[0].texture == &images.file);
      CHECK(near(own.quads[0].rect.z, 64.0f) &&
            near(own.quads[0].rect.w, 32.0f));
    }
    // Framed 32 x 32: 32 x 16, in the frame's middle (the frame placed by
    // its stack: a frame's own align() lines its content up too).
    Laid framed = lay(VStack{Image("crate.png").frame(32.0f, 32.0f)}.align(
                          Alignment::TopLeading),
                      screen,
                      1.0f,
                      {},
                      &images);
    CHECK(framed.quads.size() == 1u && near(framed.quads[0].rect.z, 32.0f) &&
          near(framed.quads[0].rect.w, 16.0f) &&
          near(framed.quads[0].rect.y, 8.0f));
    // A mesh's icon, at twice the scale; one in a button beside its label.
    Laid icon = lay(Image::mesh("farm:wheat").align(Alignment::TopLeading),
                    screen,
                    2.0f,
                    {},
                    &images);
    CHECK(icon.quads.size() == 1u && near(icon.quads[0].rect.z, 64.0f));
    Laid button = lay(
        Button(
            HStack{Image::mesh("farm:wheat").frame(20.0f, 20.0f), Text("Sow")},
            [] {}),
        screen,
        1.0f,
        {},
        &images);
    int pictures = 0;
    for (const UiQuad& quad : button.quads)
      pictures += quad.kind == UiQuad::Image;
    CHECK(pictures == 1);
    // None to show: nothing drawn, no room taken (but the stack's spacing).
    Laid missing =
        lay(HStack{Image("nope.png"), Text("x")}.align(Alignment::TopLeading),
            screen,
            1.0f,
            {},
            &images);
    CHECK(missing.quads.size() == 1u && near(missing.quads[0].rect.x, 8.0f));
  }

  TEST("a view struct should be a view: its body(), in stacks and as a "
       "modifier's subject")
  {
    Laid laid = lay(VStack(Alignment::Leading,
                           {Badge{.text = "New", .count = 3}, Text("Wheat")})
                        .align(Alignment::TopLeading));
    const UiQuad* label = textQuad(laid, "New");
    const UiQuad* count = textQuad(laid, "3");
    CHECK(label && count && near(label->rect.x, 4.0f) &&
          near(count->rect.x, 4.0f + 30.0f + 8.0f));
    Laid modified = lay(
        View(Badge{.text = "A"}).padding(10.0f).align(Alignment::TopLeading));
    const UiQuad* a = textQuad(modified, "A");
    CHECK(a && near(a->rect.x, 14.0f));
  }

  TEST("previews should register while they exist, knowing their file; fit "
       "should give a view's own size, or the offer where it stretches")
  {
    const std::size_t before = previews().size();
    {
      const Preview preview("Badge", [] { return Badge{.text = "Hi"}; });
      CHECK(previews().size() == before + 1 && previews().back() == &preview);
      CHECK(preview.file().ends_with("uiLayoutTests.cpp"));
      CHECK(preview.name() == "Badge" &&
            textQuad(lay(preview.make().align(Alignment::TopLeading)), "Hi"));
    }
    CHECK(previews().size() == before);

    FakeText text;
    const glm::vec2 own = fit(
        Badge{.text = "Hi", .count = 7}, {500.0f, 300.0f}, 1.0f, Theme(), text);
    CHECK(near(own.x, 4.0f + 20.0f + 8.0f + 10.0f + 4.0f) && own.y < 100.0f);
    const glm::vec2 wide = fit(
        Text("Hi").maxWidth(infinity), {500.0f, 300.0f}, 1.0f, Theme(), text);
    CHECK(near(wide.x, 500.0f) && wide.y < 100.0f);
  }

  TEST("a font atlas should measure and draw text from a real font, baking "
       "glyphs as they're first needed")
  {
    // The engine's own font, beside this file's source.
    const std::filesystem::path font =
        std::filesystem::path(__FILE__).parent_path() /
        "../../../include/assets/fonts/m6x11.ttf";
    FontAtlas atlas(
        [&](const std::string&) { return font.string(); }, "fallback.ttf");
    CHECK(atlas.lineHeight("m6x11", 22.0f) > 10.0f);
    const float ab = atlas.width("m6x11", 22.0f, "ab");
    CHECK(ab > 0.0f);
    CHECK(atlas.width("m6x11", 22.0f, "abab") > ab * 1.9f);
    CHECK(atlas.width("m6x11", 22.0f, "") == 0.0f);

    std::vector<UiQuad> quads;
    atlas.draw("m6x11", 22.0f, "a b", {10.0f, 20.0f}, glm::vec4(1.0f), quads);
    CHECK(quads.size() == 2u); // no quad for the space
    if (quads.size() == 2u)
    {
      CHECK(quads[0].kind == UiQuad::Glyph && quads[0].texture != nullptr);
      CHECK(quads[0].texture ==
            quads[1].texture); // one texture a font and size
      CHECK(quads[1].rect.x > quads[0].rect.x);
      CHECK(quads[0].texture->revision > 0 &&
            !quads[0].texture->pixels.empty());
      // The glyph is there: some of its pixels covered.
      const UiTexture& texture = *quads[0].texture;
      bool covered = false;
      for (std::size_t i = 3; i < texture.pixels.size(); i += 4)
        covered = covered || texture.pixels[i] > 200;
      CHECK(covered);
    }
    // Another size: its own texture.
    std::vector<UiQuad> bigger;
    atlas.draw("m6x11", 44.0f, "a", {0.0f, 0.0f}, glm::vec4(1.0f), bigger);
    CHECK(bigger.size() == 1u && !quads.empty() &&
          bigger[0].texture != quads[0].texture);
  }

  TEST("a font atlas should let go of faces unused through a frame, past its "
       "limit, keeping the ones in use")
  {
    const std::filesystem::path font =
        std::filesystem::path(__FILE__).parent_path() /
        "../../../include/assets/fonts/m6x11.ttf";
    FontAtlas atlas(
        [&](const std::string&) { return font.string(); }, "fallback.ttf");
    // UI scaled smoothly: a new size every frame.
    std::vector<UiQuad> quads;
    for (int size = 10; size < 10 + 3 * int(FontAtlas::kMostFaces); ++size)
    {
      atlas.beginFrame();
      quads.clear();
      atlas.draw(
          "m6x11", float(size), "a", {0.0f, 0.0f}, glm::vec4(1.0f), quads);
    }
    CHECK(atlas.faceCount() <= FontAtlas::kMostFaces + 1);
    // This frame's text is still drawable: its texture wasn't dropped.
    CHECK(quads.size() == 1u && quads[0].texture != nullptr &&
          !quads[0].texture->pixels.empty());
    atlas.beginFrame();
    CHECK(quads[0].texture->size.x > 0);
  }

  TEST("UTF-8 should come apart into its codepoints")
  {
    std::u32string out;
    forEachCodepoint(
        "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8C\xBE", [&](char32_t c) { out += c; });
    CHECK(out == U"a\u00E9\u20AC\U0001F33E"); // a, é, €, 🌾
  }

  TEST("a view struct's placeholders should fill text with field names, "
       "lists with two numbered items and pictures with boxes, keeping what's "
       "set")
  {
    Laid badge = lay(placeholderView<Badge>().align(Alignment::TopLeading));
    CHECK(textQuad(badge, "Text") && textQuad(badge, "0"));

    Board board;
    placeholder(board, "", 0);
    CHECK(board.rows.size() == 2u && board.rows[0].what == "What 1" &&
          board.rows[1].what == "What 2" && board.rows[0].count == 3 &&
          board.title == "Jobs");
    CHECK(board.rows[0].icon.node()->kind == Node::Kind::Image &&
          board.rows[0].icon.node()->placeholder);

    Laid laid = lay(placeholderView<Board>().align(Alignment::TopLeading));
    CHECK(textQuad(laid, "Jobs") && textQuad(laid, "What 2"));
    // The pictures: boxes 32 points square, with nothing to look up.
    int boxes = 0;
    for (const UiQuad& quad : laid.quads)
      boxes += quad.kind != UiQuad::Glyph && near(quad.rect.z, 32.0f) &&
               near(quad.rect.w, 32.0f);
    CHECK(boxes == 2);

    // An optional gets one; a variant fills what it holds (the first, made
    // so).
    std::optional<std::variant<Row, Badge>> either;
    placeholder(either, "Either", 0);
    CHECK(either && std::holds_alternative<Row>(*either) &&
          std::get<Row>(*either).what == "What");

    CHECK(textQuad(lay(placeholderView<Fixed>().align(Alignment::TopLeading)),
                   "Can't be made without values: give it a preview "
                   "(sfs::ui::Preview)"));
  }

  TEST("preview styles should register while they exist, knowing their "
       "file, and dress a view")
  {
    const std::size_t before = previewStyles().size();
    {
      const PreviewStyle style([](View view)
                               { return view.color(Color::rgb(255, 0, 0)); });
      CHECK(previewStyles().size() == before + 1 &&
            previewStyles().back() == &style);
      CHECK(style.file().ends_with("uiLayoutTests.cpp"));
      const View dressed = style.apply(Text("Hi"));
      CHECK(dressed.node()->color && dressed.node()->color->r == 1.0f);
    }
    CHECK(previewStyles().size() == before);
  }

  TEST("a placeholder picture should shrink to fit its frame, keeping square")
  {
    Laid laid = lay(
        Image::placeholder().frame(40.0f, 20.0f).align(Alignment::TopLeading));
    CHECK(laid.quads.size() == 1u && near(laid.quads[0].rect.z, 20.0f) &&
          near(laid.quads[0].rect.w, 20.0f));
  }

  TEST("a slider should follow a drag along its track, in its range and "
       "steps, and a toggle flip when clicked")
  {
    float value = 5.0f;
    bool on = false;
    const View ui =
        VStack(Alignment::Leading,
               {
                   Slider(
                       value, [&](float to) { value = to; }, 0.0f, 10.0f, 1.0f),
                   Toggle("rain", on, [&](bool to) { on = to; }),
               })
            .align(Alignment::TopLeading);
    Laid laid = lay(ui);
    const Hit* slider = nullptr;
    const Hit* toggle = nullptr;
    for (const Hit& hit : laid.hits)
    {
      if (hit.drag)
        slider = &hit;
      if (hit.action)
        toggle = &hit;
    }
    CHECK(slider && toggle);
    if (slider && toggle)
    {
      CHECK(near(slider->rect.width, 160.0f) &&
            near(slider->rect.height, 22.0f));
      // The knob's centre runs 8 points in from either end: at the far end,
      // the top of the range; beyond it, still the top; near a third of the
      // way, the nearest step.
      const float middle = slider->rect.y + 11.0f;
      drag(laid, slider, {slider->rect.x + 152.0f, middle});
      CHECK(near(value, 10.0f));
      drag(laid, slider, {slider->rect.x + 400.0f, middle});
      CHECK(near(value, 10.0f));
      drag(laid, slider, {slider->rect.x + 8.0f + 144.0f * 0.33f, middle});
      CHECK(near(value, 3.0f));
      drag(laid, slider, {slider->rect.x - 50.0f, middle});
      CHECK(near(value, 0.0f));

      // The toggle: its switch and label, clicked on.
      CHECK(toggle->rect.y >= slider->rect.y + slider->rect.height);
      CHECK(near(toggle->rect.width, 44.0f + 40.0f));
      click(laid, toggle);
      CHECK(on);
      CHECK(laid.text.drawn.size() == 1u && laid.text.drawn[0] == "rain");
    }

    // Disabled: neither does anything.
    Laid off = lay(ui.disabled());
    for (const Hit& hit : off.hits)
      CHECK(!hit.drag && !hit.action);
  }

  TEST("a scroll view should be its content's size up to its room, showing "
       "and laying out only what's inside it, cut at its edges")
  {
    State state;
    const View ui =
        ScrollView(tenLines()).maxHeight(100.0f).align(Alignment::TopLeading);
    Laid laid = layKept(ui, state);
    const Hit* view = scrollHit(laid);
    CHECK(view && near(view->rect.height, 100.0f) &&
          near(view->rect.width, 50.0f));
    // Five lines show; the rest aren't drawn at all.
    CHECK(laid.text.drawn.size() == 5u);
    const UiQuad* first = textQuad(laid, "line0");
    CHECK(first && near(first->clip.y, 0.0f) && near(first->clip.w, 100.0f));
    CHECK(!textQuad(laid, "line5"));
    // A bar down its edge, as it's taller inside.
    CHECK(barHit(laid) != nullptr);

    // Shorter content: its own size, and no bar.
    Laid shortOne =
        layKept(ScrollView(VStack{Text("a"), Text("b")}.spacing(0.0f))
                    .maxHeight(100.0f)
                    .align(Alignment::TopLeading),
                state);
    const Hit* small = scrollHit(shortOne);
    CHECK(small && near(small->rect.height, 40.0f));
    CHECK(barHit(shortOne) == nullptr);

    // In a stack short of room, it gives its room up first.
    State stacked;
    Laid fitted = layKept(VStack{Text("title"), ScrollView(tenLines())}
                              .frame(0.0f, 120.0f)
                              .align(Alignment::TopLeading),
                          stacked);
    const Hit* rest = scrollHit(fitted);
    CHECK(rest && near(rest->rect.y, 28.0f) && near(rest->rect.height, 92.0f));

    // Where it starts scrolled to, for a first frame.
    State fresh;
    Laid started = layKept(ScrollView(tenLines())
                               .scrollOffset(40.0f)
                               .maxHeight(100.0f)
                               .align(Alignment::TopLeading),
                           fresh);
    const UiQuad* third = textQuad(started, "line2");
    CHECK(third && near(third->rect.y, 0.0f));
    CHECK(!textQuad(started, "line0") && !textQuad(started, "line1"));
  }

  TEST("the wheel should scroll a scroll view, eased, within its content; "
       "its thumb should drag it and its track page it")
  {
    State state;
    const View ui =
        ScrollView(tenLines()).maxHeight(100.0f).align(Alignment::TopLeading);
    Laid laid = layKept(ui, state);
    const Hit* view = scrollHit(laid);
    CHECK(view && view->scroll);
    if (!view || !view->scroll)
      break;
    CHECK(!view->scroll({0.0f, -10.0f})); // at the top already
    const uint64_t before = state.changes;
    CHECK(view->scroll({0.0f, 50.0f}));
    CHECK(state.changes > before); // the layer's changed
    // Eased: part way after a moment, there a while later.
    Laid moving = layKept(ui, state, 0.02f);
    const UiQuad* line3 = textQuad(moving, "line3");
    CHECK(line3 && line3->rect.y < 60.0f && line3->rect.y > 10.0f);
    Laid there = layKept(ui, state, 1.0f);
    line3 = textQuad(there, "line3");
    CHECK(line3 && near(line3->rect.y, 10.0f));
    // Partly showing: drawn, cut at the top.
    const UiQuad* line2 = textQuad(there, "line2");
    CHECK(line2 && near(line2->rect.y, -10.0f) && near(line2->clip.y, 0.0f));

    // No further than the end; then it can't go on.
    const Hit* again = scrollHit(there);
    CHECK(again && again->scroll({0.0f, 500.0f}));
    Laid end = layKept(ui, state, 1.0f);
    CHECK(textQuad(end, "line9") &&
          near(textQuad(end, "line9")->rect.y, 80.0f));
    CHECK(near(state.scrolls.begin()->second.offset.y, 100.0f));
    const Hit* atEnd = scrollHit(end);
    CHECK(atEnd && !atEnd->scroll({0.0f, 10.0f}));

    // The thumb: half the track (it shows half), at its far end now.
    const glm::vec2 thumb = scrollThumb(100.0f, 100.0f, 200.0f, 100.0f, 20.0f);
    CHECK(near(thumb.x, 50.0f) && near(thumb.y, 50.0f));
    CHECK(near(scrollThumb(100.0f, 100.0f, 200.0f, 0.0f, 20.0f).x, 0.0f));
    CHECK(near(scrollThumb(100.0f, 100.0f, 1000.0f, 0.0f, 20.0f).y, 20.0f));
    CHECK(near(scrollThumb(100.0f, 100.0f, 80.0f, 0.0f, 20.0f).y, 100.0f));
    CHECK(scrolled({0.0f, 90.0f}, {0.0f, 50.0f}, {0.0f, 100.0f}) ==
          glm::vec2(0.0f, 100.0f));

    // Pressed on the track above the thumb: a page up.
    const Hit* bar = barHit(end);
    CHECK(bar && bar->press && bar->drag);
    if (bar && bar->press && bar->drag)
    {
      bar->press({bar->rect.x + 2.0f, bar->rect.y + 5.0f});
      CHECK(state.scrolls.begin()->second.target.y < 15.0f);
      // On the thumb, then dragged: it follows at once (no easing).
      Laid paged = layKept(ui, state, 1.0f);
      const Hit* top = barHit(paged);
      CHECK(top != nullptr);
      if (top)
      {
        // The track's 94 long (3 in from each end), the thumb 47 at the top.
        top->press({top->rect.x + 2.0f, top->rect.y + 10.0f});
        drag(paged, top, {top->rect.x + 2.0f, top->rect.y + 10.0f + 47.0f});
        CHECK(near(state.scrolls.begin()->second.offset.y, 100.0f));
        // Half a thumb's length down from where it was: 0.6 of the way.
        drag(paged, top, {top->rect.x + 2.0f, top->rect.y + 10.0f + 23.5f});
        CHECK(near(state.scrolls.begin()->second.offset.y, 60.0f));
      }
    }
  }

  TEST("what's scrolled out of a scroll view shouldn't be clickable, and "
       "what's partly in only where it shows")
  {
    State state;
    std::vector<View> buttons;
    for (int i = 0; i < 6; ++i)
      buttons.push_back(Button("b" + std::to_string(i), [] {}));
    const View ui = ScrollView(VStack(std::move(buttons)).spacing(0.0f))
                        .scrollOffset(17.0f)
                        .maxHeight(100.0f)
                        .align(Alignment::TopLeading);
    Laid laid = layKept(ui, state);
    std::vector<const Hit*> clickable;
    for (const Hit& hit : laid.hits)
      if (hit.action)
        clickable.push_back(&hit);
    // Buttons 34 high, scrolled 17: half the first, the second and third,
    // most of the fourth.
    CHECK(clickable.size() == 4u);
    if (!clickable.empty())
    {
      CHECK(near(clickable[0]->rect.y, 0.0f) &&
            near(clickable[0]->rect.height, 17.0f));
      for (const Hit* hit : clickable)
        CHECK(hit->rect.y >= 0.0f && hit->rect.y + hit->rect.height <= 100.01f);
    }
  }

  TEST("the wheel should go to the innermost scroll view that can still "
       "scroll that way")
  {
    State state;
    const View inner = ScrollView(tenLines()).id("inner").frame(0.0f, 50.0f);
    const View ui = ScrollView(VStack{inner, tenLines()}.spacing(0.0f))
                        .maxHeight(100.0f)
                        .align(Alignment::TopLeading);
    Laid laid = layKept(ui, state);
    const Hit* innermost = scrollHit(laid);
    CHECK(innermost && innermost->rect.height <= 50.01f);
    CHECK(state.scrolls.count(idKey("inner")) == 1u);
    // The inner one takes it till its end; then it's the outer's turn.
    CHECK(innermost && innermost->scroll({0.0f, 150.0f}));
    CHECK(innermost && !innermost->scroll({0.0f, 10.0f}));
    const Hit* outer = nullptr;
    for (const Hit& hit : laid.hits)
      if (hit.scroll && hit.rect.height > 60.0f)
        outer = &hit;
    CHECK(outer && outer->scroll({0.0f, 10.0f}));
  }

  TEST("a disclosure group should show its content only while open, its "
       "header opening and closing it, eased")
  {
    State state;
    const View ui = VStack(Alignment::Leading,
                           {
                               DisclosureGroup("Weather", Text("rain")),
                               Text("after"),
                           })
                        .spacing(0.0f)
                        .frame(300.0f)
                        .align(Alignment::TopLeading);
    Laid closed = layKept(ui, state);
    // Closed: its header (26 high: the label and 6) and not its content.
    CHECK(textQuad(closed, "Weather") && !textQuad(closed, "rain"));
    const UiQuad* after = textQuad(closed, "after");
    CHECK(after && near(after->rect.y, 26.0f));
    const Hit* header = clickable(closed, Node::Kind::Disclosure);
    CHECK(header && header->action && near(header->rect.width, 300.0f));
    bool chevron = false;
    for (const UiQuad& quad : closed.quads)
      chevron =
          chevron || (quad.kind == UiQuad::Chevron && near(quad.radius, 0.0f));
    CHECK(chevron);
    if (!header || !header->action)
      break;

    // Clicked: opening, part way, its content cut where it's got to.
    click(closed, header);
    Laid opening = layKept(ui, state, 0.1f);
    after = textQuad(opening, "after");
    CHECK(after && after->rect.y > 26.0f && after->rect.y < 52.0f);
    const UiQuad* rain = textQuad(opening, "rain");
    CHECK(rain && rain->clip.w < 52.0f);
    // Open: the header, a 6 gap and its content.
    Laid open = layKept(ui, state, 1.0f);
    after = textQuad(open, "after");
    CHECK(after && near(after->rect.y, 52.0f));
    rain = textQuad(open, "rain");
    CHECK(rain && near(rain->rect.y, 32.0f) && rain->clip.w > 1.0e8f);

    // Closed again by its header, and remembered while it's laid out.
    const Hit* again = clickable(open, Node::Kind::Disclosure);
    CHECK(again && again->action);
    if (again && again->action)
      click(open, again);
    Laid shut = layKept(ui, state, 1.0f);
    CHECK(!textQuad(shut, "rain"));
    Laid still = layKept(ui, state, 1.0f);
    CHECK(!textQuad(still, "rain"));

    // Open at first, without state (as a preview lays it out).
    Laid given = lay(DisclosureGroup("Soil", Text("wet"), true)
                         .align(Alignment::TopLeading));
    CHECK(textQuad(given, "wet") != nullptr);
  }

  TEST("a disclosure group's accessory should be clickable over its header; "
       "a bound one asks the game; an accordion keeps one open")
  {
    State state;
    int resets = 0;
    bool shown = false;
    const View ui =
        VStack(Alignment::Leading,
               {
                   DisclosureGroup("A", Text("a1"), true)
                       .accessory(Button("Reset", [&] { ++resets; }))
                       .accordion("g"),
                   DisclosureGroup("B", Text("b1")).accordion("g"),
                   DisclosureGroup(
                       "C", shown, [&](bool to) { shown = to; }, Text("c1")),
               })
            .frame(300.0f)
            .align(Alignment::TopLeading);
    Laid laid = layKept(ui, state);
    CHECK(textQuad(laid, "a1") && !textQuad(laid, "b1") &&
          !textQuad(laid, "c1"));
    // The accessory's in front of its header: it's what a click there hits.
    const Hit* reset = nullptr;
    for (const Hit& hit : laid.hits)
      if (hit.action && hit.rect.contains({290.0f, 10.0f}))
        reset = &hit;
    CHECK(reset && sourceOf(laid, *reset)->kind == Node::Kind::Button);
    if (reset)
      click(laid, reset);
    CHECK(resets == 1);

    // B opened: A closes.
    std::vector<const Hit*> headers;
    for (const Hit& hit : laid.hits)
      if (hit.action && sourceOf(laid, hit)->kind == Node::Kind::Disclosure)
        headers.push_back(&hit);
    CHECK(headers.size() == 3u);
    if (headers.size() == 3u)
    {
      click(laid, headers[1]);
      Laid swapped = layKept(ui, state, 1.0f);
      CHECK(!textQuad(swapped, "a1") && textQuad(swapped, "b1"));
      // C asks the game, and opens when it says so.
      click(laid, headers[2]);
      CHECK(shown);
    }
  }

  TEST("a node's hash should be the same for the same tree, whatever its "
       "functions, and change with anything it shows; modifiers on a view "
       "being built change it in place, on a kept one a copy")
  {
    const auto make = [](std::string text, std::function<void()> action)
    {
      return VStack(Alignment::Leading,
                    {Text(std::move(text)).font(Font::caption()),
                     Button("Go", std::move(action)).help("go")})
          .padding(8.0f)
          .background(Panel());
    };
    int a = 0, b = 0;
    const View one = make("6:40 pm", [&] { ++a; });
    const View two = make("6:40 pm", [&] { ++b; });
    CHECK(one.node()->hash() == two.node()->hash());
    CHECK(one.node()->hash() != make("6:41 pm", [] {}).node()->hash());
    CHECK(one.node()->hash() != make("6:40 pm", nullptr).node()->hash());
    // The background, its padding, the stack, the text, the button and its
    // label: modifiers on a view being built don't add nodes.
    CHECK(one.node()->count() == 6u);
    CHECK(Text("a").color(Color::black()).node()->hash() !=
          Text("a").node()->hash());
    // In place: one node, not a chain of copies.
    const View built = Text("x").color(Color::black()).font(Font::title());
    CHECK(built.node()->kind == Node::Kind::Text && built.node()->color &&
          built.node()->font);
    // A kept view isn't changed by a modifier on it.
    const View kept = Text("y");
    const View tinted = kept.color(Color::black());
    CHECK(!kept.node()->color && tinted.node()->color);
    CHECK(kept.node()->hash() != tinted.node()->hash());
    // A hit's source: its node, found by its place in the tree.
    const Node* label = nodeAt(*one.node(), 5);
    CHECK(label && label->kind == Node::Kind::Text && label->text == "Go");
    CHECK(nodeAt(*one.node(), 6) == nullptr);
  }

  // A layer kept from frame to frame, as UiModule keeps one.
  struct Kept
  {
    State state;
    Cache cache;
    FakeText text;
    View view;
    const Cache::Stats&
    lay(const View& next, Pointer pointer = {}, float seconds = 0.0f)
    {
      view = next;
      text.drawn.clear();
      state.advance(seconds);
      layout(
          next, {1000.0f, 500.0f}, 1.0f, Theme(), text, pointer, state, cache);
      return cache.stats();
    }
    const Hit* clickable(Node::Kind kind, int which = 0) const
    {
      for (const Hit& hit : cache.hits())
        if (hit.action && nodeAt(*view.node(), hit.source)->kind == kind &&
            which-- == 0)
          return &hit;
      return nullptr;
    }
  };
  // The same quads (where, what colour, cut where), as laid out from scratch.
  const auto sameAsFresh = [](const Kept& kept, Pointer pointer = {})
  {
    Laid fresh = lay(kept.view, {1000.0f, 500.0f}, 1.0f, pointer);
    const std::vector<UiQuad>& quads = *kept.cache.quads();
    if (quads.size() != fresh.quads.size() ||
        kept.cache.hits().size() != fresh.hits.size())
      return false;
    for (std::size_t i = 0; i < quads.size(); ++i)
      if (quads[i].rect != fresh.quads[i].rect ||
          quads[i].color != fresh.quads[i].color ||
          quads[i].clip != fresh.quads[i].clip)
        return false;
    for (std::size_t i = 0; i < fresh.hits.size(); ++i)
      if (kept.cache.hits()[i].id != fresh.hits[i].id ||
          kept.cache.hits()[i].source != fresh.hits[i].source ||
          !near(kept.cache.hits()[i].rect.x, fresh.hits[i].rect.x) ||
          !near(kept.cache.hits()[i].rect.y, fresh.hits[i].rect.y))
        return false;
    return true;
  };
  // A HUD: a clock panel and a job board.
  const auto hud = [](std::string time, std::string job, int* clicks)
  {
    return HStack(Alignment::Top,
                  {
                      VStack(Alignment::Leading,
                             {Text(std::move(time)),
                              Button("Pause", [clicks] { ++*clicks; })})
                          .padding(8.0f)
                          .background(Panel()),
                      Spacer(),
                      VStack(Alignment::Leading,
                             {Text(std::move(job)), Button("Eye", [] {})})
                          .padding(8.0f)
                          .background(Panel()),
                  })
        .align(Alignment::TopLeading);
  };

  TEST("a kept layer the same as last frame shouldn't be laid out or drawn "
       "again; one whose text changed only where it did")
  {
    Kept kept;
    int clicks = 0;
    const Cache::Stats& first = kept.lay(hud("6:40 pm", "Plough", &clicks));
    CHECK(!first.kept && first.placed > 0 && first.reused == 0);
    CHECK(kept.text.drawn.size() == 4u);
    CHECK(sameAsFresh(kept));
    const auto quads = kept.cache.quads();

    // Made again, the same: nothing laid out, nothing drawn, nothing copied.
    const Cache::Stats& same = kept.lay(hud("6:40 pm", "Plough", &clicks));
    CHECK(same.kept && same.placed == 0);
    CHECK(kept.text.drawn.empty());
    CHECK(kept.cache.quads() == quads);

    // The clock's text changed (as wide): only it drawn again, the rest kept.
    const Cache::Stats& tick = kept.lay(hud("6:41 pm", "Plough", &clicks));
    CHECK(!tick.kept && tick.reused > 0);
    CHECK(kept.text.drawn.size() == 1u &&
          (kept.text.drawn.empty() || kept.text.drawn[0] == "6:41 pm"));
    CHECK(sameAsFresh(kept));

    // A wider text moves what's after it: drawn again, still right.
    kept.lay(hud("12:41 pm", "Plough", &clicks));
    CHECK(sameAsFresh(kept));
    // And the board's job changed (as wide): the clock kept.
    kept.lay(hud("12:41 pm", "Harrow", &clicks));
    CHECK(kept.text.drawn.size() == 1u &&
          (kept.text.drawn.empty() || kept.text.drawn[0] == "Harrow"));
    CHECK(sameAsFresh(kept));

    // Its button calls the newest tree's function, though its drawing's kept.
    int newer = 0;
    kept.lay(hud("12:41 pm", "Harrow", &newer));
    CHECK(kept.cache.stats().kept);
    const Hit* pause = kept.clickable(Node::Kind::Button);
    CHECK(pause != nullptr);
    if (pause)
      pause->action(*nodeAt(*kept.view.node(), pause->source));
    CHECK(newer == 1 && clicks == 0);
  }

  TEST("a kept layer should be drawn again when its State changes, and when "
       "the pointer comes over a button, only that button")
  {
    Kept kept;
    int clicks = 0;
    kept.lay(hud("6:40 pm", "Plough", &clicks));
    const Hit* eye = kept.clickable(Node::Kind::Button, 1);
    CHECK(eye != nullptr);
    if (!eye)
      break;

    // Hovered: the eye button drawn again (its look), nothing else (not even
    // its label, which is where it was).
    Pointer pointer;
    pointer.hovered = eye->id;
    const Cache::Stats& over =
        kept.lay(hud("6:40 pm", "Plough", &clicks), pointer);
    CHECK(!over.kept && over.reused > 0 && kept.text.drawn.empty());
    CHECK(sameAsFresh(kept, pointer));
    CHECK(!sameAsFresh(kept)); // (its hovered colour)
    // Still over it: kept. The pointer over the panel, not a button: the
    // button drawn again as it was.
    CHECK(kept.lay(hud("6:40 pm", "Plough", &clicks), pointer).kept);
    pointer.hovered = 12345;
    const Cache::Stats& off =
        kept.lay(hud("6:40 pm", "Plough", &clicks), pointer);
    CHECK(!off.kept && kept.text.drawn.empty());
    CHECK(sameAsFresh(kept));
    CHECK(kept.lay(hud("6:40 pm", "Plough", &clicks), pointer).kept);

    // A scroll view scrolled: drawn again as it eases, then kept.
    Kept scroller;
    const View list =
        ScrollView(tenLines()).maxHeight(100.0f).align(Alignment::TopLeading);
    scroller.lay(list);
    CHECK(scroller.lay(list).kept);
    const Hit* view = nullptr;
    for (const Hit& hit : scroller.cache.hits())
      if (hit.scroll)
        view = &hit;
    CHECK(view && view->scroll({0.0f, 40.0f}));
    CHECK(!scroller.lay(list, {}, 0.02f).kept);
    CHECK(!scroller.lay(list, {}, 1.0f).kept);
    CHECK(sameAsFresh(scroller) == false); // (fresh isn't scrolled)
    const UiQuad* line2 = nullptr;
    for (const UiQuad& quad : *scroller.cache.quads())
      if (quad.kind == UiQuad::Glyph && near(quad.rect.y, 0.0f))
        line2 = &quad;
    CHECK(line2 != nullptr); // line2 at the top, scrolled 40
    scroller.lay(list, {}, 1.0f);
    CHECK(scroller.lay(list, {}, 1.0f).kept);

    // Scrolled part way, a line in it changed: the lines wholly in sight
    // kept (they were cut to the view as they are now).
    const auto lines = [](std::string last)
    {
      std::vector<View> each;
      for (int i = 0; i < 9; ++i)
        each.push_back(Text("line" + std::to_string(i)));
      each.push_back(Text(std::move(last)));
      return ScrollView(
                 VStack(std::move(each), Alignment::Leading).spacing(0.0f))
          .scrollOffset(30.0f)
          .maxHeight(100.0f)
          .align(Alignment::TopLeading);
    };
    Kept cut;
    cut.lay(lines("line9"));
    cut.lay(lines("line9"));
    const Cache::Stats& changed = cut.lay(lines("LINE9"));
    CHECK(!changed.kept && changed.reused >= 4);
    // (line9's out of sight; line1 and line6, cut at the edges, aren't kept)
    CHECK(cut.text.drawn.size() == 2u);
    cut.lay(lines("LINE9"));
    // As laid out from scratch (which starts scrolled there too).
    CHECK(sameAsFresh(cut));
  }

  TEST("a disclosure group's state should stay with it when a view of "
       "another kind is put before it")
  {
    Kept kept;
    const auto panel = [](bool note)
    {
      std::vector<View> rows;
      if (note)
        rows.push_back(Text("note"));
      rows.push_back(DisclosureGroup("Weather", Text("rain")));
      return VStack(std::move(rows), Alignment::Leading)
          .frame(300.0f)
          .align(Alignment::TopLeading);
    };
    kept.lay(panel(false));
    const Hit* header = kept.clickable(Node::Kind::Disclosure);
    CHECK(header != nullptr);
    if (header)
      header->action(*nodeAt(*kept.view.node(), header->source));
    kept.lay(panel(false), {}, 1.0f);
    CHECK(std::find(kept.text.drawn.begin(), kept.text.drawn.end(), "rain") !=
          kept.text.drawn.end());
    kept.lay(panel(true), {}, 1.0f);
    CHECK(std::find(kept.text.drawn.begin(), kept.text.drawn.end(), "rain") !=
          kept.text.drawn.end());
  }

  return testing::report("uiLayoutTests");
}
