# Material graphs

A material graph builds a material's look from nodes: colours, textures, noise,
maths and effects, linked together into the surface's colour, opacity,
metalness, roughness and glow. Use one when the standard material's settings
aren't enough, for blotchy or patterned surfaces, glowing rims, masks, or
textures blended together.

A graph material is still a `.mat` file. It goes in material slots, drops onto
objects and shows its preview in the Assets panel like any other material.

New to graphs? The [recipes](./recipes/index.md) build common looks step by
step (glows, water, lava, terrain, rust, cobblestones), each ready to paste
into a graph.

## Making one

- **New:** right-click a folder (or empty space) in the Assets panel →
  **New Material Graph**. It starts out looking like a fresh standard material:
  white, times the model's own colours.
- **From a standard material:** select it, then press **Convert to Node Graph**
  in the Inspector. The graph it gets draws exactly what the material drew
  before (its colour, textures, tiling, metal and roughness, as nodes), so
  nothing changes until you edit it.

Double-click a graph material in the Assets panel to open it, or select it and
press **Edit Graph** in the Inspector.

## The Material Editor

The graph opens in its own tab, below the scene's viewport, so you can watch
the scene while you edit. Other graphs you open join it as tabs beside it.

The tab has three parts:

- **The canvas** holds the nodes and the links between them.
- **The sidebar** shows a preview, then the selected node's values. The
  preview is drawn exactly as the scene draws it, moving if the graph uses
  Time; drag it to turn it. Under it, pick the shape it's on: a Sphere shows
  every angle at once (shine, reflections, rims), a Cube how the material wraps
  around edges and corners, and a Plane one flat surface (tiling, patterns,
  bumps). The shape is only for the preview; it doesn't change the material.
- **The toolbar** has Save, Frame, and the material's transparency. It also
  shows any problems with the graph.

Every edit shows in the scene straight away, on every object with the material.
The file changes only when you save.

## Nodes and links

Every graph ends in one **Material Output** node. Its inputs are the surface:

| Input | What it does |
| --- | --- |
| Base Colour | The surface's colour |
| Alpha | Opacity, for cut-out and see-through materials |
| Metallic | 0 for paint, wood and plastic; 1 for bare metal |
| Roughness | 0 is mirror-smooth; 1 is matte |
| Normal | Which way the surface faces, for bumps: link a Normal Map node here |
| Emission | Light the surface gives off, unaffected by the scene's lighting (glows, screens). It lights the things around it too, more strongly the brighter it glows and farther the more of it there is (see [Emission lights](../../runtime/rendering-3d/index.md#emission-lights)). Above 1 (brighter than white) it also blooms. |
| Alpha Clip | For cut-out materials: parts with less Alpha than this aren't drawn |
| Bloom | How much of its light blooms (1). 0 never blooms, however bright; above 1 blooms more than its brightness alone would. The camera sets how bloom looks. |

Other nodes work things out and feed them in. Each node's outputs are on its
right and its inputs on its left. A value flows along a link from an output to
an input.

| To | Do this |
| --- | --- |
| Add a node | Right-click empty space (or press Space), then pick one or type to search; Enter adds the first match |
| Link two nodes | Drag from an output to an input (or the other way round) |
| Add a node already linked | Drag a link from a pin and let go over empty space, then pick the node |
| Move or remove a link | Drag it off the input it goes into, and drop it on another (or on empty space, to remove it) |
| Cut a pin's links | Alt+click the pin |
| Select | Click a node; Ctrl+click adds or removes one; drag across empty space to box-select |
| Move nodes | Drag them |
| Pan and zoom | Middle- or right-drag pans; the wheel zooms |
| Fit the graph in view | **Frame**, or F (fits the selected nodes, if any) |
| Use a texture | Drag it from the Assets panel onto the canvas: it becomes a Texture node |

An input with nothing linked to it uses its own value, shown beside the input's
name (see [Editing values](#editing-values)). A few inputs read something from the mesh instead: a Texture
node's UV input uses the mesh's texture coordinates until you link something to
it.

A pin's colour shows what it carries:

| Colour | Carries |
| --- | --- |
| Grey | A number |
| Green | Two numbers (a Vector 2, such as texture coordinates) |
| Yellow | Three numbers (a Vector 3, such as a colour or a direction) |
| Purple | Four numbers (a Vector 4, such as a colour with alpha) |

Any output can link to any input. A number fills every part of a wider input,
and a wider value drops its extra parts. Maths nodes take the widest value
linked to them, so multiplying a colour by a number gives a colour.

## Editing values

Edit values right on the nodes:

- **Numbers and vectors:** drag a field left or right to change it, or
  double-click it to type a value. Inputs that only make sense from 0 to 1
  (Alpha, Metallic, Roughness, Alpha Clip) are sliders.
- **Colours:** click a colour (a Colour node's bar, or a swatch beside an input)
  to open a colour picker. Click away to close it.

Editing a node's value selects it. Zoomed far out, the fields are too small to
use, so they show as plain text: zoom in to edit them, or use the sidebar.

Selecting a node also shows its values in the sidebar, at a comfortable size:

- A **Colour** node's colour, or a **Number** node's number.
- A **Texture** node's image. Pick it from the list, or drop one from the Assets
  panel. Tick **Data, not colour** for images that hold numbers rather than a
  picture: masks, roughness or metalness maps.
- Each input's own value, while nothing is linked to it. A linked input shows
  where it comes from, with an **Unlink** button.

## The nodes

Right-click empty space to see them all, sorted by category. Hover over one in
the menu for what it does.

- **Input:** Colour, Number, Texture, Normal Map, Triplanar Texture,
  Triplanar Normal Map, UV,
  Vertex Colour (a model's own colours), Position, Normal, View Direction,
  Time.
- **Math:** Add, Subtract, Multiply, Divide, Power, Minimum, Maximum, Mix,
  Clamp, Saturate, One Minus, Negate, Absolute, Fraction, Floor, Posterize,
  Square Root, Sine, Cosine, Step, Smoothstep, Remap.
- **Vector:** Split, Combine, Dot Product, Cross Product, Length, Distance,
  Normalize.
- **UV:** Tiling and Offset.
- **Patterns:** Noise (smooth random blotches), Voronoi (cells, like cobbles
  or scales) and Checkerboard.
- **Effects:** Fresnel (bright edges, for rims and glows).

A few need a word more:

- **Normal Map** reads a normal map image (the bluish one that comes with
  many textures) into the Material Output's **Normal**, so light catches bumps
  the mesh doesn't have. Maps are read OpenGL style (green up), as Blender and
  glTF make them. Its Strength makes the bumps deeper or shallower.
- **Triplanar Texture** projects an image from three sides and blends them by
  which way the surface faces. It needs no texture coordinates and never
  stretches, which suits terrain, rocks and generated meshes. Its Scale is how
  many times the image repeats per metre.
- **Triplanar Normal Map** does the same for a normal map, into **Normal**:
  bumps on terrain and rocks, with no texture coordinates needed. Give it the
  same Scale as the Triplanar Texture it goes with, so the bumps line up with
  the picture.
- **Voronoi** gives each cell a random number (**Cell**) and how far each
  point is from its cell's middle (**Distance**). Randomness 0 lines the cells
  up into a grid.

Time counts the seconds the scene has been running, for things that move (water,
flicker, pulsing glows). It runs while you edit too, so you see the movement in
the viewport and the preview.

## Transparency

The toolbar's transparency setting decides how Alpha is used:

- **Opaque** ignores Alpha.
- **Cut-out (mask)** doesn't draw the parts with less Alpha than Alpha Clip.
  Use it for leaves and fences.
- **See-through (blend)** lets the scene show through by Alpha. Use it for
  glass and water.

## Saving, undo and closing

- **Ctrl+S** (or **Save**) writes the material's file. The tab's title shows a
  dot while it has unsaved changes.
- **Ctrl+Z** and **Ctrl+Y** undo and redo the graph's edits, one drag or
  change at a time. The Edit menu does the same while the graph's tab was the
  last one you used.
- **Ctrl+C**, **Ctrl+V** and **Ctrl+D** copy, paste and duplicate the
  selected nodes, with the links between them. **Del** deletes them. The
  Material Output can't be deleted.
- To copy a whole material into another, press **Ctrl+A** then **Ctrl+C** in
  one graph and **Ctrl+V** in the other. The pasted nodes link into that
  graph's Material Output, and give it their values.
- Closing a tab with unsaved changes asks whether to save. **Discard** puts the
  scene back to the saved material.

## Problems

A graph that can't be worked out shows its problems in the toolbar and the
sidebar. Each problem has a **Show** button that selects the node it's about,
and that node gets a red outline. Until it's fixed, the scene keeps showing the
graph as it last worked. A graph saved with a problem draws magenta in the game,
so it's easy to spot.

Read more: [Material graphs in the engine](../../core/rendering-3d/material-graphs/index.md).
