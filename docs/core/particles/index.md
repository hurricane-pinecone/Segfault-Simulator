# Particle engine

The particle engine simulates short-lived sprites — sparks, blood, smoke, embers,
magic — from a plain-data description you author once. It's **engine-core**:
dependency-free, no rendering. *Using* it in a rendered game (registering the
module, spawning, making decals stick) is a runtime concern — see
[Particles in your game](../../runtime/particles/index.md).

- **Effect** — the authored description of a kind of particle (a
  `ParticleEffectDesc`), registered under a name.
- **Particle** — one live sprite, simulated until it dies.
- **Decal** — a permanent mark an effect can leave where a particle lands; see
  [Decals & splatter](./decals/index.md).

## The effect description

A `ParticleEffectDesc` is plain data — build one in a small function and tweak the
fields. Defaults are sane; set only what you need.

```cpp
sfs::ParticleEffectDesc makeSparkEffect()
{
  sfs::ParticleEffectDesc d;

  // Where particles spawn, relative to the emitter.
  d.shape = sfs::EmissionShape::Circle; // Point / Circle / Ring / Box / Cone
  d.shapeRadius = 0.1f;

  // How many, how often.
  d.burstCount = 24;        // per spawn
  // d.emitRate = 30.0f;    // OR continuous particles/sec for entity emitters
  // d.burstInterval = 0.5f;// OR an emitter that re-bursts every N seconds

  // Per-particle starting state (each sampled uniformly in [min,max]).
  d.lifetime = {0.3f, 0.8f};
  d.speed = {2.0f, 6.0f};             // planar launch speed
  d.launchHeightSpeed = {1.0f, 3.0f}; // upward launch speed (iso height axis)
  d.startHeight = {0.1f, 0.4f};
  d.size = {0.05f, 0.12f};
  d.directionSpread = 1.4f;           // launch angle spread (radians); default = full circle

  // Forces.
  d.gravity = {0.0f, 0.0f};           // planar acceleration
  d.gravityZ = -9.0f;                 // vertical; negative falls
  d.drag = 2.0f;                      // velocity damping per second

  // Appearance over normalized life (0 = born, 1 = death).
  d.colorOverLife = sfs::Gradient::twoStop({1.0f, 0.9f, 0.4f}, {1.0f, 0.2f, 0.0f});
  d.alphaOverLife = sfs::Curve{}.add(0.0f, 0.0f).add(0.1f, 1.0f).add(1.0f, 0.0f);
  d.sizeOverLife  = sfs::Curve::linear(1.0f, 0.2f);

  // Look + blend.
  d.look = sfs::ParticleShape::Radial; // built-in shape (see below)
  d.blend = sfs::BlendMode::Additive;  // Additive for light, Alpha for matter

  d.maxParticles = 256;                // capacity per live occurrence
  return d;
}
```

Field groups, in brief:

| Group | Fields |
| --- | --- |
| Emission | `shape`, `shapeRadius`, `coneAngleDegrees`, `boxExtents` |
| Rate | `emitRate` (continuous), `burstCount`, `burstInterval` |
| Per-particle | `lifetime`, `speed`, `launchHeightSpeed`, `startHeight`, `size`, `rotation`, `angularVelocity`, `directionSpread` |
| Forces | `gravity`, `gravityZ`, `drag` |
| Over life | `sizeOverLife`, `colorOverLife`, `alphaOverLife` |
| Look | `look`, `texture`, `frameCols`/`frameRows`/`frameFps`/`frameOverLife` (sprite-sheet animation) |
| Render / sim | `blend`, `space`, `ground`, `stickDuration` |
| Decals | `leavesDecal`, `decal` |
| Budget | `maxParticles` |

`FloatRange` fields take `{min, max}` (or a single value) and are sampled per
particle. `EmissionShape::Cone` narrows the launch direction to `coneAngleDegrees`;
otherwise `directionSpread` sets the fan.

## Shapes and textures

Particles and decal marks pick a **built-in shape** rather than naming a texture,
so you never reference the engine's internal art:

```cpp
d.look = sfs::ParticleShape::Radial; // soft round dot (the default)
d.look = sfs::ParticleShape::Pixel;  // crisp filled square
```

For custom art, set an explicit texture id instead — it overrides the shape:

```cpp
d.texture = "myFlame"; // a texture you registered on the asset store
```

`frameCols` / `frameRows` / `frameFps` animate a sprite-sheet texture over time
(`frameOverLife = true` maps the whole sheet across a particle's lifetime).

## Curves and gradients

`Curve` (scalar) and `Gradient` (RGB) key values over normalized life `t` in
`[0,1]`:

```cpp
sfs::Curve::constant(1.0f);
sfs::Curve::linear(1.0f, 0.0f);
sfs::Curve{}.add(0.0f, 0.0f).add(0.2f, 1.0f).add(1.0f, 0.0f); // fade in then out

sfs::Gradient::twoStop({1,0,0}, {0.2f,0,0});
sfs::Gradient{}.add(0.0f, {1,1,1}).add(1.0f, {1,0.5f,0});
```

Up to 8 stops; sampling clamps outside the first/last stop.

The engine also ships **ready-made effect prefabs** (blood layers, embers) you can
register as-is or copy and tweak — those are a runtime helper, covered in
[Particles in your game](../../runtime/particles/index.md#built-in-prefabs).

Authoring types: `ParticleEffectDesc`, `DecalSpec`, `ParticleSpawnParams`,
`ParticleShape`, `EmissionShape`, `GroundBehavior`, `SimulationSpace`, `BlendMode`,
`Curve`, `Gradient`.
