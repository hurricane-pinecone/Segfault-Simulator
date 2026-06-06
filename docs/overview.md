# SegFaultSimulator documentation

SegFaultSimulator (**sfs**) is a 2D game engine with an ECS core and two render
paths over one core renderer — a **flat 2D** path for side-on or top-down games,
and an **isometric heightfield** path. A game picks a path by adding the matching
render system; the engine forces neither.

This is the documentation home. Each section has an **overview** for the common
case, with deeper pages where the detail warrants it.

## Sections

| Section | What it covers |
| --- | --- |
| [Architecture](./architecture/overview.md) | How the engine fits together: the ECS core, the render seam, module composition, engine vs game ownership |
| [Particles & decals](./particles/overview.md) | Authoring particle effects, spawning them, and sticking persistent splatter to surfaces |
| [Scripting](./scripting/overview.md) | Giving a game a live Lua modding API, editable at runtime on native and web |

## Conventions

- Snippets are engine-only and game-agnostic — substitute your own types wherever
  a placeholder appears (`yourTerrain`, `YourGame`, an effect builder, …).
- Coordinate units differ by render path: world **tiles** on the isometric path,
  **pixels** on the flat path.
