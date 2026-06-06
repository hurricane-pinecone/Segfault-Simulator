# Authoring particle effects

Everything for building and firing effects.

## Spawning

### One-shot burst

```cpp
particles.spawnBurst("blood", worldPos, elevation);
```

`elevation` is the ground level the burst sits on (isometric); pass `0` on the
flat path. `spawnBurst` returns `false` if the effect name isn't registered.

To spawn "on the ground" without looking up the height yourself (isometric, once
stains/terrain are wired):

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
  d.burstCount = 24;        // per spawnBurst
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
particle. `EmissionShape::Cone` narrows the launch direction to
`coneAngleDegrees`; otherwise `directionSpread` sets the fan.

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

## Built-in prefabs

The engine ships ready-made effects so you get good results with no art — colour
does the work. They are ordinary `ParticleEffectDesc` builders you register as-is
or copy and tweak.

```cpp
#include "engine/core/particles/particlePrefabs.h"

// Four-layer blood, registered as "<prefix>_mist/_spray/_gobs/_drip":
sfs::registerBloodEffects(particles);          // default red, prefix "blood"
sfs::registerBloodEffects(particles, "ichor",  // a second colour
                          glm::vec3{0.15f, 0.55f, 1.0f},
                          glm::vec3{0.0f, 0.08f, 0.35f});

// A continuous additive ember drift, for an emitter on a light:
particles.registerEffect("embers", sfs::emberEffect());
```

The four blood layers (`bloodMistEffect`, `bloodSprayEffect`, `bloodGobsEffect`,
`bloodDripEffect`) are meant to be fired together at one point for a layered
burst; spray, gobs, and drip leave decals. `recolourBlood(desc, hi, lo)` returns a
colour variant (its decals follow the particle colour).

## Performance & budgets

- **Global cap.** `particles.setMaxParticles(n)` bounds total live particles
  across every effect; over the cap, new emissions are dropped gracefully so
  stacked effects can't run the frame cost away. Each effect also has its own
  `maxParticles` capacity per live occurrence.
- **World vs screen.** `SimulationSpace::World` particles are occluded by terrain
  and depth-sorted; `Screen` particles draw flat on top. Use `Screen` for UI.

## API reference

On the `Particles` module:

| Call | Purpose |
| --- | --- |
| `registerEffect(name, desc)` | Register or replace an effect by name |
| `spawnBurst(name, worldPos, elevation, spawn={})` | One-shot world burst |
| `spawnScreenBurst(name, screenPos, spawn={})` | One-shot screen-space burst |
| `enableStains(terrain = nullptr)` | Make decal effects stick |
| `groundElevationAt(worldPos)` | Surface elevation for a terrain-aware spawn |
| `setMaxParticles(n)` | Global live-particle ceiling |
| `liveParticleCount()` | Current live count (debug / HUD) |
| `hasEffect(name)` / `effect(name)` / `effectNames()` | Inspect registered effects |

Components: `ParticleEmitterComponent` (continuous entity-bound emitter).

Authoring types: `ParticleEffectDesc`, `DecalSpec`, `ParticleSpawnParams`,
`ParticleShape`, `EmissionShape`, `GroundBehavior`, `SimulationSpace`, `BlendMode`,
`Curve`, `Gradient`.
