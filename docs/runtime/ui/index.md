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
always shows the game as it is, with nothing to keep in sync.

## Views

| View | What it is |
| --- | --- |
| `Text("…")` | A line of text, or several split at `\n` |
| `Button("label", action)`, `Button(view, action)` | A button calling `action` when clicked |
| `Image("file.png")`, `Image::mesh("farm:wheat")` | A picture at its own size, or a mesh as a 32-point icon; framed smaller, it shrinks to fit |
| `Image::placeholder()` | A soft box where a picture will be |
| `ProgressBar(fraction)` | A bar filled part way (0 to 1) |
| `Spacer(min)` | Takes up whatever room its stack has spare, pushing the rest apart |
| `VStack{…}`, `VStack(Alignment::Leading, {…})` | Children one below another |
| `HStack{…}`, `HStack(Alignment::Top, {…})` | Children side by side |
| `ZStack{…}` | Children on top of one another, each placed by its `align()`; fills the space it's given |
| `ForEach(items, [](auto& item) { return …; })` | A stack's children made from a list |
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
| `.help("…")` | A tooltip when hovered for a moment |
| `.id("…")` | Keeps a button in a changing list itself from frame to frame |

`Panel()` is a ready-made look: a warm, dark, see-through card with rounded
corners and a soft shadow.

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
on across UI.

Layers draw in order (`Layer::order`, higher on top) and can be hidden
(`Layer::visible`).

## How it works

`UiModule` (a render module every 3D scene has) asks each layer for its
`body()` every frame and lays it out: stacks measure their children and share
spare room between flexible ones (spacers, infinite frames). It outputs quads
(rounded boxes, glyphs) and what's clickable. The WebGPU renderer draws the
quads in one pass over the finished frame, after bloom and depth of field.
Boxes come from a rounded-rectangle distance field (smooth corners, soft
shadows, borders), and text from a font atlas (SDL_ttf). The UI is part of the
game's picture, so it looks the same in the editor's viewport and in an
exported game.
