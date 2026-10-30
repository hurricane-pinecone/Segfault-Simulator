#include "../../testHarness.h"

#include <engine/runtime/ui/fontAtlas.h>
#include <engine/runtime/ui/layout.h>
#include <engine/runtime/ui/placeholder.h>
#include <engine/runtime/ui/preview.h>

#include <filesystem>

#include <cmath>
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
      buttons[0]->action();
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
        named = named || hit.id.ends_with("#two");
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

  return testing::report("uiLayoutTests");
}
