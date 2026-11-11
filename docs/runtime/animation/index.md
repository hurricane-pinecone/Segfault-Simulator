# Skeletal animation

Characters and other rigged models move with skeletal animation: a model's
joints are posed by animation clips, and its skinned meshes deform with them
on the GPU. Make the animations in Blender (or take Mixamo animations through
Blender) and export glTF 2.0 (`.glb`): the armature, the skin weights, and each
action as a named animation (Blender: *Animation > Actions* or *NLA strips*
exported as separate animations).

## What comes in

Importing a `.glb` (see [Meshes, materials & lighting](../rendering/meshes-and-lighting/index.md))
brings, besides its meshes and materials:

- **Skins.** A skinned mesh keeps its skin: its joints (by node name), each
  joint's inverse bind matrix, and each vertex's first four joints and weights
  (`JOINTS_0`/`WEIGHTS_0`, normalised to sum to 1).
- **Clips.** Each glTF animation becomes a clip with its name and length:
  translation, rotation and scale channels on the model's nodes, with linear
  (rotations slerped), step or cubic spline keys. Everything is moved into the
  engine's Z-up axes and the model's import scale, as the meshes are.
- **The joints as entities.** A model is a prefab: its node tree, each node an
  entity. A model with a skin always comes in as that tree (even with one
  mesh), so its joints are entities named as in Blender (`hips`, `arm-left`...).
  Anything parented to a joint follows it as it moves: a tool in a hand, a hat
  on a head.

The model's clips are listed in the Inspector when the model is selected in
the Assets panel. Not supported: morph targets (shape keys) and their weight
channels, more than four influences a vertex (`JOINTS_1`), and skins whose
joints have the same name as other nodes of the model.

## Playing clips: AnimatorComponent

Put an `AnimatorComponent` on the model's root entity (any entity above both
its joints and its skinned meshes). Its fields are what it starts with when
the scene plays:

| Field | |
| --- | --- |
| `clip` | The clip it plays, by name (empty: none, the model as bound) |
| `speed` | How fast (1 as made, 0 held, negative backwards) |
| `loop` | Whether the clip starts again at its end, or stops there holding its last pose |
| `playing` | Whether its time runs |
| `time` | Where it is, 0 at the clip's start to 1 at its end; kept up to date as it plays, and setting it seeks (start two characters at different times so they don't walk in step) |
| `fade` | How long switching `clip` cross-fades, in seconds |
| `source` | The model asset (`asset:<id>`) whose clips it plays; empty: its skinned meshes' own model. Another model's clips play on this one's joints by name (a shared animation library for characters with the same skeleton) |

Game code drives it through `AnimationSystem` (what needs its clips or
starts one) and the component's fields (the rest). The component is plain
data: its hidden fields (`playback`, `finished`...) are the system's to keep.

```cpp
auto& animation = registry->getSystem<sfs::AnimationSystem>();
auto& animator = entity.getComponent<sfs::AnimatorComponent>();
animation.play(entity, "walk", 0.25f); // cross-fade to walk over a quarter second
animator.speed = walkSpeed / 1.4f;
if (animator.clip == "pick-up" && animator.finished)
  animation.play(entity, "idle", 0.2f);
float seconds = animation.length(entity, "sprint");
animator.playing = false;              // hold the pose; true carries on
animator.time = 0.5f;                  // seek
animation.restart(entity);             // the clip playing, from its start
```

`play()` with a fade blends the clip in while the others fade out; playing
again before a fade ends keeps the clips fading out (up to four at once), so
quick changes stay smooth. A clip's length and the clip names are known once
its model has loaded (the first update after the scene starts, in a game).
Animation events are not supported yet.

## AnimationSystem

Every scene has an `AnimationSystem` (StandardScene adds it). Each update it:

1. moves each animator's time on by the update's seconds times its `speed`
   (times `AnimationSystem::setTimeScale()`), or not at all while the game is
   paused (`Registry::setPaused()`); `speed` 0 holds one;
2. samples its clips and blends them by their cross-fade weights. The nodes its
   clips move start from their rest pose (as the model was placed), so a
   channel one clip has and the next doesn't goes back to rest;
3. sets the joints' `TransformComponent`s (while the game plays);
4. works out each skinned mesh's joint palette (`inverse(mesh) * joint *
   inverseBind`, all relative to the animator) and posed bounds, and hands
   them to the scene's `MeshLibrary` (`setSkinPose`), keyed by the mesh's
   entity.

An animator none of whose meshes was drawn since its last pose (off screen and
out of every shadow) is posed only every 8th update; its time runs anyway.

## GPU skinning

Each skinned mesh entity gets its own copy of the mesh on the GPU
(`IRenderer::createSkinnedMesh`): its own vertex buffer, sharing the original's
indices and materials. In a frame it's drawn in after a new pose, a compute
pass (`skinning.wgsl`, before every other pass) deforms the original's
vertices into it: positions and normals by the weighted sum of their joints'
palette matrices, the palettes of every character drawn that frame in one
storage buffer. From then on the copy is an ordinary mesh, so every pass draws
it deformed with no shader variants: lit, the sun's shadow cascades, point
light shadows (where a skinned copy always counts as moving, never baked into
a light's still cube), the depth and normals pass (ambient occlusion, contact
shadows, grass occlusion), and rain's overhead map. Meshes that aren't skinned
cost exactly what they did.

A character costs about 1 µs of GPU time a frame it moves (two meshes of a
Kenney mini character on an RTX 3070), plus its own vertex buffer (44 bytes a
vertex). A character holding still isn't deformed again.

Culling uses the pose's bounds: each joint's box around the vertices it moves
(found at import), moved by its palette matrix, so a stretched arm is never
culled. Picking, grass bending and deformers read the pose too: a ray hits
the posed triangles (skinned on the CPU when asked), and a deformer presses
the ground under the posed mesh (a footprint where a foot is down).

The compute pass runs on every backend: Vulkan, D3D12, Metal, wgpu's OpenGL
fallback and the browser's WebGPU. If it doesn't build, skinned meshes draw as
bound (logged).

## In the editor

A model with animation clips placed in a scene or a prefab (dragged in from
the Assets panel) gets an Animator on its root, playing its `idle` clip (any
case), or its first if it has no `idle`. It's an ordinary component: change
its clip, or remove it. Models without clips don't get one.

The Inspector shows an Animator with a **Clip** dropdown of its model's clips,
and a **Source** dropdown of the project's models. While editing, the
viewport shows the pose at `time` without moving the joints' transforms (so
nothing is saved as changed): drag **Time** to scrub, or press **Play** next
to *Preview* to play the clip without entering Play mode (**Stop** puts its
time back). Only skinned meshes show a preview; things parented to joints
follow them once the game plays.
