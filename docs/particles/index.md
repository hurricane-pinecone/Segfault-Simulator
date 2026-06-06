# Particles & decals

Particles are short-lived sprites the engine simulates and draws for you — sparks,
blood, smoke, embers, magic. Effects that land on a surface can leave a **decal**:
a permanent stain on the terrain or a platform. Both work on either render path;
only the units differ — world **tiles** (isometric) or **pixels** (flat).

You author an effect once as plain data, register it under a name, then fire it by
name. No per-frame work, no manual draw calls.

## Quick start

```cpp
// 1. Add the particle module to your render system (Context = its render context).
auto& particles = renderSystem.withModule<sfs::Particles<Context>>();

// 2. Register an effect under a name.
particles.registerEffect("spark", makeSparkEffect());

// 3. Optionally make decal-leaving effects stick to the world.
particles.enableStains(&yourTerrain); // iso: an ITerrainSurfaceSource
//   flat: particles.enableStains();  // sticks to the scene's colliders

// 4. Fire it.
particles.spawnBurst("spark", worldPos, elevation);
```

`sfs::Particles<Context>` is a render module; the `Context` (`IsometricRenderContext`
or `FlatRenderContext`) is your render system's context type and selects the
matching behaviour. Everything else is on the module you get back from `withModule`.

## Concepts

- **Effect** — the authored description of a kind of particle (a
  `ParticleEffectDesc`), registered under a name.
- **Burst / emitter** — how an effect fires: a one-shot **burst** at a point, or a
  continuous **emitter** attached to an entity.
- **Particle** — one live sprite, simulated until it dies.
- **Decal** — the permanent mark an effect leaves where a particle lands, when
  stains are enabled.

## The two spawn paths

```cpp
// One-shot at a point:
particles.spawnBurst("blood", worldPos, elevation);

// Or a continuous emitter that follows an entity:
entity.addComponent<sfs::ParticleEmitterComponent>("embers", offset, heightOffset);
```
