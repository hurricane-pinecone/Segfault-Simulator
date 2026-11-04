# Meshes, materials & lighting

What the renderer draws and how it lights it: meshes and their materials, the
camera, the sun and point lights, shadows, bloom, depth of field, haze and
grass. It's the same in every [style](../styles/index.md): a flat or
isometric game's sprites, particles and voxel terrain are meshes too, lit by
the same lights, so the lighting, shadows and effects here apply to them as
well. Mesh entities (`MeshRendererComponent`), models and grass are placed in
the world as their transforms say, not through a 2D game's `Space2D`, so they
belong to 3D games.

Meshes are drawn by `MeshModule`, one of the [render
modules](../render-modules/index.md) a `RenderSystem` hosts.
`StandardScene` registers it for you.

## Meshes

A `MeshData` is a CPU-side indexed triangle list. Each vertex has a position,
a normal and a colour. Z is up, and counter-clockwise faces are front faces.
Build meshes from the primitives in `meshPrimitives.h`, and combine several
into one mesh with `meshes::append`:

```cpp
#include "engine/core/rendering/meshPrimitives.h"

sfs::MeshData barn = sfs::meshes::box({8, 6, 4}, red);
sfs::meshes::append(barn,
                    sfs::meshes::prism({8.8f, 7, 2.6f}, slate),
                    glm::translate(glm::mat4(1.0f), {0, 0, 4}));
```

Primitives sit on the Z = 0 plane: a box's base is at Z = 0, not its centre. So
an entity placed at a ground point stands on it.

| Builder | Shape |
| --- | --- |
| `box(size, color)` | Axis-aligned box |
| `plane(size, color)` | Upward-facing rectangle |
| `cylinder(radius, height, segments, color)` | Capped cylinder |
| `cone(radius, height, segments, color)` | Capped cone |
| `sphere(radius, segments, rings, color)` | UV sphere resting on Z = 0 |
| `prism(size, color)` | Gable roof, ridge along X |

Every shape has texture coordinates, so textures and material graphs show on
it. Each flat face holds the whole image upright; a sphere, cylinder or cone
wraps it once around (from the top down, on a sphere); caps lay it flat.
`append()` keeps them.

Upload a mesh through the render system's `MeshLibrary`. It returns a
`MeshHandle`, and keeps the mesh's bounds for culling. Meshes are freed with the
render system.

```cpp
const sfs::MeshHandle barnMesh = render.meshes().add(barn);
```

### Loading models in the background

Scenes find model meshes by name (`asset:<id>:<part>`) through the asset
store. With `assetStore.setBackgroundLoading(true)` (the editor turns it on),
a model that isn't loaded yet is imported on a worker thread instead of
there and then:

- The library hands out a stand-in for the name at once (`pending()`; it
  draws nothing), so the scene opens straight away.
- Each frame, `StandardScene` takes in the models that have finished
  (`AssetStore::collectLoads()`) and turns their stand-ins into the meshes
  (`MeshLibrary::fulfilPending()`). Entities keep the same handle throughout.
- `AssetStore::loadProgress()` says how many have finished, for a progress
  bar.

It's off by default, so a game's scene is complete when it starts. Background
work runs on a `WorkerPool` (`engine/core/jobs/workerPool.h`), a few threads
that run jobs in order; a job must not touch the scene or the GPU, only hand
its result back.

`MeshLibrary::setKeepVertices(true)` keeps every mesh's vertices on the CPU
too, so `meshData(handle)` can give a mesh back (the editor draws prefab
previews from them). It's off by default, to save the memory.

## Drawing entities

Give an entity a `TransformComponent` and a `MeshRendererComponent`:

```cpp
sfs::TransformComponent transform{{-12.0f, 5.0f, 0.0f}};
transform.setYaw(0.3f);

sfs::Entity barn = scene.createEntity();
barn.addComponent<sfs::TransformComponent>(transform);
barn.addComponent<sfs::MeshRendererComponent>(sfs::MeshRendererComponent{barnMesh});
```

A child entity draws relative to its parent; see
[Parents & children](../../../core/hierarchy/index.md).

`MeshModule` batches every entity that shares a mesh into one instanced draw. A
field of 200 crops of one kind costs one draw call. Each instance is culled
twice: against the camera, for the lit pass, and against the sun's shadow
volume, for the shadow pass. An object just off screen can still cast a shadow
onto visible ground.

| Field | Meaning |
| --- | --- |
| `mesh` | The uploaded mesh |
| `materials` | A material for each of the mesh's sections (material slots), in order, in place of its own (`"asset:<id>"` of a material asset); empty entries, and slots past the end, keep the mesh's own |
| `tint` | Multiplies the mesh's colours. Alpha below 1 makes the object see-through. |
| `castsShadow` | Whether it blocks light: the sun's, and shadowed point lights'. Turn it off for ground planes. |
| `emitsLight` | Whether its materials' emission lights what's around it (see [Emission lights](#emission-lights)) |
| `lightStrength` | How brightly its emission lights things (1: as bright as it glows) |
| `lightRange` | How far its emission lights things (1: as far as that much glow reaches) |
| `lightCastsShadows` | Whether things block its emission's light |
| `emissionColor`, `emission` | A glow of its own, all over it, on top of its materials' emission (0: none; above 1 blooms). For a lamp switched on and off from a script, without a material for each state. It lights nothing round it: give the lamp a light for that. |

Objects sharing a mesh and materials draw as one instanced batch. See-through
things (glass materials, and objects with a faded tint) draw after everything
solid, back to front, one at a time, so keep their numbers modest.

### Levels of detail

A `MeshLodComponent` beside the `MeshRendererComponent` draws simpler meshes in
its mesh's place farther from the camera, and past them a billboard impostor,
so a wood can reach the horizon as real trees:

| Field | Meaning |
| --- | --- |
| `lod1`, `lod2`, `lod3` and their `...Distance` | The next levels' meshes and where each takes over (m from the camera to the entity's origin, x its largest scale axis). A level without a mesh is skipped |
| `impostor`, `impostorDistance` | A quad drawn past every level, turned about the entity's up axis to face the camera |
| `impostorFrames` | How many views round the model the impostor's textures hold, in a row |
| `impostorShadows` | Whether far ones cast (as the last mesh level); off, they cast none |
| `hysteresis` | How far past a level's ends the camera goes before it changes (no flicking back and forth on a boundary) |
| `fadeSeconds` | How long a change of level cross-fades over (0: at once) |
| `shadowLevel` | The level that casts the shadow while a finer one is drawn (a cheaper outline in the shadow map; 0: each its own) |
| `cullDistance` | Past this, nothing (0: never) |

A change of level cross-fades by dithering for `fadeSeconds` (0.3 s by default):
the instance's fade (`MeshInstance::emission.w`: above 0 fading out, below 0
fading in) cuts complementary pixels out of the two levels, then the new one is
drawn solid. `MeshModule` keeps each entity's level and when it changed; nothing
stays dithered at any distance. Only cut-out surfaces dither (masked and foliage
materials, see below); others switch at once, so no shader that draws solid
things gets a `discard`. Shadows don't fade. The camera is checked against the
mesh's own bounds, for every level.

The impostor is a quad in the mesh's x = 0 plane, facing +x, its pivot on the
ground under the middle like the model's. Its material's textures hold
`impostorFrames` views of the model side by side: frame k seen from the
model's +x turned 360 k / frames degrees anticlockwise. `MeshModule` turns it to
the camera and shows the frame nearest the view's direction through the
instance's `uvRect`. Bake its normals in the frame's
own tangent space (x the view's right, y up, z toward the viewer), and it's lit
like the model. CozyFarm's `tools/trees` generator writes trees this way.

## Materials

A `MeshMaterial` is either the standard surface (glTF's metallic-roughness
model: colour, textures, tiling, metal, roughness and a normal map; models
bring theirs, normal maps included) or a compiled
[material graph](../../../core/rendering/material-graphs/index.md), when its
`shader` is set. `MeshLibrary::material()` loads a material asset by name, and
`MeshLibrary::setMaterial()` makes or changes one from game code.

The renderer builds pipelines per surface: one set for the standard surface,
and one for each graph shader and alpha mode, shared by every material that
uses it. Materials that differ only in their values share pipelines, and
changing a value doesn't rebuild anything. A graph shader that fails to build is
logged once, and its materials draw magenta.

Each frame's `FrameParams::time` is the seconds `RenderSystem` has been
updated, for animated materials (a graph's Time node). The scene updates every
frame, in a game and in the editor alike, so animated materials move while
editing too.

### Foliage materials

A material with `foliage.enabled` (`FoliageSettings`; in a material asset
`"foliage": {"translucency": 0.6, "bend": 1, "sway": 1, "flutter": 1}`, in a
glTF model its material's `extras`: `{"sfs": {"foliage": {...}}}`) draws with
the foliage surface: the standard surface's textures and values, for a tree's
bark and its leaf cards (alpha-tested quads, each a texture of many leaves).

- **Vertex colours are data:** R how open the point is (it multiplies the
  albedo: darker deep in a crown, baked per vertex), G how much it sways with its
  branch (0 on the trunk, 1 at the twigs' tips), B how much it flutters (0 on
  bark, up to 1 at a leaf card's far corners).
- **Wind** (the scene's, as grass's: gusts rolling across the land the way it
  blows): the whole plant bends by its height above its instance's origin (x
  `bend`), its branches sway out of step (x `sway` x G), its leaves flutter
  (x `flutter` x B). In every pass, so shadows move too; nothing on the CPU.
- **Translucency:** sunlight shows through from behind (`translucency` x the
  metallic-roughness texture's red, which glTF leaves unused: a leaf's
  thinness), brightest looking toward the sun through it.
- **Normals kept on back faces:** a crown's cards have normals bent out from its
  middle, so it shades as one soft volume from either side.
- **Cut-outs kept as it shrinks:** its alpha is raised with the mipmap level, so
  a far crown doesn't thin away to nothing.
- It's always drawn cut out (so its bark can cross-fade between levels of
  detail too), and dithers where its instance fades.

Any foliage material, or a masked standard one, with `fadeNearCamera` (metres;
in a material asset `"fadeNearCamera": 0.4`, in a glTF's extras
`{"sfs": {"fadeNearCamera": 0.4}}`; a model's foliage gets 0.4 without it)
fades away by dithering where the camera would be inside it: gone within that
far of the eye, back by 1.5 times it, in the camera's view only (its shadow
stays). So a camera that doesn't collide with trees can pass through a crown
or a trunk without clipping into it, and a tree a metre in front stays solid.
`FrameParams::lineOfSight` (off by default) also fades such things within that
radius of the line from the eye to `cameraTarget`, for a game that wants
nothing ever in front of what's looked at.

A game tints foliage by season the usual way: a material made in code
(`MeshLibrary::setMaterial()`) in the leaves' slot, or the entity's `tint`.

### Terrain materials

A terrain material (`MeshMaterial::terrain`, a `TerrainMaterial` in
`engine/core/rendering/terrain.h`; a `.mat` file's `"terrain"` section) textures
big ground with up to five layers (grass, dirt, a path, rock) blended by a
**splat map**, a texture over the mesh's UVs: the first layer is everywhere,
and the map's red, green, blue and alpha paint the second to fifth over it
(each 0 to 1; where they add up past 1 they share it).

```json
{
  "formatVersion": 2,
  "alphaMode": "opaque",
  "terrain": {
    "splatMap": "asset:<id>",
    "blendSharpness": 0.65, "antiTiling": true, "variation": 0.18,
    "triplanarSharpness": 0.5,
    "layers": [
      {"name": "Grass", "baseTexture": "asset:<id>", "normalTexture": "asset:<id>",
       "roughnessTexture": "", "tint": [1, 1, 1], "tileSize": 3,
       "roughness": 0.95, "normalScale": 0.8, "grass": 1},
      {"name": "Dirt", "baseTexture": "asset:<id>", "normalTexture": "asset:<id>",
       "tileSize": 2.5, "roughness": 0.92, "grass": 0},
      {"name": "Rock", "baseTexture": "asset:<id>", "normalTexture": "asset:<id>",
       "tileSize": 9, "grass": 0, "triplanar": true}
    ]
  }
}
```

| Layer field | Meaning (default) |
| --- | --- |
| `baseTexture` | Its colour (sRGB); the alpha is its height, which layers blend by (opaque: flat) |
| `normalTexture` | A normal map (OpenGL style), read as data ("": flat) |
| `roughnessTexture` | Green x `roughness` ("": `roughness` all over) |
| `tint` | Its colour, x (1, 1, 1) |
| `tileSize` | World units across one repeat of its textures (4) |
| `roughness`, `normalScale` | As a standard material's (1, 1) |
| `grass` | How much of a [grass](#grass) field on the mesh grows over it: 1 as its maps say, 0 none (1) |
| `meadow` | How much it looks like grass seen from afar, past where blades are drawn (0): grass growing on the terrain carries its ground's look over it out to the horizon (see Grass, The ground); on its own, its colour breaks up in patches, a soft sheen shows at low angles, and light and dark bands roll across it with the wind, as over the blades |
| `triplanar` | Also lay its textures from the sides where the ground is steep (cliffs, rock, a gorge's walls), so they don't stretch down the slope (false) |
| `puddles` | How readily water pools on it, 0 to 1: its share of the micro relief's dips, and how full its low spots get. Dirt and paths 1, a meadow hardly (1) |

| Material field | Meaning (default) |
| --- | --- |
| `splatMap` | The splat map ("": the first layer only). Read as data, sampled smoothly, clamped at its edges |
| `blendSharpness` | 0 a soft cross-fade between layers; toward 1 a crisp edge where the higher layer pokes through (0.5) |
| `antiTiling` | Break repeats up (true) |
| `variation` | How much the brightness drifts over tens of metres (0.15) |
| `triplanarSharpness` | How `triplanar` layers switch between their side and top projections: 0 a wide blend, toward 1 a crisp switch (0.5) |
| `puddles` | Rain water collects in the mesh's low spots as the weather's `puddles` rise (false). See [Puddles](#puddles) |
| `puddleDepth` | How deep water stands in a low spot at full, in metres above its lowest point (or to its brim, if shallower) (0.05) |
| `microRelief` | Shallow dips this deep (metres) scattered over the ground, drawn only, so flat ground has low spots too (0) |
| `microReliefSize` | About how wide those dips are, in metres (3) |
| `deformable` | Pressed in where things go over it: tyre tracks, furrows, footprints (false). See [Deformation](#deformation) |

How it draws (`meshTerrainSurface.wgsl`): each layer's textures lie over the
world's XY from above, `tileSize` across, so ground of any size keeps its
texel size (a mesh's own UVs place only the splat map). With anti-tiling, each
layer is sampled at three randomly shifted and turned copies on a hexagonal
grid and blended, sharpened toward the nearest (Mikkelsen's hex-tiling, after
Heitz and Neyret), with its normals turned to match, so a 200 m field shows
no repeats; and a gentle brightness drift (value noise over 8 to 25 m) keeps
it from looking uniform. From above, a layer stretches down steep slopes: a
`triplanar` layer is also laid from the sides there, biplanar (after Quilez):
from the two axes the surface faces most, blended by its normal, each
projection hex-tiled and its normal map's tilt turned back into the world's
axes before it's laid onto the surface. The second projection's weight is
remapped so it's gone below about 35 degrees of slope, where the texture is
sampled from above only, as without it; so it costs a second set of samples
only on steep ground. Layers with no weight at a point aren't sampled
(explicit-gradient sampling keeps that legal). The vertex colour isn't used.

The layers go up as two texture arrays (colours with heights; normals with
roughness), every layer resized to the largest (at most 2048), shared by
terrain materials with the same textures; with the splat map that's three
textures, however many layers, well inside WebGPU's limits. Texture arrays
are core WebGPU, so it's the same on every backend and in browsers (on
wgpu's OpenGL fallback an array always has at least two layers, which it
needs).

**Painting it.** The editor's terrain brush (T) paints the splat map on the
mesh drawn with it, in place: `paintSplat()` changes the `TextureData`, and
`IRenderer::updateTexture()` sends the changed rectangle up (the splat map is
uploaded without mipmaps so it can be). A game can do the same.

**What a game paints on it.** A game drawing its own ground (worked fields,
say) keeps the terrain's splat map as authored and paints over it: a
material it makes in code (`MeshLibrary::setMaterial()`), stood in for by a
terrain through a style (`"materials"`, see [visual styles](../visual-styles/index.md)),
hands its `terrainPaint` (layers 2-5's weights, RGBA, data) and
`terrainTint` (a colour multiplying the layers, sRGB; its alpha is how dry
the ground is: below 1 it's soaked, darker and glossier, as rain wets it) to
the terrain (`TerrainMaterial::paint`, `::tint`). Where the paint gives a
layer weight, the splat map's weights give way by as much. Both are uploaded
without mipmaps, so the game changes them in place (`IRenderer::updateTexture()`
with the rectangle it repainted). Both are read through a gentle wobble fixed
to the ground (about a texel), so its texels never show as squares nor its
edges as straight lines. The paint doesn't limit grass: the game's grass maps
do.

**Grass on it.** `GrassModule` grows grass on a terrain (the mesh's first
material, as drawn now, so a style's variant counts) only as much as its
layers' `grass` lets it, x its own maps: painting dirt makes it bare there. The
maps a game reads and wears aren't changed by that; `GrassModule::terrainPainted()`
grows it again after the splat map changes.

#### Puddles

With `puddles` on, `PuddleModule` (one of a standard scene's render modules)
has the renderer work out where water collects on each mesh drawn with the
terrain (as its first material, as drawn now). It's found on the GPU, as a
**puddle map** over a 128 m window of the world's XY (12.5 cm texels), ahead
of the camera: centred where the view meets the ground, at most 40 m out.
Beyond the window water stands nowhere (the ground's only wet).

1. The ground's height at each texel: the mesh's surface (laid onto a 0.5 m
   grid on the CPU, in the background, each time the window moves 16 m on),
   less the terrain's micro relief (value-noise hollows, scaled by the
   layers' `puddles` through the splat map), plus how far the ground's
   pressed in (a [deformable](#deformation) terrain's window: the lowest of
   it under each texel, so a rut narrower than a texel still counts).
2. Its hollows are found by a morphological closing: each texel's highest
   neighbour within 2.5 m, then that's lowest within 2.5 m again, is the
   brim water fills a hollow narrower than that to. The lowest within 2.5 m
   is the hollow's bottom.
3. Each hollow holds up to `puddleDepth` above its bottom, or to its brim if
   shallower, less where the brim slopes (water runs off a rut down a hill),
   none in one shallower than 4 mm.

It's found again when the window moves, the ground's pressed in (at most
every 0.5 s while something drives over it), the deformation's window moves,
or the terrain's puddle settings change; not every frame. Each time costs
the GPU a fraction of a millisecond (timed as `puddles` with
`SFS_GPU_TIMING`). The water is drawn only: nothing on the CPU knows where it
stands (a game reads the deformation map, `DeformationModule::offsetAt()`,
for its ruts and hollows).

The map holds, per texel, how far up its hollow's water line it is (0 its
bottom, 1 the line) and how deep the hollow's water is there. The
terrain shader puts water where the line, risen to the weather's `puddles` x
the layers' `puddles` there, is above it. So as `puddles` goes from 0 to 1,
water spreads out from each hollow's bottom to its line. Water stays
when the rain stops: drying it is the game's job, by lowering `puddles`.

**Fill rate.** `PuddleModule::setFillRate(ground, texture)` says how fast
water rises in the puddles over the mesh's UVs (read as data, red 0 to 1;
null, the default, is 1 everywhere): the line there is risen to `puddles` x
that too, so where it's low water only stands in the deepest part of a
hollow, if at all. A game's soil, say: loose tilled earth soaks rain up (0.1),
packed tracks pond at once (1). Setting it finds the puddles again, so set it
when it changes (every few seconds at most), not every frame. The map keeps
it per texel (its blue).

A puddle is flat water, drawn through the lit pass's wet surfaces (Surface's
`water`): its own normal flat, very smooth, the ground under it darkened as
soaked. Under a physical sky it reflects the sky and clouds (its reflection
textures) by Fresnel, and catches the sun's glint. Rain's ripples ring it. The
ground shows through browner and murkier the deeper the water is, and a band
just above the water line is soaked darker (Surface's `soak`). Grass blades
aren't kept out of puddles: give the layers grass grows on a low `puddles`
(the farm's dirt and paths have no grass anyway).

The renderer gets each window with `IRenderer::setTerrainPuddles(material,
TerrainPuddles)` (the ground under it and the settings), kept with the
material through its updates. One map per material, so give each puddled
mesh its own terrain material. The CPU side is in
`engine/core/rendering/puddles.h`, the passes in
`engine/src/shaders/puddles.wgsl`.

#### Deformation

A `deformable` terrain is pressed in where things go over it, and stays so:
tyre tracks with their lugs in them, an implement's furrows, footprints.
`DeformationModule` (one of a standard scene's render modules) keeps a
**deformation map** for each such entity (`engine/core/rendering/deformation.h`):
how far its ground is pressed in (negative) or pushed up, 3 cm a texel, in
world space. It's sparse, in tiles of 64 x 64 texels (1.9 m, 16 KB) made only
where something has touched the ground, so a big field costs nothing until
it's driven over; it's plain data on the CPU.

**Deformers.** A `DeformerComponent` presses in wherever its meshes touch the
ground: the entity's own and, with `children`, its children's (put one on a
tractor prefab's root and its wheels and implement all deform; its body never
reaches the ground). As one of its meshes moves, the mesh's underside (what's
seen of it from below, near the ground: lugs, tines, a roller), looked at
sixteen times a texel so edges slope rather than step, presses the ground down
to `sinkDepth` below it. It presses again once it's gone a little way, not
each frame, so what it costs goes with the distance driven, not the frame
rate, and a parked one costs nothing: one rolling (a wheel, a disc) pressed
where it is, often enough that its ruts come within a millimetre of as deep
(each 8 cm on a wheel of 80 cm radius, 5 cm on one of 30); one sliding (a
tine, a plough body) each 6 cm (its underside found afresh, as it bobs), or as
soon as it's lowered or turned enough to move any of it a texel, pressed all
along the way it went, a texel at a time (its underside moved back along it),
so it cuts an unbroken furrow rather than a row of dimples, its ridges even
along it. With the game going fast (`Registry::timeScale()`), a mesh goes
further between frames: a rolling one is pressed each stride along the way it
went too, and either is pressed at most 16 times a frame (4 rolling), each
press the lowest of its share of the places, so tracks stay unbroken at any
speed; one that went further than 25 m/s of game time could take it (or a
metre) was put there, not driven, and is pressed only where it lands. A
mesh is made ready for this once (its triangles grouped, so those nowhere
near the ground aren't looked at, nor, on a closed solid, those facing up); a
skinned one is taken as it's posed. The soil pressed out rises
in a low ridge to the sides (`ridge`: the share of it; the rest compacts), at
most 0.4 x `sinkDepth` high, or 0.4 x the rut a wheel's pressing, if deeper,
and walls steeper than soil stands slump, around what it pressed. Nothing
goes deeper than 0.5 m. Rest vehicles on the ground as it is: lowering one by
its sink only presses it further each pass. In deep mud a game can let one
sit down into the ground (say by how muddy it is there and how heavy the
vehicle is, not by the rut it's making): its ruts go as deep as it sits plus
its sink, its ridges higher, and its wheels sit in them.

| DeformerComponent | Meaning (default) |
| --- | --- |
| `enabled` | Whether it deforms now, e.g. off while an implement's raised (true) |
| `sinkDepth` | How far it sinks into the softest ground, metres: about its weight on its footprint (0.05) |
| `softnessResponse` | How much the soil's softness changes that: 0 always `sinkDepth`, 1 `sinkDepth` x the softness (0.8) |
| `ridge` | The share of the soil it presses out that's pushed up alongside (0.5) |
| `children` | Whether its children's meshes deform too (true) |
| `tread` | For smooth tyres: an image whose red is a pattern pressed in under it, laid along its +x ("": none; modelled lugs press in by themselves) |
| `treadDepth`, `treadSize` | How deep the tread presses (0.02 m), and how far it spans before it repeats (0.5 m) |

**Soil.** `setSoftness(ground, softness)` sets how soft the ground is, 0
(hard: deformers barely mark it) to 1 (the default, the softest), everywhere
or as a function of the world XY (e.g. a game's soil moisture: wet mud deep,
dry ground shallow), asked where a deformer presses: at the corners of blocks
of 4 x 4 texels (12 cm), blended between them (a step in it, at the edge of a
game's grid cell, slopes over a block).

**From a game.** Through `render.module<sfs::DeformationModule>()`:

- `stamp(ground, center, heading, size, depth, shape)` presses a shape in (a
  footprint, a hoof): `shape` an image asset whose red is the imprint, from
  above, its top along `heading` (empty: a rounded oval), `size` metres
  along and across it, `depth` deep from the ground where it lands.
- `relax(ground, smooth, fade)` weathers the tracks: edges round off and
  ridges slump into ruts by `smooth`, and `fade` of what's left goes (call it
  after rain, or once a day).
- `relax(ground, tiles, corners)` weathers only some tiles, each by its own
  amount: `corners` (one per tile) is the smoothing at its four corners,
  blended across it, so wet ground can weather while dry ground beside it
  stays as it is (no fade). `map(ground)->tiles()` lists the tiles that exist.
- `clearRegion(ground, polygon)` (or a rectangle) flattens it, e.g. where a
  field's been tilled.
- `offsetAt(ground, point)` and `heightAt(ground, point)` read it (the
  latter the surface's height, pressed in: to settle a vehicle into its ruts).
- `save(ground)` packs the map small (millimetres, runs of flat ground and
  small steps between neighbours in few bytes) to keep with a saved game;
  `load(ground, bytes)` takes it back. It isn't saved with the scene.
- `map(ground)` is the `DeformationMap` itself, to read or change directly.

```cpp
auto* deformation = render.module<sfs::DeformationModule>();
deformation->setSoftness(ground, [&](glm::vec2 at) { return soil.wetness(at); });
deformation->clearRegion(ground, field.outline); // tilled
```

**Drawing.** Near the camera it's real geometry: a patch of fine rings (4 cm
between vertices at its middle, twice that each ring out, 20 m either side)
around where the view meets the ground (at most 24 m from the camera),
displaced on the GPU and drawn with a copy of the terrain's material, and
the terrain's own mesh is cut away under it. Its rings meet without cracks,
each vertex smoothed over its spacing (finer detail is in its per-pixel
normals), and it casts and takes shadows, so ruts shade themselves. Out to
about 30 m from the patch's middle, the ground is parallax-mapped from the
same map, cross-fading with the patch over a band inside its edge, so there's
no line; beyond, it's flat. The GPU holds a window of the map, 2048 texels
square (61 m, 8 MB of half floats), that wraps, so as the camera moves only
the tiles coming into it are uploaded. With nothing pressed in near the
camera, nothing more is drawn.

Grass growing on a deformable terrain is worn away where it's pressed in
(its Coverage, through `GrassModule`), and its [puddles](#puddles) fill the
ruts. `setWearsGrass(ground, false)` turns that off, for a game with its own
rules for what traffic does to grass. `setContacts(ground, callback)` tells
a game where each deformer touches the ground as it moves (a `Contact`: the
deformer's entity and the mesh that touched, the underside's world XY
rectangle, how far it moved since it last pressed, its `sinkDepth`, how
much it pressed in, 0 on ground too hard to mark, how deep it pressed the
ground below its own surface, and how soft the ground under it is), e.g. to
pack its soil, wear paths, or tell a wheel bogging down in mud. The callback is called from inside the module, so it notes what
it needs and doesn't call the module back. The renderer takes the window with `IRenderer::setTerrainDeformation()`
and `writeTerrainDeformation()`; a surface places a mesh's vertices itself with
an optional `surfaceVertex()` (see `meshSurface.wgsl`).

### Materials from code

`MeshLibrary::setMaterial(name, material)` makes (or, called again, updates in
place) a named material from game code, for example one whose texture the
game paints as it runs. Entities choose it by name in a `materials` slot like
any other; updating it shows on everything using it at once.

## Camera

`RenderSystem::camera()` returns the scene's `Camera`. It is an orbit camera
for a Z-up world: it looks at `target` from `distance` away, turned by `yaw`
around +Z and tilted by `pitch` above the ground. In a 3D game without a
camera entity, game code drives these fields, so a free orbit, a follow camera
and a fixed angle are just different ways of setting them. (A flat or
isometric game sees through a camera entity looking straight at its plane;
see [Styles](../styles/index.md#the-camera).)

```cpp
sfs::Camera& camera = render.camera();
camera.target = playerPosition;
camera.yaw += turnInput * dt;
camera.projection = sfs::Camera::Projection::Orthographic; // or Perspective
```

`groundForward()` and `groundRight()` give camera-relative movement axes on the
ground. `RenderSystem::screenRay(pixel)` returns the world-space ray through a
pixel of the view, from the camera as last rendered, for picking.
`Ray::intersectPlaneZ` finds where it hits a ground plane.

### Camera entities

A scene made in the editor usually sees through a camera entity instead: an
entity with a `CameraComponent` and a `TransformComponent`. In a 3D game the
camera sits at the entity's position and looks along its +X axis, with +Z up
(the way `setYaw` turns things); `views::looking(yaw, pitch)` and
`views::isometric()` (`views.h`) give rotations for common angles. Each frame, `RenderSystem` points `camera()` the way
the first active camera entity looks, so you move the view by moving the
entity. It can be a child, e.g. of a vehicle it rides on.

```cpp
sfs::Entity eye = scene.createEntity();
sfs::TransformComponent transform{{0.0f, -10.0f, 4.0f}};
transform.setYaw(1.5708f); // look along +Y
eye.addComponent<sfs::TransformComponent>(transform);
eye.addComponent<sfs::CameraComponent>();
```

| Field | Meaning |
| --- | --- |
| `active` | Whether it can be the scene's view. Turn one camera off and another on to cut between them. |
| `orthographic` | Parallel projection instead of perspective |
| `fieldOfView` | Vertical, in degrees (perspective) |
| `orthoHeight` | Visible world height (orthographic) |
| `zoom` | Magnification on top of the projection (2: everything twice as big) |
| `nearPlane`, `farPlane` | The visible depth range |
| `focusDistance` | How far ahead the camera's attention is. The sun's shadows are sharpest there, and when there are too many point lights, the ones nearest it are kept. |
| `bloom` and `bloomIntensity`, `bloomThreshold`, `bloomSoftness`, `bloomRadius` | The camera's [bloom](#bloom) |
| `depthOfField` and `focusRange`, `focusBlur`, `focusHyperfocal` | The camera's [depth of field](#depth-of-field) |

Roll is ignored, so the view stays level.

To steer it like an orbit camera, keep a `Camera` as the orbit's state and
write it back to the entity each frame:

```cpp
// m_view: a Camera member, set up once from the entity in onCreate:
//   m_view.setPose(eyePosition, eyeForward, camera.focusDistance);
m_view.yaw += turnInput * dt;
sfs::hierarchy::setWorldMatrix(
    entity(), sfs::TransformComponent(m_view.position(), m_view.orientation()).matrix());
```

`setFollowSceneCamera(false)` stops the render system from following camera
entities, leaving `camera()` to whoever drives it. The editor does this while
editing, so its viewport can look around freely. A scene with no camera entity
renders through `camera()` as set, as before.

## Lighting

`setLighting(SceneLighting)` sets the sun, the ambient level and the sky
colour, in every style, so a day/night controller works the same in a flat,
isometric or 3D game:

| Field | Meaning |
| --- | --- |
| `sun.direction` | Points toward the sun. Z <= 0 means the sun has set, and shadows switch off. |
| `sun.color` | Sunlight tint |
| `sun.diffuseStrength` | How much sun-facing surfaces brighten over ambient |
| `moon.direction` | Points toward the moon (3D only). Z <= 0 means it's below the horizon (the default: no moon). |
| `moon.phase` | How much of its face is lit: 0 new, 0.5 half, 1 full |
| `moon.color`, `moon.diffuseStrength` | Its light's tint and strength under a flat sky (a physical sky works them out) |
| `ambient` | Base brightness. |
| `skyColor` | Background clear colour |
| `cloudCover` | How overcast it is (0..1): dims the sun's and the moon's light. A physical sky draws the clouds (see [Weather](#weather)). |

Once the sun has set, the moon takes over as the light that shines and casts
shadows. It fades in as the sun sinks to about 6 degrees below the horizon,
and it's brighter the fuller and higher it is (`moonlight()` in `lighting.h`
gives its share). A physical sky also draws its disk, lit to its phase, and
glows brighter under it.

The shading:

- Ambient favours surfaces that face up.
- Sides turned away from the sun fall to a "shaded" level.
- Cast shadows pull a surface down to that shaded level.
- Point lights brighten what the sun doesn't reach: the shade by day,
  everything at night. They blend with a brightness cap, so overlapping lamps
  don't blow out.
- Cast shadows come from a sun shadow map (or cascades of them) and, for
  some point lights, cube shadow maps (see [Shadows](#shadows)).

That's the **flat** sky, the default. A 3D scene can have a **physical** sky
instead (`setSkySettings()`, or a visual style's `"sky"`): an atmosphere the
sun lights, behind everything, whose light then lights the scene physically
(the sun's colour through the air, the sky's diffuse light and reflections);
only the sun's and moon's directions, the moon's phase and the cloud cover
are taken from `SceneLighting` then. Its diffuse light is worked out again on a
worker thread as the sun moves, the frame keeping the last until it's ready.
See
[Physical sky and image-based lighting](../visual-styles/index.md#physical-sky-and-image-based-lighting).

### Point lights

Give an entity a `PointLightComponent`. All its units are world units: metres
in a 3D game, and in a 2D game its own units (a flat game's pixels, an
isometric game's tiles across and height levels up). A 2D game's lights sit
on its plane at the entity's position, `offset.z` above it; a flat game's
shine from in front of the plane (at least a quarter of their reach).

```cpp
lantern.addComponent<sfs::PointLightComponent>(
    sfs::PointLightComponent{{1.0f, 0.78f, 0.45f}, 1.2f, 9.0f, {0.55f, 0, 1.8f}});
//                             colour              intensity radius offset
```

| Field | Meaning |
| --- | --- |
| `color`, `intensity` | Its colour and brightness |
| `radius` | How far it reaches; nothing beyond |
| `offset` | Where it is relative to the entity, such as a lantern's flame above its base |
| `castsShadow` | Whether things block it (default on; see [Point-light shadows](#point-light-shadows)) |
| `sourceRadius` | How big the lamp is (its bulb, its flame). Shiny things reflect a highlight that big. |

Point lights light surfaces two ways. Their diffuse light brightens what the
sun doesn't reach (the shade by day, everything at night), softly capped so
overlapping lamps don't blow out. Their highlights reflect off shiny
surfaces: metal and glass, which have little colour of their own to light,
show the lights by reflecting them. A light with a size (`sourceRadius`)
reflects as a sphere that big, not a pinpoint, following Karis's sphere-light
approximation, as Unreal does.

A frame uses up to `kMaxPointLights` (4096) lights. When a scene has more,
the render system keeps the ones nearest the camera target.

Lights are clustered, so many of them stay cheap. Each frame the view is cut
into clusters, screen tiles by depth slices (see `lightClusters.h`). Every light
is listed in the clusters its radius can reach, and a pixel only checks its own
cluster's list. What a light costs depends on how much of the screen it covers,
not on how many lights the scene has. A cluster lists at most 256 lights; past
that, the ones nearest the camera target win.

### Spot lights

A spot light is a point light that shines only in a cone: a headlamp, a
torch, a stage light. Turn on `spot` on a `PointLightComponent` (or add
**Spot Light** from the Hierarchy's menu). It shines along its entity's +x,
so turn the entity to aim it; a child of a moving thing (a vehicle's lamp
part) is carried and turned with it.

| Field | Meaning |
| --- | --- |
| `spot` | Shine in a cone, not all round |
| `innerAngle` | Its full-strength core, degrees from its axis |
| `outerAngle` | Where its light has faded out, degrees from its axis (it fades smoothly between the two) |
| `radius` | How far along the beam it reaches |

```cpp
sfs::PointLightComponent headlight{{1.0f, 0.88f, 0.72f}, 4.0f, 50.0f};
headlight.spot = true;
headlight.innerAngle = 13.0f;
headlight.outerAngle = 24.0f;
lamp.addComponent<sfs::PointLightComponent>(headlight);
```

Everything else is a point light's: its colour and intensity, its fall-off
along its reach, its highlights, the daytime dimming under a physical sky, and
shadows. A spot is clustered by the sphere round its cone (`lightBounds()` in
`lighting.h`), much smaller than its reach for a narrow beam, so it's listed in
fewer clusters than a point light reaching as far. Shading it costs one more
dot product per pixel it might reach, and pixels outside its cone skip it; plain
point lights cost what they did. Its shadows use the point lights' cube shadow
maps and budget, but only objects in its cone are drawn into them, and only into
the faces its cone looks through (a 50 metre headlight usually draws into one or
two of the six). Spots are for 3D scenes: in a 2D game the light shines all
round.

### Emission lights

A material's emission lights the things around it, not just itself: a lava
pool lights the rocks at its edge, a glowing orb the floor beneath it. Each
frame, the render system turns every glowing object's glow into point lights.
They go to the renderer with the scene's own lights and are counted
separately in its stats (`RenderStats::emissionLights`).

- **How bright:** the glow's brightness, its brightest channel averaged over
  the glowing surface. A light glowing at 3 x white lights three times as
  strongly as one at white.
- **How far:** how much glow there is, brightness x glowing area. A bigger
  glowing object reaches farther; twice the size, twice as far.
- **Where and what colour:** the light sits where the glow is (a lamp model
  whose bulb glows is lit from the bulb), in the glow's colour.
- **Big things:** a small object gets one light. A big one is split into
  cells, each with its own light, so its light comes from all of it.
- **Moving glow:** it follows the glow as it changes. A pulsing glow pulses
  its light, and a glow that depends on where the object is (a Position node)
  is worked out where it is.
- **Cut-outs and see-through:** cut-out parts give off nothing, and
  see-through ones (or a tint alpha below 1) give off light by their alpha, as
  they're drawn.

How much a surface glows is worked out on the CPU, at points spread evenly
over each mesh section (taken when the mesh is added; see
`sampleSurface()` in `emissionLights.h`), with the material graph's own CPU
evaluation. A glow that's the same wherever its object is (it reads only UVs,
vertex colours and time) is worked out once a frame per mesh and material,
however many objects share them. A glow that depends on where it's seen from
(a Fresnel rim) is averaged over the directions light leaves the surface in.

Only material graphs give off light; the standard surface has no emission.
Each glowing object decides its own light, on its `MeshRendererComponent`:
`emitsLight` turns it off, `lightStrength` and `lightRange` scale how brightly
and how far, and `lightCastsShadows` says whether things block it. The
Inspector shows these only while one of the object's materials can glow
(`canGlow()`: a node graph whose Emission isn't simply black).

These lights are point lights like any other. They cast shadows (when they're
among the frame's [shadowed lights](#point-light-shadows)), except from the
glowing object itself, and they're as big as the glow (`sourceRadius`), so a
mirror beside a lava pool reflects the lava's glow, not specks of light.

### Bloom

Lit surfaces never go brighter than white (unless a
[tone mapper](../visual-styles/index.md#tone-mapping-and-grading) is on),
but emission can. A material whose
emission goes above 1 glows past its edges onto what's around it, as bright
lights do through a real lens. Ordinary surfaces don't bloom at all.

A single very bright pixel (a glint on something thinner than a pixel)
counts for less as bloom is gathered (Karis's average over its first
halving), so it can't flash a blob of glow around itself; broad bright light
blooms as before. `SFS_FIREFLIES=<light>` marks, for finding such pixels,
each one more than four times as bright as those around it and brighter than
`<light>` magenta (its brightness in green: log2 x 8 + 64), and any that
isn't a number cyan.

Bloom belongs to the camera: the scene camera's `CameraComponent` sets it,
and the editor viewport shows the game camera's even while you look around
freely. A scene without a camera entity uses `setBloomSettings(BloomSettings)`.

| Field | Meaning |
| --- | --- |
| `bloom` | Whether bright light blooms |
| `bloomIntensity` | How strong the glow is (0.6) |
| `bloomThreshold` | How bright light must be to bloom (1, white) |
| `bloomSoftness` | How gently it eases in above the threshold (0.5) |
| `bloomRadius` | How far the glow spreads (1) |

How much each object blooms is up to the object: how bright its emission is,
times its material's **Bloom** (the Material Output's input, 1 by default). At
0 a material never blooms, however bright; above 1 it blooms more than its
brightness alone would.

The scene draws with 4x multisampled anti-aliasing (except on wgpu's OpenGL
fallback): each pixel's coverage is
sampled four times, so edges, and thin things like grass blades, come out smooth
rather than stair-stepped. The samples are averaged (resolved) as the lit pass
ends, and everything after it works on the resolved picture. Depth of field
reads each pixel's first depth sample.

The scene draws into an HDR texture, and beside it the light that blooms (its
colour x each material's Bloom). Bloom then takes the part of that above the
threshold and halves it down a chain of textures, up to six. It adds each one
back onto the next size up, which spreads the glow wide and soft. Finally the
scene and the glow are added together and
[graded](../visual-styles/index.md#tone-mapping-and-grading) (by default only
clamped at white) into the target.

A [visual style](../visual-styles/index.md) can replace the camera's bloom
with its own while it's on.

### Depth of field

With depth of field, what's nearer or farther than the camera's focus blurs,
as through a real lens. It's off by default. Like bloom, it belongs to the
scene camera's `CameraComponent`, and the editor viewport shows it while you
look around freely. The focus is always the view's own: `focusDistance` ahead
of a camera entity, or the editor's orbit target. A scene without a camera
entity has none.

| Field | Meaning |
| --- | --- |
| `depthOfField` | Whether to blur at all |
| `focusRange` | How deep the sharp part around the focus is, in world units (10) |
| `focusBlur` | How blurred the farthest and nearest things get (1) |
| `focusHyperfocal` | The lens's hyperfocal distance, in world units: focused this far or farther, everything beyond the focus is sharp (80; 0: only `focusRange`) |

Depth of field is off on wgpu's OpenGL fallback. Blur grows with distance
beyond the sharp range. It's full one and a half focus
distances past it, and grows by as much toward the camera.

The sharp part also deepens as the focus moves off, as through a real lens.
A thin lens's blur at distance `d`, focused at `s`, goes as
`aperture × |d − s| / d`, which is `focusHyperfocal × |1/s − 1/d|` in shares
of the most that still looks sharp. Each pixel takes the lesser of that blur
and the sharp range's. Close up, where a real lens keeps very little sharp,
`focusRange` still is. Zoomed out to an overview, most of the landscape is
sharp; focused `focusHyperfocal` away or farther, all of it is out to the
horizon. Raise `focusHyperfocal` to keep far views softer for longer.

It runs between the lit pass and bloom (`postFocus.wgsl`), reading the depth
buffer:

1. It halves the scene, and stores beside each pixel how blurred it is.
2. It blurs that with a 32-tap disc as wide as each pixel's blur. Taps only
   count where their own blur reaches, so sharp things don't smear over
   blurred ones.
3. It mixes the result over the full-size scene, by each pixel's blur.

### Haze

`setHazeSettings(HazeSettings)` sets the scene's haze, which Scene Settings
shows as **Haze** and **Haze distance**. It's saved with the scene. Haze fades
far things into the sky colour, as distance does outdoors, and hides where
things stop being drawn, such as grass's draw distance. It's off by default.

| Field | Meaning |
| --- | --- |
| `strength` | How hazed the farthest things get (0: none, 1: fully) |
| `distance` | How far away things are half that hazed |

It's applied as each surface is lit, so hazed things glow (bloom) less too.

### Height fog, ambient occlusion and contact shadows

`setFogSettings(FogSettings)` turns on exponential height fog (mist thicker low
down, coloured by the sky and glowing toward the sun), which takes haze's
place while it's on; `setAmbientOcclusionSettings(AmbientOcclusionSettings)`
turns on screen-space ambient occlusion (the light from all around kept out of
creases and the foot of walls; never the sun's or point lights');
`setContactShadowSettings(ContactShadowSettings)` turns on screen-space contact
shadows (small, sharp sun shadows where things meet the ground, finer than the
shadow maps; under a physical sky). All are off by default, saved with the
scene when they're not, shown in Scene Settings under **Occlusion & Fog**, and
a visual style can replace each. See
[Visual styles](../visual-styles/index.md#ambient-occlusion) for their fields
and how they work. They're a 3D scene's: a 2D scene has none.

### Weather

`setWeather(SceneWeather)` sets the weather, which a game usually drives each
frame, as it does the lighting. How cloudy it is stays the lighting's
`cloudCover`. Scene Settings shows both under **Weather**, and the weather is
saved with the scene when it isn't the default. It's a 3D scene's.

| Field | Meaning |
| --- | --- |
| `rain` | How hard it's raining now (0..1) |
| `wetness` | How wet surfaces are (0..1). It lags the rain as things wet and dry. |
| `puddles` | How full the low spots are (0..1): water standing in a terrain's low spots (see [Puddles](#puddles)) |
| `wind` | The wind the clouds drift on, in metres a second along world X and Y |
| `cloudDarkness` | How heavy the clouds are: white fair-weather cloud (0) to dark rain cloud (1) |

Under a physical sky the clouds are a layer 1.5 km up, made of drifting noise.
At low cover they're scattered cumulus, in the middle broken cloud, and near 1
a grey deck. The sun or moon lights them: tops are white at noon, sides and
undersides glow at sunset, and they stay lit a little after the sun has set
for the ground. At night they're dark. Thick cloud hides the sun, the moon and
the stars. The more overcast it is, the flatter the light gets:

- The sun's light dims, its shadows soften (up to 3x their softness), and
  only clouds' moving shadows take it where they pass.
- The sky's light turns grey and even, and the blue between clouds pales.
- Haze and fog take on the clouds' grey.
- Eyes don't adapt to it the way they adapt to dusk (see the sky's
  `adaptation`), so an overcast noon stays dull and a cloudy night stays
  darker than a clear one.

The background draws the clouds per pixel each frame as they drift. The sky's
reflection textures redraw them each time they've drifted (or the camera's
moved) 30 m, and when the sun moves; the sky's diffuse light accounts for them
on average (in the background, as it does for the sun's moves).

The shaders get the weather as the `Frame` uniform's `weather` (rain,
wetness, puddles, cloud cover) and `weatherWind` (the wind, then how far the
clouds have drifted in km).

#### Rain

While `rain` is above 0:

- **Streaks** fall through the air around the camera: about 40,000 in a
  downpour (many fewer in a drizzle: about 5,000 at rain 0.3), half in a box around the camera and half in a bigger one reaching
  ahead. They fall at about 8 m/s, slanted by the weather's wind, and each is
  drawn as long as its blur while it falls. They drift as far as the wind has
  carried them (the render system keeps that, `RainDrift`, in the `Frame`'s
  `rainDrift`), and the wind they see is eased over about a second, so a gust
  or a change of direction turns them smoothly instead of sliding the whole
  field of drops sideways. Each is at least a pixel wide
  (thinner ones are drawn fainter). The sky lights them (brightest looking
  toward the sun), and so do the point lights around them. At night they're as
  dark as the night is. They fade out about 40 m away and go into the haze, fog
  and veil like any surface.
- **Splashes**: drops land on the ground ahead of the camera (more near it).
  Each throws up a small crown of droplets and a spreading ring, and lasts a
  third of a second. They only land on things that face up.
- **Ripples**: rings spread wherever water stands (puddles, a soaked flat
  stone) and on smooth, flat, up-facing surfaces like a river or a pond.
- **Drips**: in heavy rain (from about 0.4), water runs off the edges of
  roofs, eaves, awnings and tree canopies. Drops gather under the edge, fall
  to the ground and splash there; in a downpour they string out into broken
  trickles. The edges are found on the overhead map (below), not authored:
  wherever what's drawn steps down more than 0.6 m to open ground, and water
  runs to the edge (a roof sloping toward it, a flat one, or something thin;
  not a gable's sloping side). About 4,000 places around the camera each
  look along a line for one, and the ones that find an edge drip from it a
  few times before looking elsewhere. After the rain stops, soaked edges keep
  dripping for a while, a quarter as many and slowly. The ground along the
  drip line, just outside an edge, is soaked and has water standing on it.
  Drips are lit as streaks are, and fade out about 25 m away.
- **The veil**: the falling rain greys the distance, the same at every height,
  toward the fog's colour (the overcast sky at the horizon). A downpour hides
  about half of what's 150 m away, and light rain hardly any.

Nothing falls, splashes or ripples under a roof or a tree. Rain's
*overhead map* says where the sky is open. It's a depth map looking straight
down over a 128 m square ahead of the camera (6 cm a texel), drawn each
rainy frame from the sun's shadow casters (grass left out) and the opaque
things in view that don't cast a shadow (often the ground). It needs the
sun's shadows to be on. Without them, rain falls everywhere and nothing
splashes. Things past the map's edge count as open.

How rain looks is `setRainSettings(RainSettings)`, which a visual style can
replace (its `"rain"` section). It's saved with the scene when it isn't the
default, and it's under **Weather** in Scene Settings.

| Field | Default | Meaning |
| --- | --- | --- |
| `enabled` | true | Whether rain is drawn at all (wet surfaces still are) |
| `density` | 1 | How many streaks fall (1: about 40,000 in a downpour, an eighth of that in a drizzle; up to 2) |
| `length` | 0.5 | How long a streak is, in metres |
| `width` | 2 | How wide a streak is, in millimetres |
| `brightness` | 1 | How bright streaks and splashes are (1: as the sky and lamps light them) |
| `splashes` | 1 | How many drops splash (1: about 8,000 at once in a downpour; 0: none) |
| `ripples` | 1 | How strongly drops ring standing water (0: none) |
| `veil` | 1 | How thick the distant veil is (0: none) |
| `drips` | 1 | How much water drips off edges in heavy rain and after it, and how wet the drip line is (0: none; up to 2) |

Streaks and splashes leave their coverage in the scene's alpha, and temporal
anti-aliasing takes more of the current frame there. Streaks move too fast for
its history, which would otherwise smear them away.

#### Lightning

`strikeLightning(LightningStrike)` sets off a strike (a 3D scene's). A game
does it during storms (CozyFarm's weather does, more at a storm's height).

| Field | Default | Meaning |
| --- | --- | --- |
| `position` | 0, 0, 0 | Where it hits the ground (world). An in-cloud strike flashes in the cloud base above it. |
| `intensity` | 1 | How bright it is (1: a typical strike; 0.5 to 2 for variety) |
| `inCloud` | false | In the cloud only: no channel to the ground |
| `seed` | 0 | Shapes its channel and strokes. Give each strike its own. |

A strike flickers through its strokes (`lightningBrightness()`). A
cloud-to-ground one has 2 to 5 return strokes over 0.15 to 0.5 s, each a flash
dying away in tens of milliseconds, sometimes one held by a continuing
current. An in-cloud one has 3 to 7 softer pulses. Up to 4 strikes flash at
once; the oldest gives way.

Under a physical sky a strike really emits light:

- **The channel** is a line of light from the cloud base (1.5 km up, leaning a
  little) to the ground. Each surface takes its light from where along the
  line it comes from on average, so it's as bright as a line light: about 1 /
  the distance near it, and 1 / the distance squared far off. At a kilometre,
  a typical stroke lights what faces it about as brightly as the sun at noon.
  It gives Lambert diffuse light and a GGX glint, so wet ground and puddles
  catch sharp highlights from it. Within 1.2 km of what the camera looks at,
  it casts a shadow: a point light at that average point, its cube map taking
  one of the point-light shadow slots first, lighting nothing itself (it's
  kept out of the light clusters).
- **The cloud base** glows around the strike (about 1.8 km across, or 2.5 km
  for one in the cloud), even when the strike itself is out of view. The
  background draws the glow on the clouds, mottled by their pattern. Surfaces
  reflect it where their reflection meets the cloud layer, cheaply (the
  reflection textures aren't redrawn), so puddles flash too. It lights the land
  softly from above, the veil of rain and the haze, so distant strikes still
  flicker the landscape. Rain streaks catch both lights.
- **Auto exposure holds** while any strike is going, so a flash brightens the
  scene and the dark between strikes stays as dark as it was.

The channel is drawn too, under any sky. It's a jagged, branching path,
regenerated per strike from its seed (`lightningChannel()`), drawn as
additive ribbons facing the camera, a few pixels wide however far off. They're
far brighter than the scene, so bloom picks them up. The main channel follows
every stroke. Branches are lit by the first stroke and then fade. After each
stroke a faint afterimage fades over about half a second.

`holdLightning(seconds)` holds every strike that long after it began (0: the
first stroke's peak), for screenshots and tests. `frameLightning()` is what the
frame got. The shaders get it as the `Frame` uniform's `lightningLow`,
`lightningHigh`, `lightningLight` and `lightningGlow`. `meshCommon.wgsl` has
`lightningGlowAt(p)` (the glowing clouds' light at a point) and
`lightningCloudsToward(origin, direction)` (the glow seen along a ray), and
`meshLit.wgsl` has `lightningLighting()`.

#### Wet surfaces

`wetness` wets every lit surface the rain reaches: meshes, terrain, grass and
its ground. It follows Lagarde's physically based wet surfaces. Porous things
(rough and not metal, like soil, wood, stone and cloth) soak the water up and
darken, down to half their albedo when soaked. Everything gets glossier: its
roughness drops by 30% to 45%, less on porous things. The lower roughness
gives stronger, sharper reflections of a physical sky through its reflection
textures, and stronger sun and lamp highlights. Up-facing surfaces get all of
the wetness, walls about a third (rain driven against them), and down-facing
ones none. Sheltered spots stay dry, using the same overhead map as the rain.
Soaked flat ground holds a thin film of water, which is smoother still and
picks up ripples.

For shader authors: `meshCommon.wgsl` has `rainRipples(p: vec2f, time: f32,
rain: f32) -> vec2f`. It returns the slope (d height / dx, dy) of rain's
ripples at a world XY point. Tilt an up-facing normal with
`normalize(n - vec3f(slope * strength, 0.0))`. `meshLit.wgsl` has
`surfaceWetness()` (how wet a point is: weather, facing and shelter),
`standingWater()` (how much water stands on it; a terrain's puddles add
theirs through Surface's `water`, and their soaked rims through `soak`),
`wetMaterial()` (the wet albedo, roughness and normal, rippled where water
stands) and `rainExposure()` (0 sheltered to 1 open sky).

### Shadows

`setShadowSettings(ShadowSettings)` configures the sun's shadow map and the
point lights':

| Field | Meaning |
| --- | --- |
| `enabled` | Whether the sun casts shadows |
| `distance` | Radius around the camera target that receives shadows. Larger covers more ground at lower detail. |
| `depthBias` | Removes self-shadowing "acne" |
| `softness` | Filter radius in shadow-map texels (0 = hard edges) |
| `pointLights` | How many point lights cast shadows at once (16; 0 for none, at most 32) |
| `pointResolution` | Each point light's shadow map's face size, in texels (256) |
| `cascades` | 1: one map over `distance` around the camera target (the default); 2 to 4: [cascades](#cascaded-shadows) along the view instead |
| `cascadeDistance` | How far from the camera cascaded shadows reach (200) |
| `cascadeSplit` | Where the view is split between cascades: 1 small near the camera, 0 even (0.75) |

The shadow region follows the camera target in whole-texel steps, so shadow
edges stay still while the camera pans.

### Cascaded shadows

With `cascades` of 2 to 4 (a 3D scene's), the sun's shadow covers the view
from the camera out to `cascadeDistance` in slices, each with a map of its
own (the same size as the one map, side by side in one atlas): small and
sharp near the camera, bigger farther away. The slices split the view by the
practical split scheme (`cascadeSplit` between logarithmic and even);
`cascadeSplits()`, `fitSunCascades()` (`engine/core/rendering/sunShadow.h`)
work them out.

- **Stable**: each cascade covers the sphere around its slice, whose size
  depends only on the view's shape, so it keeps its size as the camera turns;
  and it moves in whole texels, so shadow edges don't shimmer as the camera
  moves.
- **Blended**: a point is shadowed by the finest cascade that holds it,
  blending into the next over the outer tenth of each, and shadows fade out
  over the last fifth of the distance.
- Each cascade's depth bias and normal offset scale with its texel size, and
  `softness` is in each cascade's own texels (the same look, wider far away).
- Things beyond a cascade toward the sun (a tree up the hill at sunset) still
  cast into it, and each cascade draws only the casters that reach into it.

A cascaded atlas is twice the map's size across (a 2048 map: 4096 x 4096 for
3 or 4 cascades). A device that can't make a texture that big (some WebGL-class
OpenGL drivers) gets the one map instead. A 2D game always has the one map.

### Point-light shadows

Point lights cast shadows too, and so do glowing objects' lights. Shadow maps
cost a draw of everything near the light six times over, so only some lights
get them each frame. Of the lights with `castsShadow` whose reach is in view,
up to `pointLights` of them get shadows, the ones nearest what the camera
looks at (counting how far they reach). The rest light without shadows that
frame. Under a physical sky by day, a light too dim to see against daylight
(its intensity x what eyes adapted to the day see lamps by, under 0.2) gets
none at all, and its shadow fades in as dusk falls (full by 0.4). On wgpu's OpenGL fallback there are no point-light shadows (see
[Render backends](../backends/index.md#what-each-graphics-api-can-do)).

Each shadowed light gets a cube shadow map: six faces, each holding how far
the nearest surface is in each direction. Only objects within the light's reach
are drawn into it, and cut-out materials (leaves, fences) cast only where
they're solid. A surface is shadowed where something nearer the light lies in
its direction, softened over a few texels.

A glowing object's light sits inside the object, so the object would block
all of its own light. Instead, anything within a box around the light holding
the glowing surface (`PointLight::shadowNear`) doesn't block it. So
something touching a glowing object can't shadow it either.

## Grass

`GrassComponent` grows grass on an entity's mesh; `GrassModule` (registered by
`StandardScene`) draws it. For the settings, the wind, what flattens it and the
brush, see the editor's [Grass](../../../editor/index.md#grass). Underneath:

- **Ground:** `GrassGround::build()` (`grass.h`) rasterises the mesh from
  above onto a grid over its own XY, with texels about `kGrassTexel` (a
  quarter of a world unit) across. Each texel keeps the height and normal of
  the highest triangle facing up within 60 degrees there. It's built again
  only when the mesh, or how it's turned or scaled, changes. Moving the
  entity, or changing its blades' density, height or variation, grows the
  blades in view again once (see Growing blades); other settings cost nothing.
- **Big meshes:** a mesh more than 2.5 times as wide as
  `GrassModule::windowReach()` (1.2 times the draw distance, plus
  `windowSlack()`: 0.15 of it, at least 8) grows grass only in a window that
  far either side of the camera (`grassWindow()`), so its ground, maps and
  trample map stay small however big the land is. Once the camera is
  `windowSlack()` from the window's middle, a worker thread builds the next
  window around it, and the module swaps it in when it's ready
  (`IRenderer::updateGrassGround`), so there's no hitch. A window's texels sit
  on multiples of `kGrassTexel` from the mesh's origin, and blades are placed
  from the origin too, so they don't move when the window does. A field set
  into a hole in the land (farmland in a landscape, say) meets the land's
  grass with no seam, as long as their edge lies on that lattice. Each window's
  maps start from `paint`; a game's changes to them last as long as the
  window.
- **Maps:** `GrassMaps` holds seven channels over a grid: `Coverage` (how
  much grows), `Length` (how tall it is), `Data0` to `Data3`, which the engine
  gives no meaning to, for a material to read, and `Bounds` (where it may grow
  at all). Values run from 0 to 1. A component's `paint` holds them as saved:
  the grid, plus each changed channel as run-length pairs in base64. With
  nothing painted, grass grows everywhere at full length.
  Grass gives out two ways:
  - **Softly, where `Coverage` falls** (worn or worked ground): thinner and
    shorter over a band about `kGrassEdgeBand` (3) texels inside the fall,
    and where coverage is partial, in clumps with gaps (a worn way's
    surviving tufts) rather than evenly sparse; the clumps' noise is fixed to
    the ground and wanders their edge across the band, so neither the texels
    nor straight edges show. The band is worked out on the CPU as the maps go
    up (`grassEdgeBand()`: a distance from the fall, into the shape image's
    blue), so the GPU reads one texel for it; deep in full grass it does
    nothing more. The grass's green ground under the blades takes their
    density on average, not the clumps, and is green only where that's
    nearly full (from 0.75): smooth and fixed to the ground, so it never
    shimmers as the camera moves, and never green on part-worn ground where
    blades are few (the terrain's own there: a game paints it as worn
    soil).
  - **To the blade, where `Bounds` falls below a half** (a road, a yard, a
    wall): it's read between texels and cut at a half, with no band. Written
    as a distance from the edge (`GrassMaps::boundsValue(distance, texel)`: a
    half at the edge, ramped over two texels either side; take the least of
    it and what's there to keep grass off several things), the edge is drawn
    where it really is, finer than a texel. The editor's brush paints it too.
  A mower, or wear, changes `Coverage` and `Length` and leaves `Bounds` be.
- **Changing them as a game plays:** `GrassModule::maps(entity)` gives the
  live maps. `stamp()` (with `Toward`, `Add`, `Max` or `Min` blending),
  `fill()` and `sample()` take world positions and radii. Only the texels that
  changed go up to the GPU (`IRenderer::updateGrassMaps`). These changes
  aren't saved with the scene: a game saves `GrassMaps::toJson()` itself if it
  wants them kept. Setting the component's `paint` starts the maps over from
  it. For example, a game might lower `Coverage` a little wherever wheels go,
  to wear a path, or write moisture into `Data0` for its grass material to
  turn dry grass yellow. **Write grass only through `maps()`, `stamp()`,
  `fill()` (and `terrainPainted()` after painting the terrain under it):
  anything else won't be seen**, since the blades grown from the maps are kept
  until the module says where they changed.
- **Growing blades:** blades are grown on the GPU (`meshGrassGrow.wgsl`), in
  tiles kept from frame to frame (`GrassTileCache`, `grassTiles.h`). The
  ground is split into fixed tiles, 64 by 64 cells of each lattice (below), so
  3.2 by 3.2 units for the finest at 400 blades a unit. A tile is built
  (`cs_build`) the first time it's in view and near enough: every cell's
  blade worked out from the ground and maps, all that doesn't depend on the
  camera (where it is, its height from the ground, how tall `Length` lets it
  grow, whether `Coverage` lets it grow at all, its ground's slope), and kept
  in 16 bytes. Each frame, the tiles in view and in reach are found on the
  CPU (by box: nearest distance, frustum) and only the camera's part is done
  over their kept blades (`cs_grow`): how many are kept at their distance,
  growing in, width, in view, detail. Wind, trampling and the Data channels
  are applied as they're drawn, so they never build a tile. A tile coming
  into view is built that frame, from the same ground and maps, so it looks
  just as it would have.
  - A tile keeps only the blades ranked low enough to be drawn nearest the
    camera, and a step more: it's built again with more as the camera nears,
    and with fewer once the camera's well away (a few a frame).
  - It's built again where what it's grown from changes. `GrassMaps` notes
    the 16-texel blocks whose `Coverage`, `Length` or `Bounds` changed
    (`takeGrownChanged()`), the module passes them on with the maps
    (`GrassMapsImage::regrow`), and only the tiles over them are built again:
    two edits far apart don't build what's between. Painting the terrain
    under it does the same where its cover changed. A new ground (a big
    mesh's window moving on), new maps, moving the entity, or changing the
    blades' density, height or variation builds them all again.
  - Their blades live in one buffer a field, given out in 64-blade chunks: it
    grows as needed, and tiles not used for 180 frames (or, past 32 MB, the
    least lately used) are let go, up to 128 MB at most.
  - Blades are listed front to back, so the lit pass's depth test hides
    more of the farther ones behind those drawn first rather than shading
    them. The tiles in view come nearest first (by their middles), and their
    blades are listed tile by tile in that order: `cs_count` finds which of
    each tile's blades are drawn and in which list, and counts them;
    `cs_order` adds up the counts in the tiles' order, for where each tile's
    blades go; `cs_grow` grows them into their places, each tile's rows from
    the end nearer the camera. The three lists sit one after the other, each
    as long as it needs (the draws read where each starts), so short grass
    always gets its coarser list.
  - Tiles hidden behind what's drawn opaque (buildings, hills, trees) are
    left out, when the frame has ambient occlusion: its depth pass (at half
    size) runs before the grass grows, and `grassCulling.wgsl` takes the
    farthest of its depths over blocks of the screen, in levels each with
    blocks twice the size. `cs_count` tests each tile's box (its ground's
    height range, with room for its blades to stand, lean and cast their
    shadows) against the blocks it covers, at the finest level where they're
    few: hidden only if it's nearer nowhere than the farthest of what's over
    it. It's this frame's depth, so turning the camera never leaves holes.
    (Being at half size, a gap between opaque things narrower than about two
    pixels can go unseen in it: grass seen only through such gaps, all over
    a tile, could be left out.)
    Not for a tile that reaches behind the camera, nor, with the sun under
    `kGrassLowestCulledSun`, for one near enough to cast shadows (they'd fall
    too far to allow for). `SFS_GRASS_UNCULLED` turns it off, for comparing.
  - `SFS_GRASS_UNCACHED` grows every blade every frame, as before, for
    comparing. `SFS_GRASS_TILES` tints tiles for a moment as they're built:
    blue as they come into view (or need more blades), magenta when built
    again for an edit, so a missed update is easy to see.

  What's grown each frame:
  - Blades are spread over four lattices, each twice as coarse as the one
    before. Each lattice cell holds one blade at a hashed spot, so the grass is
    the same every frame.
  - Grass is at full density near the camera, then fewer blades are kept
    farther out: the share kept falls smoothly with distance (`grassKeptShare()`;
    all of them out to 770 times the finest cells' spacing, at most 0.3 of the
    draw distance, then by a power of 2.3 of the distance), down to none at
    the draw distance (from 0.6 of it on). Each blade has its own fixed rank
    from its cell's hash (the coarser its lattice, the lower), and is kept
    where its rank is below that share. So the blades kept are the same ones
    wherever the camera is: a patch thins out evenly, blade by blade, as the
    camera draws away, with no ring where it changes. A blade near its cut-off
    grows in over a short stretch rather than popping, and where fewer are
    kept blades are drawn wider (up to 4 times), so the field still reads
    full. Each lattice is grown only as far as any of its blades are kept,
    and only over the part of its reach in view (seen from above).
    `SFS_GRASS_LEGACY_THINNING` thins the old way, for comparing: each
    lattice reaching a set share of the draw distance, its blades shrinking
    away at its end.
  - The pass drops blades out of view or where the maps leave none. The rest
    go into three lists by distance, with 4, 2 or 1 segments, and each list is
    one indirect draw. Short grass (under about 0.22 tall, then under 0.1)
    gets fewer segments near the camera too, since it barely curves. Over the
    stretch before each step in detail a blade takes on the coarser blade's
    shape (its vertices slide onto the coarser strip's), so the step doesn't
    show. The cost follows what's on screen, not how big the field is.
- **The ground:** before its blades, the grass draws its mesh again in the
  grass material's colour (shaded as low down in the grass), blended over the
  ground by `Coverage`, so the gaps between blades look like grass while a
  bare path shows the ground's own material. As the blades thin out to none
  (from `kGrassFadeFrom` of the draw distance to it), it takes on how they
  look from afar instead (`fs_ground` in `meshGrass.wgsl`): their tone on
  average, its normal turned toward the camera as theirs are, a grain fixed to
  the world about a pixel fine, and the wind's gusts in step with theirs.
  The grain fades out as pixels grow past 7 to 25 cm and the camera rises
  past 35 to 110 m. From high or far off it would look like paint run toward
  the camera. There the field is smooth colour, with gentle lighter, darker,
  yellower and bluer patches tens of metres across. The ground is drawn at
  exactly its mesh's depth: both vertex stages' positions are `@invariant`,
  with no depth bias. A bias would pull the far ground over the trees and
  streams standing on it, since depth's steps are metres apart a kilometre off.
  It carries on so out to the horizon, with no draw distance of its own, so
  far grass takes the same material, maps data, season, wetness and light as
  the blades. Past the maps (a big mesh's window of them) it covers its
  terrain's layers as much as each is `meadow` (`grassFarGround()`: the
  terrain's splat map is bound with the grass), with the maps' data as they
  hold it on average, handing over over their last 4%. `SFS_GRASS_GROUND_ONLY`
  draws the ground without the blades, to compare the two.
- **Drawing:** each blade is a triangle strip built in the vertex shader
  (`meshGrass.wgsl`): a curve from its root, tapering to a point, bent over
  by its own lean, the wind (`FrameParams::wind`) and trampling. How far
  the wind bends a blade grows with its length: grass 0.35 tall or more sways
  fully, and mown grass hardly moves. It's lit
  by the same fragment stage as everything else, so materials, point lights,
  shadows and bloom all work on grass. There are so many blades, each a few
  pixels across, that what a vertex hands on to its fragments costs as much
  as their lighting: a blade's vertex hands on little (where it is, its
  normal, how far up and across the blade, how shut in and how wet), and its
  fragments read what's the same all over the blade from the blade itself.
  It hands them on taken where the blade covers the pixel (centroid), not at
  the pixel's middle: a far blade is a sliver that often covers some of a
  pixel's samples but not its middle, and its values carried out to there
  could be anything, which showed as single pixels flashing bright.
  What barely changes along a blade is found at its vertices where that
  pays: on the nearest list's blades (big on the screen) what the air does
  and the clouds' shadow; on the rest the clouds' shadow, and past
  `GrassUniforms::vertexSun` (30 m, or the nearest list's end or the blades'
  shadows' reach if farther) the sun's shadows too, a blade at a time over
  the 10 m past it (by a number of its own, so it doesn't swim). Out there
  neither the blades' shadows nor anything else's change much along one.
  A depth pre-pass for the blades can be turned on (`SFS_GRASS_PREPASS=<1-3>`,
  the nearest lists it covers, or `alternate`: two frames on and two off, for
  comparing screenshots). Their depth is drawn first, before the grass's
  ground (`vs_depth`, depth only), and then they're lit only where their
  depth is equal to it, writing none: each pixel shades just the nearest
  blade, and the ground under blades isn't shaded either. Both vertex stages
  place a blade with the same sums (`bladePoint()`) and their positions are
  `@invariant`, so the depths match exactly. Only for an opaque grass
  material (a masked one could cut blades out as it lights them). It's off by
  default: measured over all three lists on an RTX 3070 it saved about 0.4 ms
  of the blades' shading and 0.24 ms of the ground's, but placing the blades
  again cost about 1.05 ms (they're bound by their vertices and tiny
  triangles more than by their pixels, already drawn near to far).
  A material graph's Grass node reads
  the blade's length, how far up it the point is, a random number, and the
  Data channels where it grows (`SurfaceIn::grass` and `grassData`; the
  defaults on anything else).
- **Trampling:** `GrassTrample` is a grid over the field's ground in the
  world. It's pressed by `GrassBender` spheres where they reach into the
  grass, and springs back over `recovery` seconds. Each update the module
  gathers benders (rigid bodies that bend grass, as their colliders, and
  `GrassBenderComponent`s). When the map changes, it sends it to the renderer
  as an image (`IRenderer::updateGrassTrample`).

- **Shadows:** grass grows before the sun's shadow pass. Blades within
  `grassShadowReach()` of the camera cast into the nearest
  `kGrassShadowCascades` (2) cascades: to the second's reach, at most
  `kGrassShadowMostReach` (32), or `kGrassShadowOneMapReach` (8) with one
  shadow map. Further out, a blade's shadow would be less than a texel.
  The grow pass lists those blades' indices as it grows them, so no second
  pass grows them. The shadow pass draws them depth-only, with one indirect
  draw (`meshGrassShadow.wgsl`). Each is two segments along the same curve
  the lit pass draws (`grassBlade.wgsl`), so its shadow sways and flattens
  with it. It's turned square to the sun and drawn 1.6 times wider, so it
  doesn't vanish edge-on or flicker at the map's resolution. It's pushed back
  from the sun by about its width, so a blade isn't shadowed by itself, and it
  thins out toward the end of the reach. `GrassComponent::castsShadows` turns
  it off.
- **Self-shadowing:** the blades are darker toward their roots, where the
  grass around them shuts out the sky's light and half the sun's. This uses
  `SurfaceIn::grass.w`, 0 on anything else, which takes from ambient
  occlusion and the sun's visibility in the lit pass. The ground drawn under
  the blades counts as deep in the grass, a little less so far away. So a
  field reads as deep at any distance, past the reach of its shadows.
  `GrassComponent::selfShadow` sets how strong it is.

- **Measuring:** with `SFS_GRASS_STATS` set (natively), the grow pass counts
  its blades at each step (looked at, kept by distance, covered, in view,
  each list, shadow casters) and the device counts the vertices, triangles
  and fragments the blades' draws shade; the averages are logged every 60
  frames read back, with the tiles' (grown from, built, kept, hidden, and
  their blades' buffer in use and in all). `SFS_GRASS_STATS=2` also counts the fragments drawn at
  each pixel (overdraw; it keeps the GPU from skipping hidden ones early, so
  it's slower).

## Debug settings

`RenderSystem::settings()` exposes ambient, sun strength, shadow, wind and
projection controls as `ModuleSetting`s, so the debug panel shows them like any
other system's settings. Set the `SFS_GPU_TIMING` environment variable to have the
WebGPU renderer time its sun shadow, grass growth, puddle and lit passes with
GPU timestamp queries, where the device has them, and log their averages in
milliseconds every few seconds (the puddles' over the frames they ran in;
0 when they didn't). `stats()` reports the last frame's draw calls,
instances, shadow instances, point lights, and how many of those came from
emission and cast shadows.

`SFS_CPU_TIMING` does the same for the CPU side, logging named scopes'
(`SFS_CPU_SCOPE("name")`, `engine/core/util/cpuTiming.h`) time and call count
every few seconds; nested scopes count inside the ones around them. To catch a
hitch rather than wait for the periodic log, `SFS_SLOW_FRAME_MS=<ms>` (with or
without `SFS_CPU_TIMING`) logs a single "Slow frame" line, with its biggest
scopes, for any frame that takes longer than that end to end.

## Read more

- [Render modules](../render-modules/index.md): writing a module of your own.
- [Render backends](../backends/index.md): the `IRenderer` contract, and
  implementing another backend.
