# 3D rendering

The 3D render path draws a free-camera 3D scene: meshes with real sun shadows,
point lights, and the same lighting model as the isometric renderer. It is a
generic base. It assumes nothing about grids, tiles or voxels, so a game decides
those things for itself.

It follows the same structure as the 2D/isometric path:

| Piece | 2D / isometric | 3D |
| --- | --- | --- |
| Renderer contract | `IQuadRenderer` | `IRenderer3D` |
| Render system | `IsometricRenderSystem` | `RenderSystem3D` |
| Render context | `IsometricRenderContext` | `RenderContext3D` |
| Commands | `AnyRenderCommand` | `AnyRenderCommand3D` |
| Placement | `TransformComponent` | `Transform3DComponent` |

## Setting up a 3D game

The 3D renderer is provided by the WebGPU backend. Link `sfs::engine-webgpu`
and return a `WebGpuRenderBackend` from `makeRenderBackend()`:

```cpp
#include "engine/webgpu/webGpuRenderBackend.h"

std::unique_ptr<sfs::IRenderBackend> MyGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}
```

In your scene, add a `RenderSystem3D` with the backend's renderer and register
the modules you want. `MeshModule` draws mesh entities:

```cpp
#include "engine/runtime/rendering3d/modules/meshModule.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"

void MyScene::onInit()
{
  auto& render = addSystem<sfs::RenderSystem3D>(renderer3D());
  render.withModule<sfs::MeshModule>();
}
```

## Scene services

A scene reaches the backend's renderers through `SceneServices`. Each backend
provides only the renderers it has: the OpenGL backend provides a quad renderer
and a text renderer, and the WebGPU backend provides a 3D renderer.

| Accessor | Returns |
| --- | --- |
| `renderer3D()` | The 3D renderer. Logs and aborts if the backend has none. |
| `tryRenderer3D()` | The 3D renderer, or null. |
| `quadRenderer()` / `tryQuadRenderer()` | The 2D quad renderer, the same way. |
| `textRenderer()` / `tryTextRenderer()` | The text renderer, the same way. |

Asking for a renderer the backend doesn't have is a setup bug, such as a 3D
scene on the OpenGL backend. It fails loudly, like `getSystem()`. Use the
`try` form only in code that adapts to whichever backend it runs on.

## Meshes

A `MeshData` is a CPU-side indexed triangle list. Each vertex has a position,
a normal and a colour. Z is up, and counter-clockwise faces are front faces.
Build meshes from the primitives in `meshPrimitives.h`, and combine several
into one mesh with `meshes::append`:

```cpp
#include "engine/core/rendering/3d/meshPrimitives.h"

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
- Each frame, `Scene3D` takes in the models that have finished
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

Give an entity a `Transform3DComponent` and a `MeshRendererComponent`:

```cpp
sfs::Transform3DComponent transform{{-12.0f, 5.0f, 0.0f}};
transform.setYaw(0.3f);

sfs::Entity barn = scene.createEntity();
barn.addComponent<sfs::Transform3DComponent>(transform);
barn.addComponent<sfs::MeshRendererComponent>(sfs::MeshRendererComponent{barnMesh});
```

A child entity draws relative to its parent; see
[Parents & children](../../core/hierarchy/index.md).

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
[material graph](../../core/rendering-3d/material-graphs/index.md), when its
`shader` is set. `MeshLibrary::material()` loads a material asset by name, and
`MeshLibrary::setMaterial()` makes or changes one from game code.

The renderer builds pipelines per surface: one set for the standard surface,
and one for each graph shader and alpha mode, shared by every material that
uses it. Materials that differ only in their values share pipelines, and
changing a value doesn't rebuild anything. A graph shader that fails to build is
logged once, and its materials draw magenta.

Each frame's `FrameParams3D::time` is the seconds `RenderSystem3D` has been
updated, for animated materials (a graph's Time node). The scene updates every
frame, in a game and in the editor alike, so animated materials move while
editing too.

## Camera

`RenderSystem3D::camera()` returns the scene's `Camera3D`. It is an orbit camera
for a Z-up world: it looks at `target` from `distance` away, turned by `yaw`
around +Z and tilted by `pitch` above the ground. Game code drives these fields,
so a free orbit, a follow camera and a fixed angle are just different ways of
setting them.

```cpp
sfs::Camera3D& camera = render.camera();
camera.target = playerPosition;
camera.yaw += turnInput * dt;
camera.projection = sfs::Camera3D::Projection::Orthographic; // or Perspective
```

`groundForward()` and `groundRight()` give camera-relative movement axes on the
ground. `screenRay(pixel, viewport)` returns a world-space ray for picking.
`Ray3D::intersectPlaneZ` finds where it hits a ground plane.

### Camera entities

A scene made in the editor usually sees through a camera entity instead: an
entity with a `Camera3DComponent` and a `Transform3DComponent`. The camera sits
at the entity's position and looks along its +X axis, with +Z up (the way
`setYaw` turns things). Each frame, `RenderSystem3D` points `camera()` the way
the first active camera entity looks, so you move the view by moving the
entity. It can be a child, e.g. of a vehicle it rides on.

```cpp
sfs::Entity eye = scene.createEntity();
sfs::Transform3DComponent transform{{0.0f, -10.0f, 4.0f}};
transform.setYaw(1.5708f); // look along +Y
eye.addComponent<sfs::Transform3DComponent>(transform);
eye.addComponent<sfs::Camera3DComponent>();
```

| Field | Meaning |
| --- | --- |
| `active` | Whether it can be the scene's view. Turn one camera off and another on to cut between them. |
| `orthographic` | Parallel projection instead of perspective |
| `fieldOfView` | Vertical, in degrees (perspective) |
| `orthoHeight` | Visible world height (orthographic) |
| `nearPlane`, `farPlane` | The visible depth range |
| `focusDistance` | How far ahead the camera's attention is. The sun's shadows are sharpest there, and when there are too many point lights, the ones nearest it are kept. |
| `bloom` and `bloomIntensity`, `bloomThreshold`, `bloomSoftness`, `bloomRadius` | The camera's [bloom](#bloom) |

Roll is ignored, so the view stays level.

To steer it like an orbit camera, keep a `Camera3D` as the orbit's state and
write it back to the entity each frame:

```cpp
// m_view: a Camera3D member, set up once from the entity in onCreate:
//   m_view.setPose(eyePosition, eyeForward, camera.focusDistance);
m_view.yaw += turnInput * dt;
sfs::hierarchy::setWorldMatrix(
    entity(), sfs::Transform3DComponent(m_view.position(), m_view.orientation()).matrix());
```

`setFollowSceneCamera(false)` stops the render system from following camera
entities, leaving `camera()` to whoever drives it. The editor does this while
editing, so its viewport can look around freely. A scene with no camera entity
renders through `camera()` as set, as before.

## Lighting

`setLighting(SceneLighting3D)` sets the sun, the ambient level and the sky
colour. These are the same controls as the isometric `IsometricAmbientLighting`,
so a day/night controller drives either path the same way:

| Field | Meaning |
| --- | --- |
| `sun.direction` | Points toward the sun. Z <= 0 means the sun has set, and shadows switch off. |
| `sun.color` | Sunlight tint |
| `sun.diffuseStrength` | How much sun-facing surfaces brighten over ambient |
| `ambient` | Base brightness. |
| `skyColor` | Background clear colour |

The shading itself is the isometric renderer's model, carried into 3D:
- Ambient favours surfaces that face up.
- Sides turned away from the sun fall to a "shaded" level.
- Cast shadows pull a surface down to that shaded level.
- Point lights brighten what the sun doesn't reach: the shade by day,
  everything at night. They blend with a brightness cap, so overlapping lamps
  don't blow out.

The only difference is the shadow source: a sun shadow map instead of a
heightmap march.

### Point lights

Give an entity a `PointLight3DComponent`. All its units are world units:

```cpp
lantern.addComponent<sfs::PointLight3DComponent>(
    sfs::PointLight3DComponent{{1.0f, 0.78f, 0.45f}, 1.2f, 9.0f, {0.55f, 0, 1.8f}});
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

A frame uses up to `kMaxPointLights3D` (4096) lights. When a scene has more,
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
separately in its stats (`RenderStats3D::emissionLights`).

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

Lit surfaces never go brighter than white, but emission can. A material whose
emission goes above 1 glows past its edges onto what's around it, as bright
lights do through a real lens. Ordinary surfaces don't bloom at all.

Bloom belongs to the camera: the scene camera's `Camera3DComponent` sets it,
and the editor viewport shows the game camera's even while you look around
freely. A scene without a camera entity uses `setBloomSettings(BloomSettings3D)`.

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

The scene draws into an HDR texture, and beside it the light that blooms (its
colour x each material's Bloom). Bloom then takes the part of that above the
threshold and halves it down a chain of textures, up to six. It adds each one
back onto the next size up, which spreads the glow wide and soft. Finally the
scene and the glow are added together, clamped, and written to the target.

### Shadows

`setShadowSettings(ShadowSettings3D)` configures the sun's shadow map and the
point lights':

| Field | Meaning |
| --- | --- |
| `enabled` | Whether the sun casts shadows |
| `distance` | Radius around the camera target that receives shadows. Larger covers more ground at lower detail. |
| `depthBias` | Removes self-shadowing "acne" |
| `softness` | Filter radius in shadow-map texels (0 = hard edges) |
| `pointLights` | How many point lights cast shadows at once (16; 0 for none, at most 32) |
| `pointResolution` | Each point light's shadow map's face size, in texels (256) |

The shadow region follows the camera target in whole-texel steps, so shadow
edges stay still while the camera pans.

### Point-light shadows

Point lights cast shadows too, and so do glowing objects' lights. Shadow maps
cost a draw of everything near the light six times over, so only some lights
get them each frame. Of the lights with `castsShadow` whose reach is in view,
up to `pointLights` of them get shadows, the ones nearest what the camera
looks at (counting how far they reach). The rest light without shadows that
frame.

Each shadowed light gets a cube shadow map: six faces, each holding how far
the nearest surface is in each direction. Only objects within the light's reach
are drawn into it, and cut-out materials (leaves, fences) cast only where
they're solid. A surface is shadowed where something nearer the light lies in
its direction, softened over a few texels.

A glowing object's light sits inside the object, so the object would block
all of its own light. Instead, anything within a box around the light holding
the glowing surface (`PointLight3D::shadowNear`) doesn't block it. So
something touching a glowing object can't shadow it either.

## Debug settings

`RenderSystem3D::settings()` exposes ambient, sun strength, shadow and
projection controls as `ModuleSetting`s, so the debug panel shows them like any
other system's settings. `stats()` reports the last frame's draw calls,
instances, shadow instances, point lights, and how many of those came from
emission and cast shadows.

## Writing a 3D render module

3D modules work like the isometric ones (see [Render modules](../rendering/render-modules/index.md)).
Implement `IRenderModule<RenderContext3D>`, or extend
`CommandModule<RenderContext3D, YourCommand>`, and register it with
`withModule<T>()`. The context provides the camera, both frusta, the lighting,
the frame's point lights and the mesh library. Emit `AnyRenderCommand3D`s and
the render system sorts and submits them.

## Implementing another renderer

`IRenderer3D` is the seam for another graphics API. A frame is `beginFrame()`
with the camera, lighting and shadow projection, then a `drawMesh()` per mesh
with its lit and shadow instances, then `endFrame()`, which renders. See
`WebGpuRenderer3D` for the reference implementation.
