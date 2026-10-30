# Styles

A game is drawn in one of three styles. They aren't separate engines or render
paths: all three use the same components, the same `RenderSystem` and the same
renderer, with the same lights, shadows, particles and UI. What differs is
where a game's positions sit in the world.

| Style | Positions | Seen |
| --- | --- | --- |
| **Flat 2D** | On a plane, in the game's units (often pixels), plus a height | Straight on, orthographic: platformers, top-down games |
| **Isometric 2D** | On a tile grid (x, y in tiles), plus a height in levels | As a true orthographic isometric picture, with real depth |
| **3D** | In the world, Z up, in metres | Through any camera you place, perspective or orthographic |

A 2D game's entities stay in the game's own space: `TransformComponent`'s
`position2D()` is where it is on the plane, and `position.z` is its height
above it. A `Space2D` turns those into world positions when things are drawn.
A 3D game has no `Space2D`; its transforms are world positions.

## Space2D

`Space2D` (`engine/runtime/rendering/space2D.h`) says where a 2D game's plane
sits in the world. Give one to the render system, and its sprites, particles,
decals, voxel terrain, point lights and camera are all placed through it:

| Field | Style | Meaning |
| --- | --- | --- |
| `projection` | both | `Space2D::Projection::Flat` or `Isometric` |
| `pixelsPerUnit` | both | Sprite pixels per world unit. A flat game in pixels keeps it at 1. |
| `yDown` | flat | The game's y runs down the screen, as screen pixels do |
| `ySort` | flat | Things lower on the plane draw in front (a top-down game) |
| `tilePixels` | isometric | A tile's diamond, in sprite pixels (e.g. 32 x 16) |
| `heightPixels` | isometric | One unit of height, in sprite pixels |

`place(position, height, layer)` gives the world point for a point of the
plane, and `unplace(world, height)` goes back, for what's under the mouse:

```cpp
const sfs::Space2D& space = *render.space2D();
const glm::vec2 world(render.screenRay(mousePixel).origin);
const glm::vec2 onPlane = space.unplace(world, 0.0f); // at ground height
```

**Flat:** the plane is the world's XY plane; a height lifts things up the
screen. `RenderLayerComponent` layers order sprites front to back (higher in
front), and `ySort` orders them by y within a layer.

**Isometric:** the plane is a grid of 1 x 1 tiles drawn as diamonds (tile x
runs down-right, tile y down-left) and heights lift things up the screen. It's
the picture an orthographic camera looking down at the grid would see, depth
and all, so the depth buffer puts things in front of what they should be: a
standing sprite in front of the blocks behind it, a block's faces in front of
those behind them. There's no sorting to get wrong.

## A flat 2D game

```cpp
#include "engine/runtime/rendering/modules/decalModule.h"
#include "engine/runtime/rendering/modules/particleModule.h"
#include "engine/runtime/rendering/modules/spriteModule.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderSystem.h"

void MyScene::onInit()
{
  // Positions in pixels, y running down the screen.
  auto& render = addSystem<sfs::RenderSystem>(renderer());
  sfs::Space2D space;
  space.yDown = true;
  render.setSpace2D(space);

  render.withModule<sfs::SpriteModule>(assetStore());
  auto& decals = render.withModule<sfs::DecalModule>(assetStore());
  render.withModule<sfs::ParticleModule>(assetStore()).enableStains(decals);
  render.withModule<sfs::UiModule>(*this);

  // A camera one window-height of pixels tall.
  sfs::CameraComponent lens;
  lens.orthographic = true;
  lens.orthoHeight = 720.0f;
  createEntity()
      .addComponent<sfs::TransformComponent>(glm::vec2{0.0f, 0.0f})
      .addComponent<sfs::CameraComponent>(lens);

  // A sprite: frame 3 of a sprite-sheet texture asset.
  sfs::SpriteComponent sprite;
  sprite.image = "asset:7b957a18b3f06184fe71ec1a0c92e581";
  sprite.frame = 3;
  createEntity()
      .addComponent<sfs::TransformComponent>(glm::vec2{100.0f, 40.0f})
      .addComponent<sfs::SpriteComponent>(sprite);
}
```

`samplePlatformer` is a complete flat game set up this way. Movement and
collision on the plane come from `MovementSystem` and `CollisionSystem`
([Runtime systems](../../systems/index.md)) or your own systems.

## An isometric 2D game

The same, with an isometric `Space2D`:

```cpp
sfs::Space2D space;
space.projection = sfs::Space2D::Projection::Isometric;
space.tilePixels = {32.0f, 16.0f}; // a 2:1 diamond
space.heightPixels = 8.0f;
space.pixelsPerUnit = 32.0f;       // world units of a tile's width
render.setSpace2D(space);
```

Sprites stand upright on the grid, leaning back as a real one would so their
tops are in front of what's behind them. Voxel terrain (`VoxelModule`, see
[Voxel world](../../voxel/index.md)), particles and decals are placed on the
grid the same way. `sampleGame` is the isometric sample: voxel terrain, lamps,
blood and embers.

## A 3D game

No `Space2D`: positions are world positions, Z up, in metres. Register the
modules you want (`MeshModule` for `MeshRendererComponent` entities, and
`GrassModule`, `SpriteModule`, `ParticleModule`, `UiModule`...), and see
through a camera entity:

```cpp
auto& render = addSystem<sfs::RenderSystem>(renderer());
render.withModule<sfs::MeshModule>();
render.withModule<sfs::UiModule>(*this);

sfs::TransformComponent eye{glm::vec3{-10.0f, -10.0f, 8.0f}};
eye.rotation = sfs::views::looking(45.0f, 30.0f); // heading 45°, 30° down
createEntity()
    .addComponent<sfs::TransformComponent>(eye)
    .addComponent<sfs::CameraComponent>();
```

In a 3D world, sprites stand upright and turn to face the camera
(`SpriteModule::Facing`), and particles simulate on the ground plane (XY) with
their height up Z.

**3D isometric** is a 3D game seen through an orthographic camera at
`views::isometric()`: real meshes, lit and shadowed, from a fixed isometric
angle. `views.h` has the presets:

| Preset | Rotation |
| --- | --- |
| `views::looking(yaw, pitch)` | Heading `yaw` degrees around +Z, looking `pitch` degrees down |
| `views::flat()` | Straight at the XY plane from +Z, +X right and +Y up the screen |
| `views::isometric(yaw = 225)` | Looking down 30 degrees, so square tiles show as 2:1 diamonds |

## The camera

Every style sees through the scene's first active camera entity
(`CameraComponent` + `TransformComponent`). In a 2D game the render system
looks straight at the plane, centred on the camera entity's `position2D()`;
make it orthographic and set `orthoHeight` to how much of the world shows
(in a flat game in pixels, the window height shows a pixel per pixel).
`zoom` magnifies on top. Add a `CameraFollowComponent` to follow something
and shake (see [Camera system](../../systems/camera-system/index.md)); it
works the same in every style.

## In a scene file

A scene file carries its style in its render settings, so the editor and an
exported game draw it the same way:

```json
"render": {
  "style": { "kind": "isometric", "pixelsPerUnit": 32, "tilePixels": [32, 16],
             "heightPixels": 8, "yDown": false, "ySort": false },
  "lighting": { ... }, "shadows": { ... }, "camera": { ... }
}
```

`"kind"` is `"3d"`, `"flat"` or `"isometric"`; the other keys are the
`Space2D` fields. A file without `"style"` loads as 3D. In the editor, pick
the style in Scene Settings → **Style** (see the
[editor](../../../editor/index.md#styles-2d-and-3d)).
