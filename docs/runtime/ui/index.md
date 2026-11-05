# Game UI

A game's UI (a HUD, menus, buttons) is written the way SwiftUI writes it: a
`body()` that describes what to show, built from small views and modifiers,
from the game's state, every frame. The engine lays it out, draws it over the
scene, and calls buttons' actions when they're clicked.

```cpp
#include <sfs/sfs.h>

class Hud : public sfs::ui::Layer
{
public:
  sfs::ui::View body() override
  {
    using namespace sfs::ui;
    return VStack(Alignment::Leading, {
               Text("Coins: " + std::to_string(coins)).font(Font::title()),
               HStack{
                   Button("Buy seeds", [this] { buy(); }).disabled(coins < 10),
                   Button("Sell", [this] { sell(); }),
               },
           })
        .padding()
        .background(Panel())
        .align(Alignment::TopLeading);
  }

  int coins = 0;

private:
  void buy();
  void sell();
};
```

A layer is a script: put it on an entity (Add Component → Script, as for any
game object), and it shows while the scene plays. Its state is just its
members, or the game's systems it reads; `body()` runs every frame, so the UI
always shows the game as it is, with nothing to keep in sync. What it drew is
kept, though: a frame whose `body()` comes out the same isn't laid out or
drawn again, and one that's changed is only where it changed (see
[Retained drawing](#retained-drawing)), so writing it this way costs next to
nothing while the UI stands still.

## Views

| View | What it is |
| --- | --- |
| `Text("…")` | A line of text, or several split at `\n` |
| `Button("label", action)`, `Button(view, action)` | A button calling `action` when clicked |
| `Image("file.png")`, `Image::mesh("farm:wheat")` | A picture at its own size, or a mesh as a 32-point icon; framed smaller, it shrinks to fit (see [Icons](#icons-and-tints)) |
| `Image(picture)` | Pixels the game draws and changes, a `ui::Picture` (see [Pictures](#pictures)) |
| `Image(texture)` | A picture the GPU draws, a `RenderTexture` such as a camera feed (see [Camera feeds](#camera-feeds)) |
| `Image::placeholder()` | A soft box where a picture will be |
| `ProgressBar(fraction)` | A bar filled part way (0 to 1) |
| `Slider(value, change, low, high, step)` | A knob along a track (160 points wide), from `low` to `high` (0 to 1 by default); dragging it, or pressing the track, calls `change` with the new value, in steps of `step` (0: any) |
| `Toggle("label", on, change)` | A switch with a label beside it; a click calls `change` with the other state |
| `Spacer(min)` | Takes up whatever room its stack has spare, pushing the rest apart |
| `VStack{…}`, `VStack(Alignment::Leading, {…})` | Children one below another |
| `HStack{…}`, `HStack(Alignment::Top, {…})` | Children side by side |
| `ZStack{…}` | Children on top of one another, each placed by its `align()`; fills the space it's given |
| `ForEach(items, [](auto& item) { return …; })` | A stack's children made from a list |
| `ScrollView(content)`, `ScrollView(content, Axis::Horizontal)` | Its content, scrolled when it's bigger than the view (see below) |
| `DisclosureGroup("title", content, open)` | A section that opens and closes with a click on its header (see below) |
| `Empty()` | Nothing (for a branch with nothing to show) |

## Modifiers

Each returns a new view, so they chain, and order matters as in SwiftUI:
`.padding().background(x)` pads inside the background, and
`.background(x).padding()` pads outside it.

| Modifier | What it does |
| --- | --- |
| `.padding(points)`, `.padding(Insets{…})` | Space round it (12 by default) |
| `.frame(width, height)` | A fixed size (0: as it would be) |
| `.maxWidth(infinity)`, `.minWidth(…)`, `.maxHeight(…)`, `.minHeight(…)` | As wide or tall as it can be, or within bounds |
| `.align(Alignment::…)` | Where it sits in a ZStack, a frame, or (a body) on the screen |
| `.spacing(points)` | A stack's gap between children |
| `.background(Color)`, `.background(Fill)`, `.background(Panel())` | Filled behind it: colour, rounded corners, border, soft shadow |
| `.cornerRadius(…)`, `.border(Color, width)` | Its background's corners and edge |
| `.color(Color)`, `.font(Font)`, `.opacity(…)` | For it and everything inside (passed down until set again) |
| `.disabled(bool)` | Its buttons can't be pressed, and look it |
| `.buttonStyle(style)` | How a button looks: the theme's, `ButtonStyle::plain()`, or the game's (see [Button styles](#button-styles)); on a view, every button in it without one |
| `.tint(Color)`, `.tint()` | Images in it drawn as masks: their alpha in one colour (or the colour they inherit) |
| `.resizable()` | An image that grows to fill its frame too, keeping its shape (on an `Image`) |
| `.interpolation(Interpolation::Nearest)` | Images in it drawn with sharp pixels (`Linear`, smooth, by default) |
| `.help("…")` | A tooltip when hovered for a moment |
| `.id("…")` | Keeps a button in a changing list itself from frame to frame; a scroll view's offset or a section's open state by name |
| `.scrollOffset(points)` | Where a scroll view's scrolled to the first frame it's shown |
| `.accessory(view)` | A disclosure group's header: a view (a button) at its far end |
| `.accordion("group")` | A disclosure group in an accordion: opening it closes the others in the group |

`Panel()` is a ready-made look: a warm, dark, see-through card with rounded
corners and a soft shadow.

## Scroll views

`ScrollView(content)` is as big as its content, up to the room it's offered.
When the content's bigger (a long list, a panel taller than the screen), it
shows part of it and scrolls: the mouse wheel or a touchpad over it, eased; a
thin bar down its edge (only while there's more to see) that can be dragged,
or clicked above or below its thumb to go a page. What's outside the view
isn't drawn, laid out or clickable, so a long list costs only what shows.

```cpp
VStack(Alignment::Leading, {
    HStack{Text("JOBS"), Spacer(), Button("Close", close)}, // stays put
    ScrollView(VStack(std::move(rows), Alignment::Leading)
                   .padding(Insets{.trailing = 14})) // room for the bar
        .id("jobs"),
})
    .frame(400)
    .padding()
    .background(Panel())
    .align(Alignment::TopTrailing);
```

- **Sizing.** In a stack that's short of room, a scroll view gives its room
  up first: the panel above keeps its title and fits the screen, the list
  under it scrolling. Elsewhere give it a size with `.frame()` or
  `.maxHeight()`; inside another scroll view going the same way it needs one
  to scroll in.
- **Kept frame to frame.** Where it's scrolled to is kept while its layer
  lives, by its place in the tree, or by name with `.id()` (better, if what's
  round it changes). `.scrollOffset(points)` sets where it starts, the first
  frame it's shown (handy for screenshots).
- **The wheel** goes to the innermost scroll view under the pointer that can
  still go that way; at its end, the one round it scrolls. A scroll view
  takes the mouse as any UI does, so the wheel over it never reaches the game
  (a camera's zoom).
- **Ways.** `Axis::Vertical` (the default), `Axis::Horizontal` (the wheel
  goes along it) or `Axis::Both`.
- The bar's colours are the theme's `scrollThumb` and `scrollThumbHovered`.

## Disclosure groups

`DisclosureGroup("title", content, open)` is a section that opens and closes:
a header row (a chevron, its title, an optional accessory at the far end) and
its content below while it's open. A click on the header opens or closes it,
its height eased over a fifth of a second, the chevron turning. It starts open
with `open`, and is remembered while its layer lives (by its place, or
`.id()`). Closed, its content isn't laid out at all. It's as wide as it's
given; the title can be any view, and the chevron takes the title's colour.

```cpp
VStack(Alignment::Leading, {
    DisclosureGroup(Text("WEATHER").font(Font::caption()).color(muted),
                    VStack{...}, true)
        .accessory(Button("Reset", reset))
        .id("weather"),
    DisclosureGroup("LIGHTNING", VStack{...}).id("lightning"),
})
```

- `.accordion("group")` puts sections in an accordion: opening one closes
  the others in the same group (in the layer).
- To keep it open or closed in the game's own state instead,
  `DisclosureGroup("title", open, [this](bool to) { open = to; }, content)`:
  its header calls `change` with the other state, like a toggle.

## Button styles

A button draws the theme's face round its label unless it's given a style,
as in SwiftUI. `ButtonStyle::plain()` draws no face: the label is the button,
dimmed while it's pressed, so any view can be clicked with the look it has.
A style of the game's makes the button's look from its label and state:

```cpp
struct KeyStyle
{
  sfs::ui::View makeBody(const sfs::ui::ButtonConfiguration& c) const
  {
    using namespace sfs::ui;
    const Color face = c.pressed ? kKeyDown : c.hovered ? kKeyHover : kKey;
    return c.label.frame(90, 30).background(
        Fill{.color = face, .cornerRadius = 7, .shadow = c.pressed ? 1.0f : 3.0f});
  }
};

Button(Text("RUN"), run).buttonStyle(KeyStyle{})
HStack{...keys...}.buttonStyle(KeyStyle{}) // each button in it
```

- `ButtonConfiguration` has the `label`, and `hovered`, `pressed` and
  `disabled`. A lambda taking it works as well as a struct.
- `ButtonStyle::filled(normal, hovered, pressed, padding)` is a ready-made
  one: the label padded on a `Fill` for each state (disabled: faded).
- The style is called when it's given, once for each state (normal, hovered,
  pressed, disabled), so all four looks are part of the tree: what a style
  reads (a colour it captured, a lamp's state) is in the tree's hash and kept
  like any view. A style can't read anything else at layout time. The button
  is the size of its normal look, and the others are put in that space, so
  keep them the same size.
- The pointer coming over a styled button lays out and draws only that
  button again, as for any button.

## Icons and tints

`Image("icons/beacon.png")` loads a PNG from the project's assets (or
`asset:<id>`). Small images, up to 256 pixels a side, and mesh icons go on
shared atlas pages with their smaller levels (each half the last, averaged)
beside them, so an icon drawn small is drawn from the level nearest its size
and stays crisp, on whole pixels. Draw a 64-pixel icon at 16, 24 or 32 points
for the sharpest result. Text and icons from one page draw together in one
call: the UI binds a glyph texture and an image texture side by side.

`.tint(color)` draws images as masks: their alpha in `color`, for monochrome
white-on-clear icons such as a lamp's tell-tale, lit or unlit. `.tint()` with
no colour uses the colour they inherit, so `.tint().color(kAmber)` round a row
of icons colours them all. `.resizable()` lets an image grow to its frame, not
only shrink, and `.interpolation(Interpolation::Nearest)` draws its pixels as
sharp squares.

```cpp
Image("icons/road_lights.png").tint(lamp.on ? kGreen : kUnlit).frame(16, 16)
```

CozyFarm's tell-tale icons are in its `assets/icons/`, drawn by
`tools/icons/make_icons.py`.

## Pictures

A `ui::Picture` is pixels the game draws and changes, such as a combine's
yield map: RGBA8, rows from the top, colours as they look on screen. Make one
shared and keep it. Write its pixels (`set(x, y, color)`, `fill()`, or
`pixels()` directly), then call `changed()`, and the next frame shows them:

```cpp
// in the layer
std::shared_ptr<sfs::ui::Picture> m_yield =
    std::make_shared<sfs::ui::Picture>(128, 128);

m_yield->set(cellX, cellY, colourFor(yield));
m_yield->changed();

// in a view
Image(m_yield).resizable().interpolation(Interpolation::Nearest).frame(240, 240)
```

A change uploads its pixels again (all of them) and lays nothing out: the
view's hash covers which picture it is and its size, but not its pixels.
`resize()` lays out the views showing it again. Keep the picture alive while
it's shown. The UI Preview fills a `std::shared_ptr<Picture>` field with a
grey checker.

## Camera feeds

A camera feed films the scene into a small picture: a tractor's rear camera, a
combine's grain tank. Put a `CameraFeedComponent` on an entity (a child of the
machine, so it rides along); it looks along the entity's +X with +Z up, like a
camera entity. Ask the render system for its picture, a
`std::shared_ptr<const RenderTexture>`, and show it:

```cpp
auto& render = scene().getSystem<sfs::RenderSystem>();
m_rear = render.cameraFeed(rearCameraEntity); // made the first time

// in a view
Image(m_rear).resizable().frame(320, 180)
```

The GPU draws it, and its pixels never come back to the CPU. Each new picture
lays nothing out, as with a `Picture`. It's filmed only while a UI shows it,
at the component's `rate` (15 a second by default), one feed a frame. A feed
nothing shows costs nothing. It's filmed in a cheaper way than the scene: the
same light, sun and lamp shadows, sky, terrain and its deformation (ruts,
furrows) and grass's ground. It has no bloom, depth of field, ambient
occlusion, anti-aliasing over time or rain drops. Grass blades are left out
unless its `grassBlades` is on, which grows them again for the feed, out to
`grassReach`. Its first frame shown is dark grey, and filming starts on the
next one.

## Views of your own, and the UI Preview

A view of the game's own is a struct with a `View body() const`, made from its
fields, like SwiftUI's `struct Badge: View`. Use it anywhere a view goes:
`HStack{ Badge{.text = "New"}, Text("Wheat") }`. Keep such views in UI files:
a header ending `.ui.h` and its source ending `.ui.cpp`, for example
`scripts/ui/FarmViews.ui.h` and `FarmViews.ui.cpp`. The script compiler skips
`.ui.h` headers, as nothing in them is a component. UI files should use only
the engine and other UI files, never the game's scripts: a layer fills views in
from the game, and views only show what they're given.

```cpp
// scripts/ui/FarmViews.ui.h
struct ClockPanel
{
  std::string time; // "9:19 am"
  int day = 1;
  std::function<void()> pause;

  sfs::ui::View body() const;
};
```

The editor's **UI Preview** tab shows the UI files open in the Code panel as
you write them, with no need to press Play. Each view struct in the `.ui.h`
shows, filled with placeholders:

- text fields show their names (`time` shows "Time"),
- `View` fields (an icon) show `Image::placeholder()`,
- lists get two items, numbered ("What 1", "What 2"),
- an `std::optional` gets a value, and an `std::variant` has whichever
  alternative it holds filled in (the first, for a new one),
- numbers, flags and actions keep their defaults, as does anything already set.

To see a view with real-looking values, or in another state, declare a
preview beside it. It shows after the placeholders:

```cpp
// scripts/ui/FarmViews.ui.cpp
static const sfs::ui::Preview pausedClock{
    "Paused", [] { return ClockPanel{.time = "6:00 pm", .day = 12}; }};
```

If the game puts its views in a look of its own, such as a font set round the
whole HUD, declare a preview style in the `.ui.cpp`. Every preview of that
file is then shown in it, placeholders included:

```cpp
static const sfs::ui::PreviewStyle farmLook{
    [](sfs::ui::View view) { return view.font(Font::named("assets/fonts/Fredoka.ttf", 20)); }};
```

Saving a UI file rebuilds only the UI Preview's own small module, in a couple
of seconds: unoptimised, with the engine's headers precompiled. The game isn't
reloaded. Saving any other script rebuilds the game in the background, as
always, and Play always plays a fresh build. To see a whole screen, write one
as a view, for example a ZStack of panels each placed with `align()`, and have
the layer's `body()` fill it in from the game.

## Sizes, fonts and the theme

Sizes are in points: pixels on a screen 1080 lines high, scaled with the
window, so UI is the same size on any screen. `UiModule::setScale()` makes it
bigger or smaller (a game's UI scale setting).

Fonts are TrueType files: `Font::named("assets/fonts/Fredoka.ttf", 24)`, a
path in the project, or a font asset (`asset:<id>`). `Font::body()`,
`caption()`, `headline()` and `title()` are sizes in the default font. Text is
UTF-8, and glyphs are baked as they're first needed, so any language's letters
work if the font has them.

The default font and colours are the scene's theme:
`render.module<sfs::UiModule>()->theme()` (its `font`, `text`, `button`
colours, `accent`...). A game usually sets its font there once, in a layer's
`onCreate`.

## Clicks and the game

Layers get the mouse before the game does. A click on a button calls its
action, and while the mouse is over UI (or a press started on it), the game's
scripts see no clicks or scrolling: `MouseInput::blocked()` says so, for
example to hide a cursor marker under the UI. A drag the game started carries
on across UI. A slider pressed on keeps following the pointer until the
button's let go, even off the slider.

Like buttons, a slider's and a toggle's state is the game's: they show the
value they're given and call `change` with a new one, and the next frame's
`body()` shows it. `.disabled()` stops them too.

Layers draw in order (`Layer::order`, higher on top) and can be hidden
(`Layer::visible`).

## How it works

`UiModule` (a [render module](../rendering/render-modules/index.md) every
`StandardScene` has, in any style; a scene of your own registers it with
`render.withModule<sfs::UiModule>(*this)`) asks each layer for its `body()`
every frame and lays it out (only what changed: see [Retained
drawing](#retained-drawing)): stacks measure their children and share spare
room between flexible ones (spacers, infinite frames). A view's size is
measured once for each room it's offered (kept from frame to frame), and
what's scrolled out of sight or in a closed section isn't laid out. It outputs
quads (rounded boxes, glyphs, chevrons), each with the rect it's cut to (a
scroll view's), and what's clickable, cut the same way. What views keep from
frame to frame (scroll offsets, open sections) is each layer's `ui::State`,
kept by the module.
The WebGPU renderer draws the
quads in one pass over the finished frame, after bloom and depth of field.
Boxes come from a rounded-rectangle distance field (smooth corners, soft
shadows, borders), and text from a font atlas (SDL_ttf). The UI is part of the
game's picture, so it looks the same in the editor's viewport and in an
exported game.

## Retained drawing

The syntax is SwiftUI's, but the drawing isn't redone every frame: each
layer's last layout is kept (`ui::Cache`, one a layer in `UiModule`), and only
what changed is laid out and drawn again. This happens in the layout itself,
for every kind of view, so a new kind of view needs nothing of its own to be
kept.

- **What changed** is found by hashing. Each node of the tree has a hash
  (`Node::hash()`) of everything it shows: its kind, text, sizes, colours,
  fonts, id, help, and its children's hashes. Functions (a button's action) go
  in only as whether they're set. Two trees that would draw the same hash the
  same, even though `body()` made them anew. Each node is hashed once, the
  first time it's asked (a view kept in a member is hashed once in its life).
- **A layer the same as last frame** (the same body hash, screen size, scale
  and theme, and nothing it read has changed) isn't laid out at all. Its quads
  are handed to the renderer as they were, shared rather than copied.
- **A layer that changed** is laid out from the top again, but every view
  whose drawing is kept (the same node by hash, at the same place in the tree,
  in the same rect, inheriting the same font, colour and opacity, cut to the
  same clip) is copied from last frame instead of being measured and drawn.
  A clock's text that changes redraws that text and its stacks. The rest of
  the HUD (the job board, the buttons beside the clock) is reused. Sizes are
  kept the same way (by hash and the room offered), and so are lines' widths
  in the font atlas.
- **What else a view reads** is noted while it's drawn, and its drawing is
  only reused while that's the same:
  - *The pointer.* A button, slider, toggle, section header or scroll bar
    looks different hovered or pressed. A view reads the pointer only through
    the layout's `hovered(id)` and `pressed(id)`, which note the id. When the
    pointer comes over a button, that button is drawn again and nothing else.
  - *The layer's `ui::State`.* Scroll offsets and open sections. A view with
    a scroll view or disclosure group in it is drawn again when the State has
    changed (`State::changes`). Easing (scrolling, a section opening) moves on
    in `State::advance()` once a frame, before the layout.
  - *Pictures still on their way.* A mesh icon not loaded yet isn't kept, so
    it shows once it comes.
  - *The font atlas.* When a font's texture grows (its glyphs' coordinates
    move), everything is drawn again (`TextShaper::generation()`). Images'
    atlas pages never move what's on them, so they need nothing like it.
  - *Pictures* (`ui::Picture`) aren't read at all: their pixels changing
    re-uploads the texture without laying anything out.
- **Button styles** are called when they're given, so their looks are in the
  tree and hashed like any view; nothing at layout time calls game code.
- **Clicks call today's functions.** A hit doesn't hold the action it was
  laid out with. It holds its view's place in the tree (`Hit::source`), and a
  click calls the function of the node at that place in the layer's newest
  tree. The tree is the same shape, or it would have been laid out again. So
  a button whose drawing was kept for a minute still calls what this frame's
  `body()` gave it.
- **Ids.** Views are told apart by their place in the tree, hashed. A stack's
  child is counted among its siblings of the same kind, so putting a `Text`
  above a `DisclosureGroup` doesn't change which section it is. Putting
  another `DisclosureGroup` above it does, unless they have `.id()`s.

For screenshots of a hover or press, `SFS_UI_POINTER=x,y` pins the UI's
pointer at that point (in points from the top left), and `x,y,down` holds it
down there too.

For testing clicks, `SFS_UI_CLICK=help,frame` presses the middle of whatever
has that text in its help, from that frame on, through the same hit testing
the mouse uses, then lets go two frames later. It logs what it found there.

Writing views stays as it was, with one thing to keep in mind: a view's look
must come from its node, what it inherits, the theme, the State and the
pointer (through `hovered()`/`pressed()`). Anything else it read wouldn't be
noticed when it changed.

### Not making `body()` at all

`body()` itself still runs every frame, and building the tree costs something:
a little for the framework (modifiers on a view being built change it in
place, not as copies), more for a layer that works out a lot to show. A layer
can say when what it shows hasn't changed, and then `body()` isn't called:

```cpp
class FpsCounter : public sfs::ui::Layer
{
public:
  uint64_t inputs() override { return sfs::ui::hashOf(m_fps); }
  sfs::ui::View body() override { /* made from m_fps */ }
};
```

`inputs()` returns a hash of everything `body()` is made from
(`ui::hashOf(...)` takes numbers, enums, strings, and optionals, vectors and
pairs of them). While it's the same as last frame's, last frame's tree is used
again, its functions too, so they mustn't capture anything that changes
without changing the hash. 0, the default, means "call `body()` every frame".
It's optional: without it, an unchanged layer is still neither laid out nor
drawn. It only saves the making of the tree.

