# Debug drawing

Lines drawn over the scene while a game is made: every collider, or anything
game code wants to see (a route, a ray, a clearance box). They are a *debug
feature*: they work in the editor, in Debug builds and in development exports,
and never in a game exported for release.

## Debug features

One switch covers every debug feature (`engine/core/util/debugFeatures.h`):

| Where                                                   | Debug features |
| ------------------------------------------------------- | -------------- |
| The editor (editing and Play)                           | on             |
| A Debug build of the engine                             | on             |
| A development export (`SfsEditor --export --development`, or the export window's *Development build*) | on |
| A release export                                        | off, whatever its scenes or environment say |

The player logs which it is as it starts (`Debug features: on (development
build)` or `off (release build)`). Configuring the engine with
`-DSFS_DEBUG_FEATURES=OFF` compiles them out altogether. Engine code adding a
debug feature checks `sfs::debugFeatures::enabled()` first.

Besides the drawing below, `SFS_CHECK_TRANSFORMS=1` checks the cached world
matrices against fresh ones every frame (see
[Parents & children](../../../core/hierarchy/index.md#world-matrices)).

## Drawing colliders

Tick **Scene Settings > Debug > Draw Colliders**. The scene saves it
(`"debug": {"drawColliders": true}` in its render settings), and every 3D
collider (box, sphere, capsule) and a 2D game's `BoxCollider2D` is drawn as
lines, in the viewport and in the game: where its entity's world transform
puts it, scaled as the physics bakes it. Solid colliders are green, triggers
orange. The lines are hidden behind what's in front of them, and drawn in one
batch, however many there are.

Two overrides, for test runs and development keys:

- `SFS_DRAW_COLLIDERS=1` (or `0`) in the environment turns it on (or off)
  over the scene's setting.
- Game code: `debugDraw().forceColliders(true)` (or `false`; `std::nullopt`
  goes back to the scene's setting).

In a release export the tick, the variable and the call do nothing (the log
says so once).

The selected entity's colliders are always outlined in the editor's viewport,
with the setting on or off.

## Your own lines

Draw with the debug drawing [facility](../../facilities/index.md),
`debugDraw()`, on every system, service, game object and scene: it adds to the
frame's `DebugLines` while updating, and the next frame draws them and empties
them, so draw every frame to keep a shape up.

```cpp
const sfs::facility::DebugDraw lines = debugDraw();
if (!lines.enabled()) // debug features off: skip working the lines out
  return;
lines.line(from, to, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));               // a segment
lines.box(placed, centre, halfSize, sfs::facility::DebugDraw::pack(color)); // a box
lines.sphere(placed, centre, radius, packed);
lines.capsule(placed, centre, radius, halfLength, packed);              // along z
```

While debug features are off (a release export) `debugDraw()` draws nothing
and its calls compile away; it never warns.

`placed` is a rigid matrix (turn and move, no scale); colours are as seen
(sRGB), packed RGBA8. Lines are a pixel wide and unlit; the frame's exposure
is undone for them, so they show about as coloured.
