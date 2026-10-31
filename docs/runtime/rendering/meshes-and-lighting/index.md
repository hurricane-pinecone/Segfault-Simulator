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

Objects sharing a mesh and materials draw as one instanced batch. See-through
things (glass materials, and objects with a faded tint) draw after everything
solid, back to front, one at a time, so keep their numbers modest.

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
    "layers": [
      {"name": "Grass", "baseTexture": "asset:<id>", "normalTexture": "asset:<id>",
       "roughnessTexture": "", "tint": [1, 1, 1], "tileSize": 3,
       "roughness": 0.95, "normalScale": 0.8, "grass": 1},
      {"name": "Dirt", "baseTexture": "asset:<id>", "normalTexture": "asset:<id>",
       "tileSize": 2.5, "roughness": 0.92, "grass": 0}
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

| Material field | Meaning (default) |
| --- | --- |
| `splatMap` | The splat map ("": the first layer only). Read as data, sampled smoothly, clamped at its edges |
| `blendSharpness` | 0 a soft cross-fade between layers; toward 1 a crisp edge where the higher layer pokes through (0.5) |
| `antiTiling` | Break repeats up (true) |
| `variation` | How much the brightness drifts over tens of metres (0.15) |

How it draws (`meshTerrainSurface.wgsl`): each layer's textures lie over the
world's XY from above, `tileSize` across, so ground of any size keeps its
texel size (a mesh's own UVs place only the splat map). With anti-tiling, each
layer is sampled at three randomly shifted and turned copies on a hexagonal
grid and blended, sharpened toward the nearest (Mikkelsen's hex-tiling, after
Heitz and Neyret), with its normals turned to match, so a 200 m field shows
no repeats; and a gentle brightness drift (value noise over 8 to 25 m) keeps
it from looking uniform. Layers with no weight at a point aren't sampled
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

**Grass on it.** `GrassModule` grows grass on a terrain (the mesh's first
material, as drawn now, so a style's variant counts) only as much as its
layers' `grass` lets it, x its own maps: painting dirt makes it bare there. The
maps a game reads and wears aren't changed by that; `GrassModule::terrainPainted()`
grows it again after the splat map changes.

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
| `depthOfField` and `focusRange`, `focusBlur` | The camera's [depth of field](#depth-of-field) |

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
| `cloudCover` | How overcast it is (0..1): dims the sun's and the moon's light, hides the stars and the moon's disk. The clouds themselves aren't drawn yet. |

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

Depth of field is off on wgpu's OpenGL fallback. Blur grows with distance
beyond the sharp range. It's full a whole focus
distance past it, and grows by as much toward the camera.

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

### Height fog and ambient occlusion

`setFogSettings(FogSettings)` turns on exponential height fog (mist thicker low
down, coloured by the sky and glowing toward the sun), which takes haze's
place while it's on; `setAmbientOcclusionSettings(AmbientOcclusionSettings)`
turns on screen-space ambient occlusion (the light from all around kept out of
creases and the foot of walls; never the sun's or point lights'). Both are off
by default, saved with the scene when they're not, shown in Scene Settings
under **Occlusion & Fog**, and a visual style can replace either. See
[Visual styles](../visual-styles/index.md#ambient-occlusion) for their fields
and how they work. They're a 3D scene's: a 2D scene has neither.

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
frame. On wgpu's OpenGL fallback there are no point-light shadows (see
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
  entity, or changing its grass's settings, costs nothing.
- **Maps:** `GrassMaps` holds six channels over a grid: `Coverage` (how much
  grows), `Length` (how tall it is), and `Data0` to `Data3`, which the engine
  gives no meaning to, for a material to read. Values run from 0 to 1. A
  component's `paint` holds them as saved: the grid, plus each changed channel
  as run-length pairs in base64. With nothing painted, grass grows everywhere
  at full length.
- **Changing them as a game plays:** `GrassModule::maps(entity)` gives the
  live maps. `stamp()` (with `Toward`, `Add`, `Max` or `Min` blending),
  `fill()` and `sample()` take world positions and radii. Only the texels that
  changed go up to the GPU (`IRenderer::updateGrassMaps`). These changes
  aren't saved with the scene: a game saves `GrassMaps::toJson()` itself if it
  wants them kept. Setting the component's `paint` starts the maps over from
  it. For example, a game might lower `Coverage` a little wherever wheels go,
  to wear a path, or write moisture into `Data0` for its grass material to
  turn dry grass yellow.
- **Growing blades:** nothing is stored per blade. Every frame a compute pass
  (`meshGrassGrow.wgsl`) grows the blades around the camera:
  - Blades are spread over four lattices, each twice as coarse as the one
    before and reaching further (0.2, 0.4, 0.65 and 1 of the draw distance).
    Each lattice cell holds one blade at a hashed spot, so the grass is the
    same every frame.
  - Grass is at full density near the camera, thinning to a quarter, then a
    sixteenth, further out, with coarser blades drawn wider. Each blade
    shrinks away before its lattice's reach ends rather than popping.
  - The pass drops blades out of view or where the maps leave none. The rest
    go into three lists by distance, with 5, 2 or 1 segments, and each list is
    one indirect draw. Short grass (under 0.22 tall, then under 0.1) gets
    fewer segments near the camera too, since it barely curves. The cost
    follows what's on screen, not how big the field is.
- **The ground:** before its blades, the grass draws its mesh again in the
  grass material's colour (shaded as low down in the grass), blended over the
  ground by `Coverage`. The gaps between blades, and the field past the draw
  distance, then look like grass, while a bare path shows the ground's own
  material.
- **Drawing:** each blade is a triangle strip built in the vertex shader
  (`meshGrass.wgsl`): a curve from its root, tapering to a point, bent over
  by its own lean, the wind (`FrameParams::wind`) and trampling. How far
  the wind bends a blade grows with its length: grass 0.35 tall or more sways
  fully, and mown grass hardly moves. It's lit
  by the same fragment stage as everything else, so materials, point lights,
  shadows and bloom all work on grass. A material graph's Grass node reads
  the blade's length, how far up it the point is, a random number, and the
  Data channels where it grows (`SurfaceIn::grass` and `grassData`; the
  defaults on anything else).
- **Trampling:** `GrassTrample` is a grid over the field's ground in the
  world. It's pressed by `GrassBender` spheres where they reach into the
  grass, and springs back over `recovery` seconds. Each update the module
  gathers benders (rigid bodies that bend grass, as their colliders, and
  `GrassBenderComponent`s). When the map changes, it sends it to the renderer
  as an image (`IRenderer::updateGrassTrample`).

Grass receives shadows but doesn't cast them.

## Debug settings

`RenderSystem::settings()` exposes ambient, sun strength, shadow, wind and
projection controls as `ModuleSetting`s, so the debug panel shows them like any
other system's settings. `stats()` reports the last frame's draw calls,
instances, shadow instances, point lights, and how many of those came from
emission and cast shadows.

## Read more

- [Render modules](../render-modules/index.md): writing a module of your own.
- [Render backends](../backends/index.md): the `IRenderer` contract, and
  implementing another backend.
