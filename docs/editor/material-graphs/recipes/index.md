# Material recipes

Step-by-step graphs for looks games often need: glows, water, lava, terrain,
worn metal and more. Each recipe says what it's for and how it works. It ends
with the graph itself, ready to paste.

## Using a recipe

1. In the Assets panel, right-click a folder → **New Material Graph**, and
   double-click it to open it.
2. Copy the recipe's graph (the button at the top right of the block copies
   it).
3. Click the canvas and press **Ctrl+V**. The nodes arrive linked into your
   Material Output, with the recipe's values on it.
4. Set the toolbar's transparency when the recipe says to, and pick images for
   any Texture nodes (select one, then choose its image in the sidebar).

The nodes the new graph started with are left where they were. Delete them if
the recipe doesn't use them (select them and press **Del**).

Every number in a recipe is a starting point. Select a node and change its
values in the sidebar while you watch the preview.

You can share your own graphs the same way: press **Ctrl+A** then **Ctrl+C** in
a graph, and paste into another with **Ctrl+V**.

## Textured, tinted and tiled

An image repeated across a surface and tinted by a colour: floors, walls,
fabric. It's what the standard material does, built from nodes so you can add
to it.

- **Tiling and Offset** repeats the texture coordinates 4 times each way.
- **Texture** reads the image there. Pick its image in the sidebar.
- **Multiply** tints it by the **Colour** (white leaves it as it is).

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0]},
    {"id": 2, "type": "tilingOffset", "position": [-620, 0], "inputs": {"Tiling": [4, 4, 0, 0]}},
    {"id": 3, "type": "texture", "position": [-420, 0], "texture": "", "dataTexture": false},
    {"id": 4, "type": "color", "position": [-420, 170], "value": [1, 1, 1, 1]},
    {"id": 5, "type": "multiply", "position": [-200, 40]}
  ],
  "links": [
    {"from": [2, "UV"], "to": [3, "UV"]},
    {"from": [3, "RGB"], "to": [5, "A"]},
    {"from": [4, "Colour"], "to": [5, "B"]},
    {"from": [5, "Out"], "to": [1, "Base Colour"]}
  ]
}}
```

To make the image repeat more or less often, change Tiling. To slide it, change
Offset.

## Glowing and pulsing

A light that breathes: magic crystals, warning lights, a lit sign. It glows
by itself, so it shows even in the dark, and at its brightest it's brighter
than white, so the glow spills onto what's around it (bloom).

- **Time** counts seconds, and **Multiply** (B 3) speeds it up.
- **Sine** turns it into a wave between -1 and 1.
- **Remap** moves the wave to between 0.3 and 3, so it never quite goes out,
  and peaks at three times white.
- **Multiply** with the **Colour** gives the glow, into **Emission**.

The Material Output's Base Colour is set nearly black, so the glow is all you
see.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Base Colour": [0.08, 0.08, 0.08, 0]}},
    {"id": 2, "type": "time", "position": [-820, 0]},
    {"id": 3, "type": "multiply", "position": [-620, 0], "inputs": {"B": [3, 3, 3, 3]}},
    {"id": 4, "type": "sine", "position": [-440, 0]},
    {"id": 5, "type": "remap", "position": [-280, 0], "inputs": {"From Min": [-1, -1, -1, -1], "From Max": [1, 1, 1, 1], "To Min": [0.3, 0.3, 0.3, 0.3], "To Max": [3, 3, 3, 3]}},
    {"id": 6, "type": "color", "position": [-280, 220], "value": [1.0, 0.45, 0.1, 1]},
    {"id": 7, "type": "multiply", "position": [-80, 60]}
  ],
  "links": [
    {"from": [2, "Time"], "to": [3, "A"]},
    {"from": [3, "Out"], "to": [4, "In"]},
    {"from": [4, "Out"], "to": [5, "In"]},
    {"from": [5, "Out"], "to": [7, "A"]},
    {"from": [6, "Colour"], "to": [7, "B"]},
    {"from": [7, "Out"], "to": [1, "Emission"]}
  ]
}}
```

For a faster pulse, raise the first Multiply's B. To set how dark it gets
between pulses, change the Remap's To Min; for more or less bloom, its To Max.

The glow lights what's around it too, pulsing with it: nearby walls and
ground catch its colour.

## Hologram with a glowing rim

Bright at the edges and faint in the middle: holograms, force fields, ghosts.
Set the transparency to **See-through (blend)**.

- **Fresnel** is 1 where the surface turns away from the camera (its outline)
  and 0 where it faces it. A higher Power gives a thinner rim.
- **Multiply** (B 3) makes the rim brighter than white, so it blooms, and a
  second **Multiply** with a **Colour** tints it, into **Emission**.
- **Remap** turns the same rim into opacity from 0.15 to 0.9, into **Alpha**,
  so the middle is nearly clear.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Base Colour": [0.03, 0.15, 0.2, 0], "Roughness": [0.4, 0, 0, 0]}},
    {"id": 2, "type": "fresnel", "position": [-760, 0], "inputs": {"Power": [2.5, 2.5, 2.5, 2.5]}},
    {"id": 3, "type": "multiply", "position": [-560, 0], "inputs": {"B": [3, 3, 3, 3]}},
    {"id": 4, "type": "color", "position": [-560, 170], "value": [0.3, 0.9, 1.0, 1]},
    {"id": 5, "type": "multiply", "position": [-300, 40]},
    {"id": 6, "type": "remap", "position": [-300, 200], "inputs": {"From Min": [0, 0, 0, 0], "From Max": [1, 1, 1, 1], "To Min": [0.15, 0.15, 0.15, 0.15], "To Max": [0.9, 0.9, 0.9, 0.9]}}
  ],
  "links": [
    {"from": [2, "Out"], "to": [3, "A"]},
    {"from": [3, "Out"], "to": [5, "A"]},
    {"from": [4, "Colour"], "to": [5, "B"]},
    {"from": [5, "Out"], "to": [1, "Emission"]},
    {"from": [2, "Out"], "to": [6, "In"]},
    {"from": [6, "Out"], "to": [1, "Alpha"]}
  ]
}}
```

## Dissolving with a burning edge

Burns away in random patches, with a glowing edge where it's eating in: a
teleport, a spell, something burning up. Set the transparency to
**Cut-out (mask)**.

- **Noise** gives every point a random value. It goes into **Alpha**.
- A **Number** goes into **Alpha Clip**: points whose noise is below it aren't
  drawn. At 0 nothing's gone; at 1 everything is.
- **Subtract** works out how far each point's noise is above the clip.
  **Smoothstep** (0 to 0.06) and **One Minus** turn that into 1 right at the
  edge, fading to 0 just inside.
- **Multiply** (B 4) and **Multiply** with an orange **Colour** make that edge
  glow brighter than white, into **Emission**, so it blooms.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [420, 0]},
    {"id": 2, "type": "noise", "position": [-700, 0], "inputs": {"Scale": [8, 8, 8, 8]}},
    {"id": 3, "type": "number", "position": [-700, 160], "value": [0.45, 0, 0, 0]},
    {"id": 4, "type": "subtract", "position": [-480, 80]},
    {"id": 5, "type": "smoothstep", "position": [-300, 80], "inputs": {"Edge 1": [0, 0, 0, 0], "Edge 2": [0.06, 0.06, 0.06, 0.06]}},
    {"id": 6, "type": "oneMinus", "position": [-140, 80]},
    {"id": 7, "type": "multiply", "position": [20, 80], "inputs": {"B": [4, 4, 4, 4]}},
    {"id": 8, "type": "color", "position": [20, 230], "value": [1.0, 0.5, 0.1, 1]},
    {"id": 9, "type": "multiply", "position": [200, 160]}
  ],
  "links": [
    {"from": [2, "Out"], "to": [1, "Alpha"]},
    {"from": [3, "Value"], "to": [1, "Alpha Clip"]},
    {"from": [2, "Out"], "to": [4, "A"]},
    {"from": [3, "Value"], "to": [4, "B"]},
    {"from": [4, "Out"], "to": [5, "In"]},
    {"from": [5, "Out"], "to": [6, "In"]},
    {"from": [6, "Out"], "to": [7, "A"]},
    {"from": [7, "Out"], "to": [9, "A"]},
    {"from": [8, "Colour"], "to": [9, "B"]},
    {"from": [9, "Out"], "to": [1, "Emission"]}
  ]
}}
```

To make it dissolve over time, link Time into the Number's place: replace the
Number with Time through a Multiply (B 0.2), so it burns away over five
seconds.

## Flowing lava

Molten rock drifting along, with its hottest parts glowing.

- **Time** through **Multiply** (B 0.08) is how far the flow has moved.
  **Combine** makes that a sideways offset (R moves along U).
- **Tiling and Offset** slides the texture coordinates by it, and **Noise**
  reads its blotches there, so they drift.
- **Mix** goes from a dark crust **Colour** to a hot orange one by the noise,
  into **Base Colour**.
- **Smoothstep** (0.55 to 0.9) picks out the hottest blotches. **Multiply**
  (B 3) and **Multiply** with the hot colour make them glow brighter than
  white, into **Emission**, so they bloom.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [220, 0]},
    {"id": 2, "type": "time", "position": [-1100, 0]},
    {"id": 3, "type": "multiply", "position": [-920, 0], "inputs": {"B": [0.08, 0.08, 0.08, 0.08]}},
    {"id": 4, "type": "combine", "position": [-740, 0]},
    {"id": 5, "type": "tilingOffset", "position": [-540, 0]},
    {"id": 6, "type": "noise", "position": [-360, 0], "inputs": {"Scale": [5, 5, 5, 5]}},
    {"id": 7, "type": "color", "position": [-360, 160], "value": [0.25, 0.02, 0.0, 1]},
    {"id": 8, "type": "color", "position": [-360, 270], "value": [1.0, 0.45, 0.05, 1]},
    {"id": 9, "type": "lerp", "position": [-160, 60]},
    {"id": 10, "type": "smoothstep", "position": [-160, 260], "inputs": {"Edge 1": [0.55, 0.55, 0.55, 0.55], "Edge 2": [0.9, 0.9, 0.9, 0.9]}},
    {"id": 11, "type": "multiply", "position": [20, 260], "inputs": {"B": [3, 3, 3, 3]}},
    {"id": 12, "type": "multiply", "position": [20, 420]}
  ],
  "links": [
    {"from": [2, "Time"], "to": [3, "A"]},
    {"from": [3, "Out"], "to": [4, "R"]},
    {"from": [4, "RG"], "to": [5, "Offset"]},
    {"from": [5, "UV"], "to": [6, "UV"]},
    {"from": [7, "Colour"], "to": [9, "A"]},
    {"from": [8, "Colour"], "to": [9, "B"]},
    {"from": [6, "Out"], "to": [9, "T"]},
    {"from": [9, "Out"], "to": [1, "Base Colour"]},
    {"from": [6, "Out"], "to": [10, "In"]},
    {"from": [10, "Out"], "to": [11, "A"]},
    {"from": [11, "Out"], "to": [12, "A"]},
    {"from": [8, "Colour"], "to": [12, "B"]},
    {"from": [12, "Out"], "to": [1, "Emission"]}
  ]
}}
```

Link the Multiply into Combine's G as well to make it flow diagonally. Raise
the Noise's Scale for smaller blotches.

## Water

A see-through surface that's darker looking down into it and paler at a
glance, with glints drifting across it. Set the transparency to
**See-through (blend)**.

- **Fresnel** (Power 4) is 0 looking straight down and 1 at a shallow angle.
  **Mix** goes from a deep **Colour** to a pale sky one by it, into
  **Base Colour**.
- **Remap** makes the same Fresnel an opacity from 0.55 to 0.95, into
  **Alpha**: clearer looking straight down, as real water is.
- The Material Output's Roughness is 0.05, so the sky reflects in it.
- **Time** slides the texture coordinates diagonally (**Multiply**,
  **Combine** into R and G, **Tiling and Offset**). **Voronoi** cells drift
  there, and **Smoothstep** and **One Minus** keep only the points right at
  each cell's middle. Those glint, into **Emission**.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [200, 0], "inputs": {"Roughness": [0.05, 0, 0, 0]}},
    {"id": 2, "type": "fresnel", "position": [-640, 0], "inputs": {"Power": [4, 4, 4, 4]}},
    {"id": 3, "type": "color", "position": [-640, 150], "value": [0.02, 0.18, 0.3, 1]},
    {"id": 4, "type": "color", "position": [-640, 260], "value": [0.55, 0.8, 0.9, 1]},
    {"id": 5, "type": "lerp", "position": [-420, 80]},
    {"id": 6, "type": "remap", "position": [-420, 260], "inputs": {"From Min": [0, 0, 0, 0], "From Max": [1, 1, 1, 1], "To Min": [0.55, 0.55, 0.55, 0.55], "To Max": [0.95, 0.95, 0.95, 0.95]}},
    {"id": 7, "type": "time", "position": [-1060, 420]},
    {"id": 8, "type": "multiply", "position": [-880, 420], "inputs": {"B": [0.05, 0.05, 0.05, 0.05]}},
    {"id": 9, "type": "combine", "position": [-700, 420]},
    {"id": 10, "type": "tilingOffset", "position": [-520, 420]},
    {"id": 11, "type": "voronoi", "position": [-340, 420], "inputs": {"Scale": [12, 12, 12, 12]}},
    {"id": 12, "type": "smoothstep", "position": [-160, 420], "inputs": {"Edge 1": [0, 0, 0, 0], "Edge 2": [0.08, 0.08, 0.08, 0.08]}},
    {"id": 13, "type": "oneMinus", "position": [-10, 420]},
    {"id": 14, "type": "multiply", "position": [-10, 300], "inputs": {"B": [0.6, 0.6, 0.6, 0.6]}}
  ],
  "links": [
    {"from": [3, "Colour"], "to": [5, "A"]},
    {"from": [4, "Colour"], "to": [5, "B"]},
    {"from": [2, "Out"], "to": [5, "T"]},
    {"from": [5, "Out"], "to": [1, "Base Colour"]},
    {"from": [2, "Out"], "to": [6, "In"]},
    {"from": [6, "Out"], "to": [1, "Alpha"]},
    {"from": [7, "Time"], "to": [8, "A"]},
    {"from": [8, "Out"], "to": [9, "R"]},
    {"from": [8, "Out"], "to": [9, "G"]},
    {"from": [9, "RG"], "to": [10, "Offset"]},
    {"from": [10, "UV"], "to": [11, "UV"]},
    {"from": [11, "Distance"], "to": [12, "In"]},
    {"from": [12, "Out"], "to": [13, "In"]},
    {"from": [13, "Out"], "to": [14, "A"]},
    {"from": [14, "Out"], "to": [1, "Emission"]}
  ]
}}
```

Give it waves with a normal map: add a **Normal Map** node with a water
normal image, reading the same moving UV, into **Normal**.

## Leaves and fences with holes

An image whose transparent parts are cut out: leaves, grass cards, fences,
chain link. Set the transparency to **Cut-out (mask)**.

- **Texture** reads the image. Its **RGB**, times the mesh's own
  **Vertex Colour**, is the colour.
- Its **A** (the image's own transparency) goes into **Alpha**. Points with
  less alpha than **Alpha Clip** (0.5) aren't drawn.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Alpha Clip": [0.5, 0, 0, 0]}},
    {"id": 2, "type": "texture", "position": [-500, 0], "texture": "", "dataTexture": false},
    {"id": 3, "type": "vertexColor", "position": [-500, 220]},
    {"id": 4, "type": "multiply", "position": [-240, 0]}
  ],
  "links": [
    {"from": [2, "RGB"], "to": [4, "A"]},
    {"from": [3, "Colour"], "to": [4, "B"]},
    {"from": [4, "Out"], "to": [1, "Base Colour"]},
    {"from": [2, "A"], "to": [1, "Alpha"]}
  ]
}}
```

If edges look too thin or too thick, change the Material Output's Alpha Clip.

## Terrain: grass on flat ground, rock on slopes and peaks

Grass where the ground is low and flat, rock on steep slopes and high up. It
uses **Triplanar Texture**, which needs no texture coordinates, so it works on
generated terrain and never stretches on cliffs. Pick a rock image and a grass
image for the two Triplanar nodes.

- **Position**, **Split** and its **B** (the height, as Z is up) go through
  **Smoothstep** (1 to 3): 0 below 1 metre up, 1 above 3. **One Minus** turns
  it into "low ground".
- **Normal**, **Split** and its **B** is how much the ground faces up.
  **Smoothstep** (0.6 to 0.8) makes that "flat".
- **Multiply** combines them: 1 only where it's low and flat.
- **Mix** goes from rock to grass by that, into **Base Colour**.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [60, 0], "inputs": {"Roughness": [0.9, 0, 0, 0]}},
    {"id": 2, "type": "position", "position": [-1100, 0]},
    {"id": 3, "type": "split", "position": [-920, 0]},
    {"id": 4, "type": "smoothstep", "position": [-740, 0], "inputs": {"Edge 1": [1, 1, 1, 1], "Edge 2": [3, 3, 3, 3]}},
    {"id": 5, "type": "oneMinus", "position": [-560, 0]},
    {"id": 6, "type": "normal", "position": [-1100, 200]},
    {"id": 7, "type": "split", "position": [-920, 200]},
    {"id": 8, "type": "smoothstep", "position": [-740, 200], "inputs": {"Edge 1": [0.6, 0.6, 0.6, 0.6], "Edge 2": [0.8, 0.8, 0.8, 0.8]}},
    {"id": 9, "type": "multiply", "position": [-380, 100]},
    {"id": 10, "type": "triplanar", "position": [-560, 340], "inputs": {"Scale": [0.5, 0.5, 0.5, 0.5]}, "texture": "", "dataTexture": false},
    {"id": 11, "type": "triplanar", "position": [-560, 560], "inputs": {"Scale": [0.5, 0.5, 0.5, 0.5]}, "texture": "", "dataTexture": false},
    {"id": 12, "type": "lerp", "position": [-160, 200]}
  ],
  "links": [
    {"from": [2, "Position"], "to": [3, "In"]},
    {"from": [3, "B"], "to": [4, "In"]},
    {"from": [4, "Out"], "to": [5, "In"]},
    {"from": [6, "Normal"], "to": [7, "In"]},
    {"from": [7, "B"], "to": [8, "In"]},
    {"from": [5, "Out"], "to": [9, "A"]},
    {"from": [8, "Out"], "to": [9, "B"]},
    {"from": [10, "RGB"], "to": [12, "A"]},
    {"from": [11, "RGB"], "to": [12, "B"]},
    {"from": [9, "Out"], "to": [12, "T"]},
    {"from": [12, "Out"], "to": [1, "Base Colour"]}
  ]
}}
```

Move the heights to suit your world (Smoothstep's edges are in metres). The
Triplanar Scale is how many times its image repeats per metre.

For bumps, add a **Triplanar Normal Map** for each texture with its normal map
and the same Scale, blend the two with another **Mix** by the same mask, and
link it into **Normal**.

## Worn and rusty metal

Bare metal with patches of rust, which are rough and not metallic.

- **Noise** (Scale 4) and **Smoothstep** (0.45 to 0.6) make a patchy mask: 0
  where it's metal, 1 where it's rust, with a short blend between.
- Three **Mix** nodes use the mask: steel to rust **Colour** into
  **Base Colour**, 1 to 0 into **Metallic**, and 0.25 to 0.9 into
  **Roughness**.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0]},
    {"id": 2, "type": "noise", "position": [-760, 0], "inputs": {"Scale": [4, 4, 4, 4]}},
    {"id": 3, "type": "smoothstep", "position": [-560, 0], "inputs": {"Edge 1": [0.45, 0.45, 0.45, 0.45], "Edge 2": [0.6, 0.6, 0.6, 0.6]}},
    {"id": 4, "type": "color", "position": [-560, 150], "value": [0.75, 0.75, 0.78, 1]},
    {"id": 5, "type": "color", "position": [-560, 260], "value": [0.45, 0.17, 0.06, 1]},
    {"id": 6, "type": "lerp", "position": [-300, 100]},
    {"id": 7, "type": "lerp", "position": [-300, 300], "inputs": {"A": [1, 1, 1, 1], "B": [0, 0, 0, 0]}},
    {"id": 8, "type": "lerp", "position": [-300, 460], "inputs": {"A": [0.25, 0.25, 0.25, 0.25], "B": [0.9, 0.9, 0.9, 0.9]}}
  ],
  "links": [
    {"from": [2, "Out"], "to": [3, "In"]},
    {"from": [4, "Colour"], "to": [6, "A"]},
    {"from": [5, "Colour"], "to": [6, "B"]},
    {"from": [3, "Out"], "to": [6, "T"]},
    {"from": [3, "Out"], "to": [7, "T"]},
    {"from": [3, "Out"], "to": [8, "T"]},
    {"from": [6, "Out"], "to": [1, "Base Colour"]},
    {"from": [7, "Out"], "to": [1, "Metallic"]},
    {"from": [8, "Out"], "to": [1, "Roughness"]}
  ]
}}
```

The same mask works for worn paint (metal showing through paint), moss on
stone or dirt on anything: change the colours and the roughness.

## Cobblestones

Stones of slightly different colours, with dark grout between them.

- **Voronoi** (Scale 6) splits the surface into cells. Its **Cell** gives each
  stone a random value, and **Mix** picks its colour between two stone
  **Colour** nodes by it.
- Voronoi's **Distance** is how far a point is from its stone's middle.
  **Smoothstep** (0.38 to 0.5) makes the far parts, near the edges, into
  grout.
- A last **Mix** lays the dark grout **Colour** over the stones there.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [120, 0], "inputs": {"Roughness": [0.85, 0, 0, 0]}},
    {"id": 2, "type": "voronoi", "position": [-760, 0], "inputs": {"Scale": [6, 6, 6, 6], "Randomness": [0.8, 0.8, 0.8, 0.8]}},
    {"id": 3, "type": "color", "position": [-560, 120], "value": [0.62, 0.6, 0.55, 1]},
    {"id": 4, "type": "color", "position": [-560, 230], "value": [0.35, 0.33, 0.3, 1]},
    {"id": 5, "type": "lerp", "position": [-340, 120]},
    {"id": 6, "type": "smoothstep", "position": [-340, -60], "inputs": {"Edge 1": [0.38, 0.38, 0.38, 0.38], "Edge 2": [0.5, 0.5, 0.5, 0.5]}},
    {"id": 7, "type": "color", "position": [-340, 320], "value": [0.12, 0.11, 0.1, 1]},
    {"id": 8, "type": "lerp", "position": [-120, 60]}
  ],
  "links": [
    {"from": [3, "Colour"], "to": [5, "A"]},
    {"from": [4, "Colour"], "to": [5, "B"]},
    {"from": [2, "Cell"], "to": [5, "T"]},
    {"from": [2, "Distance"], "to": [6, "In"]},
    {"from": [5, "Out"], "to": [8, "A"]},
    {"from": [7, "Colour"], "to": [8, "B"]},
    {"from": [6, "Out"], "to": [8, "T"]},
    {"from": [8, "Out"], "to": [1, "Base Colour"]}
  ]
}}
```

Raise Voronoi's Randomness for rounder, more irregular stones; at 0 they line
up into tiles.

## Checkered tiles

A checkerboard floor, a little glossy.

- **Checkerboard** gives two colours in squares (Scale 4 each way), into
  **Base Colour**.
- The Material Output's Roughness is 0.3.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Roughness": [0.3, 0, 0, 0]}},
    {"id": 2, "type": "checkerboard", "position": [-360, 0], "inputs": {"Scale": [4, 4, 0, 0], "Colour A": [0.9, 0.88, 0.82, 0], "Colour B": [0.15, 0.15, 0.17, 0]}}
  ],
  "links": [
    {"from": [2, "Colour"], "to": [1, "Base Colour"]}
  ]
}}
```

## Bumpy surfaces with a normal map

Bricks, bark or carved stone, with bumps the light catches, without extra
geometry. You need an image and its normal map (the bluish image many texture
sites and Blender give alongside it).

- **Texture** reads the colour image into **Base Colour**.
- **Normal Map** reads the normal map into **Normal**. Pick the normal map
  image for it; it's always read as data. Its Strength makes the bumps
  deeper (above 1) or shallower.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0]},
    {"id": 2, "type": "texture", "position": [-420, 0], "texture": "", "dataTexture": false},
    {"id": 3, "type": "normalMap", "position": [-420, 220], "inputs": {"Strength": [1, 1, 1, 1]}, "texture": "", "dataTexture": false}
  ],
  "links": [
    {"from": [2, "RGB"], "to": [1, "Base Colour"]},
    {"from": [3, "Normal"], "to": [1, "Normal"]}
  ]
}}
```

Normal maps here are OpenGL style (green up), as Blender and glTF make them.
If the bumps look like dents, the map is DirectX style: flip its green channel
in an image editor.

## Grass that dries out

For a grass component's material: lush green where the ground is wet, straw
yellow where it's dry. The game writes how dry the ground is (0 wet, 1 dry)
into the grass's **Data 0** map, for example from sprinklers and the weather.
Dryness rather than moisture, so unpainted grass (Data 0 is 0) is green.

- Two **Colour** nodes (lush green and dry yellow) are **Mix**ed by the
  **Grass** node's **Data 0**.
- Times **Vertex Colour**: each blade's own tone, darker toward its root.
- Into **Base Colour**, with Roughness 0.85 on the Material Output.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0], "inputs": {"Roughness": [0.85, 0, 0, 0]}},
    {"id": 2, "type": "grass", "position": [-700, 220]},
    {"id": 3, "type": "color", "position": [-700, -60], "value": [0.2, 0.5, 0.12, 1]},
    {"id": 4, "type": "color", "position": [-700, 80], "value": [0.62, 0.55, 0.22, 1]},
    {"id": 5, "type": "lerp", "position": [-460, 40]},
    {"id": 6, "type": "vertexColor", "position": [-460, 240]},
    {"id": 7, "type": "multiply", "position": [-240, 60]}
  ],
  "links": [
    {"from": [3, "Colour"], "to": [5, "A"]},
    {"from": [4, "Colour"], "to": [5, "B"]},
    {"from": [2, "Data 0"], "to": [5, "T"]},
    {"from": [5, "Out"], "to": [7, "A"]},
    {"from": [6, "Colour"], "to": [7, "B"]},
    {"from": [7, "Out"], "to": [1, "Base Colour"]}
  ]
}}
```

To yellow the tips first as it dries, also mix by **Height**. Use **Length**
for mown grass that's a shade lighter.

## Toon bands

Colours flattened into a few even bands, for a painted or cartoon look.

- **Texture** times **Vertex Colour** is the usual colour.
- **Posterize** (Steps 4) rounds it down to 4 levels, into **Base Colour**.

```json
{"sfsMaterialNodes": {
  "nodes": [
    {"id": 1, "type": "output", "position": [0, 0]},
    {"id": 2, "type": "texture", "position": [-560, 0], "texture": "", "dataTexture": false},
    {"id": 3, "type": "vertexColor", "position": [-560, 220]},
    {"id": 4, "type": "multiply", "position": [-340, 60]},
    {"id": 5, "type": "posterize", "position": [-160, 60], "inputs": {"Steps": [4, 4, 4, 4]}}
  ],
  "links": [
    {"from": [2, "RGB"], "to": [4, "A"]},
    {"from": [3, "Colour"], "to": [4, "B"]},
    {"from": [4, "Out"], "to": [5, "In"]},
    {"from": [5, "Out"], "to": [1, "Base Colour"]}
  ]
}}
```

Fewer Steps make bolder bands.

Read more: [Material graphs](../index.md).
