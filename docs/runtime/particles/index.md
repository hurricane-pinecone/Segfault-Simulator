# Particles in your game

Getting particles into a rendered game: register the render module, spawn effects,
and make their decals stick. Authoring the effects themselves — the
`ParticleEffectDesc`, shapes, curves, prefabs — is engine-core; read more in
[Particle engine](../../core/particles/index.md) and [Decals & splatter](../../core/particles/decals/index.md).

## Add the particle module

Particles render through a `Particles<Context>` module on your render system, where
`Context` is that system's render context type (`IsometricRenderContext` or
`FlatRenderContext`); the type selects the matching behaviour:

```cpp
auto& particles = renderSystem.withModule<sfs::Particles<Context>>();
particles.registerEffect("spark", makeSparkEffect()); // your authored effect
```

Everything below is on the module you get back from `withModule`.

## Built-in prefabs

The engine ships ready-made effects so you get good results with no art — colour
does the work. They are ordinary `ParticleEffectDesc` builders you register as-is
or copy and tweak.

```cpp
#include "engine/runtime/particles/prefabs.h"

// Four-layer blood, registered as "<prefix>_mist/_spray/_gobs/_drip":
sfs::registerBloodEffects(particles);          // default red, prefix "blood"
sfs::registerBloodEffects(particles, "ichor",  // a second colour
                          glm::vec3{0.15f, 0.55f, 1.0f},
                          glm::vec3{0.0f, 0.08f, 0.35f});

// A continuous additive ember drift, for an emitter on a light:
particles.registerEffect("embers", sfs::emberEffect());
```

The four blood layers (`bloodMistEffect`, `bloodSprayEffect`, `bloodGobsEffect`,
`bloodDripEffect`) are meant to be fired together at one point for a layered burst;
spray, gobs, and drip leave decals. `recolourBlood(desc, hi, lo)` returns a colour
variant (its decals follow the particle colour).

## Spawning

### One-shot burst

```cpp
particles.spawnBurst("blood", worldPos, elevation);
```

`elevation` is the ground level the burst sits on (isometric); pass `0` on the flat
path. `spawnBurst` returns `false` if the effect name isn't registered.

To spawn "on the ground" without looking up the height yourself (isometric, once
stains are enabled):

```cpp
particles.spawnBurst("blood", pos, particles.groundElevationAt(pos));
```

### Screen-space burst

Effects authored with `space = SimulationSpace::Screen` simulate in screen pixels
and draw as a flat overlay (no world occlusion) — good for UI hit-sparks:

```cpp
particles.spawnScreenBurst("ui_spark", screenPixel);
```

### Continuous emitter on an entity

Attach a `ParticleEmitterComponent` to emit continuously from a moving entity (a
torch, a wounded actor). The module follows the entity's transform each frame:

```cpp
entity.addComponent<sfs::ParticleEmitterComponent>(
    "embers",              // a registered effect with an emitRate
    glm::vec2{0.0f, 0.0f}, // world offset from the entity
    0.4f);                 // height above the ground
```

Set `enabled = false` on the component to stop emitting; existing particles drain
out naturally.

### Spawn momentum

Inject the impulse of whatever caused the burst (a bullet, an explosion) so the
same effect sprays along the impact direction:

```cpp
sfs::ParticleSpawnParams impact;
impact.velocity = dir * 12.0f;   // added to every particle's launch velocity
impact.aimAlongVelocity = true;  // fan the spray along that direction
particles.spawnBurst("blood", pos, elevation, impact);
```

## Making decals stick

An effect with `leavesDecal = true` stamps a permanent mark where a particle lands.
Turn the feature on once, per particle module, with `enableStains` — it wires both
the collision (what particles hit) and the decal sink (where marks are stamped) for
your render path:

```cpp
// Isometric: stick to the terrain heightfield (walls, ground, water).
particles.enableStains(&yourTerrain); // an ITerrainSurfaceSource

// Flat: stick to the scene's solids (SolidObject + BoxCollider2D).
particles.enableStains();
```

With no stains enabled, decal-leaving particles just fall to their spawn plane and
leave nothing. Tuning what a mark looks like — `DecalSpec`, `GroundBehavior`, custom
shaping — is authoring; see [Decals & splatter](../../core/particles/decals/index.md).

## Performance & budgets

- **Global cap.** `particles.setMaxParticles(n)` bounds total live particles across
  every effect; over the cap, new emissions are dropped gracefully so stacked
  effects can't run the frame cost away. Each effect also has its own
  `maxParticles` capacity per live occurrence.
- **World vs screen.** `SimulationSpace::World` particles are occluded by terrain
  and depth-sorted; `Screen` particles draw flat on top. Use `Screen` for UI.
- **Decals are cheap to keep.** Settled marks live in persistent GPU buffers, so a
  stained level costs nothing extra to re-draw as the camera moves.

## Module API

| Call | Purpose |
| --- | --- |
| `registerEffect(name, desc)` | register or replace an effect by name |
| `spawnBurst(name, worldPos, elevation, spawn={})` | one-shot world burst |
| `spawnScreenBurst(name, screenPos, spawn={})` | one-shot screen-space burst |
| `enableStains(terrain = nullptr)` | make decal effects stick |
| `groundElevationAt(worldPos)` | surface elevation for a terrain-aware spawn |
| `setMaxParticles(n)` | global live-particle ceiling |
| `liveParticleCount()` | current live count (debug / HUD) |
| `hasEffect(name)` / `effect(name)` / `effectNames()` | inspect registered effects |

`Particles<Context>` is a [render module](../rendering/render-modules/index.md);
`ParticleEmitterComponent` is the continuous entity-bound emitter component.
