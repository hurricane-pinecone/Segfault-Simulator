# Decals & splatter

A decal is a **permanent mark stamped onto a surface** where a particle lands —
blood pooling on the ground, streaking down a wall, soaking into a platform.

## Enabling stains

An effect with `leavesDecal = true` marks the surface a particle hits. Turn the
feature on once, per particle module, with `enableStains`:

```cpp
// Isometric: stick to the terrain heightfield (walls, ground, water).
particles.enableStains(&yourTerrain); // an ITerrainSurfaceSource

// Flat: stick to the scene's solids (SolidObject + BoxCollider2D).
particles.enableStains();
```

`enableStains` wires both the **collision** (what particles hit) and the **decal
sink** (where marks are stamped) for your render path — you don't register or
connect anything else. With no stains enabled, decal-leaving particles simply fall
to their spawn plane and leave nothing.

## Landing behaviour

For a particle to land and mark, give it a ground behaviour:

```cpp
d.ground = sfs::GroundBehavior::Die;  // land, stamp, vanish (most splatter)
//          GroundBehavior::Stick     // land and linger for stickDuration
//          GroundBehavior::None      // ignore the ground (no decal)
```

## Tuning the splatter look

The `decal` sub-spec controls the mark. A hit produces a soft **pool** plus a few
crisp directional **streaks**, scaled by how fast the particle was travelling:

```cpp
d.leavesDecal = true;
d.decal.look = sfs::ParticleShape::Radial; // mark texture (or decal.texture for custom art)
d.decal.useParticleColor = true;           // tint from the particle's current colour
d.decal.impactRef = 500.0f;                // speed at which streaks elongate/fan fully
d.decal.pool.size = {6.0f, 16.0f};         // soft pool footprint
d.decal.pool.alpha = 0.85f;
d.decal.streaks.maxCount = 2;              // crisp streaks flung along the impact
d.decal.streaks.width = 1.5f;
```

`impactRef` is in the effect's velocity units (a few tiles/sec on the isometric
path, a few hundred px/sec on the flat path), so set it to roughly the speed at
which you want a hit to read as a full directional splash.

## Surfaces (isometric)

On the heightfield, a hit is classified by what it struck and the mark adapts:

- **Ground** — a pool with directional streaks at the landing tile.
- **Wall** — splatter on the camera-facing face at the impact height, scattered
  along the travel direction (faster hits throw a wider splash) with a drip
  running down from the hit.
- **Water** — a single mark that fades over time.

The flat path stamps onto the collider it hit and clips the mark to that collider
so it never spills off the platform.

## Custom splatter topology

To replace the default pool-and-streaks pattern entirely, implement
`ISplatterShaper` and point the spec at it — it produces the set of marks for each
hit:

```cpp
d.decal.shaper = &myShaper; // owned by you
```

The default (`defaultSplatterShaper()`) is used when `shaper` is null.

## Keeping decals cheap

Settled marks live in persistent GPU buffers, so a stained level costs nothing
extra to re-draw as the camera moves. The surface limits how many marks a small
area accepts before it saturates, so a hammered spot stays put instead of churning
the buffer.
