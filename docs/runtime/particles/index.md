# Particles in your game

Getting particles into a rendered game: register the render module, spawn effects,
and make their marks stick. Authoring the effects themselves — the
`ParticleEffectDesc`, shapes, curves — is engine-core; read more in
[Particle engine](../../core/particles/index.md) and [Decals & splatter](../../core/particles/decals/index.md).

## Add the particle module

Particles render through a `ParticleModule` on your `RenderSystem`. It works in
every [style](../rendering/styles/index.md): particles simulate in the game's
own space (a position on its plane and a height), and are drawn as quads facing
the camera. A 2D game's are placed through its `Space2D`, as its sprites are; in
a 3D world a particle's plane is the ground (XY) and its height is up (Z).

```cpp
#include "engine/runtime/rendering/modules/particleModule.h"

auto& particles = render.withModule<sfs::ParticleModule>(assetStore());
particles.registerEffect("spark", makeSparkEffect()); // your authored effect
```

`ParticleModule` is a `ParticleEngine`, so everything below is on the module you
get back from `withModule`. `StandardScene` registers one for you (with stains
on), so a scene made in the editor has it already:

```cpp
auto* particles = getSystem<sfs::RenderSystem>().module<sfs::ParticleModule>();
```

## Spawning

### One-shot burst

```cpp
particles.spawnBurst("splash", position, height);
```

`position` is a point on the game's plane and `height` the ground level the
burst sits on (`0` for a flat game). `spawnBurst` returns `false` if the effect
name isn't registered.

To spawn "on the ground" without looking up the height yourself (once stains
are enabled with a terrain):

```cpp
particles.spawnBurst("splash", pos, particles.groundElevationAt(pos));
```

### Continuous emitter on an entity

Attach a `ParticleEmitterComponent` to emit continuously from a moving entity (a
torch, a wounded actor). The module follows the entity's world transform each
frame, so an emitter that's a child of something moves with it:

```cpp
entity.addComponent<sfs::ParticleEmitterComponent>(
    "smoke",                      // a registered effect with an emitRate
    glm::vec3{0.0f, 0.0f, 0.4f}); // offset from the entity (z: height)
```

Set `enabled = false` on the component to stop emitting; existing particles drain
out naturally. The component is reflected, so emitters can be placed in the
editor.

### Spawn momentum

Inject the impulse of whatever caused the burst (a bullet, an explosion) so the
same effect sprays along the impact direction:

```cpp
sfs::ParticleSpawnParams impact;
impact.velocity = dir * 12.0f;   // added to every particle's launch velocity
impact.aimAlongVelocity = true;  // fan the spray along that direction
particles.spawnBurst("splash", pos, height, impact);
```

## Making decals stick

An effect with `leavesDecal = true` stamps a mark where a particle lands. Marks
are drawn by a `DecalModule` (see
[Render modules](../rendering/render-modules/index.md#decalmodule)). Turn
stains on once, with the decal module as the sink:

```cpp
auto& decals = render.withModule<sfs::DecalModule>(assetStore());
auto& particles = render.withModule<sfs::ParticleModule>(assetStore());

// Land on the scene's solids (SolidObject + BoxCollider2D), clipped to them:
particles.enableStains(decals);

// Or land on a terrain (an ITerrainSurfaceSource, such as a VoxelWorld):
// ground, walls and water.
particles.enableStains(decals, &voxelWorld);
```

With no stains enabled, decal-leaving particles just fall to their spawn plane and
leave nothing. Marks lie on a 2D game's plane, so they show in flat and
isometric games. Tuning what a mark looks like — `DecalSpec`, `GroundBehavior`,
custom shaping — is authoring; see [Decals & splatter](../../core/particles/decals/index.md).

## Performance & budgets

- **Global cap.** `particles.setMaxParticles(n)` bounds total live particles across
  every effect; over the cap, new emissions are dropped gracefully so stacked
  effects can't run the frame cost away. Each effect also has its own
  `maxParticles` capacity per live occurrence.
- **See-through.** Particles are unlit and see-through (or glowing, for
  `BlendMode::Additive`), so they draw after everything solid, back to front.
- **Bounded marks.** `decals.setMaxDecals(n)` caps how many marks are kept (the
  oldest go first), and `decals.setCoverage(cellSize, perCell)` keeps a spot
  that's hit again and again from piling up marks.

## Module API

| Call | Purpose |
| --- | --- |
| `registerEffect(name, desc)` | register or replace an effect by name |
| `spawnBurst(name, position, height, spawn={})` | one-shot burst |
| `enableStains(decals, terrain = nullptr)` | make decal effects land and stick |
| `groundElevationAt(position)` | surface height for a terrain-aware spawn |
| `setMaxParticles(n)` | global live-particle ceiling |
| `liveParticleCount()` | current live count (debug / HUD) |
| `hasEffect(name)` / `effect(name)` / `effectNames()` | inspect registered effects |

`ParticleModule` is a [render module](../rendering/render-modules/index.md);
`ParticleEmitterComponent` is the continuous entity-bound emitter component.
