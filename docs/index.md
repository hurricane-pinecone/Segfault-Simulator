# SegFaultSimulator documentation

SegFaultSimulator (**sfs**) is a 2D game engine with an ECS core and two render
paths over one core renderer — a **flat 2D** path for side-on or top-down games,
and an **isometric heightfield** path. A game picks a path by adding the matching
render system; the engine forces neither.

Use the sidebar to navigate the sections: architecture, particles & decals, and
scripting. Each opens on an overview, with deeper pages for detail.

## Conventions

- Snippets are engine-only and game-agnostic — substitute your own types wherever
  a placeholder appears (`yourTerrain`, `YourGame`, an effect builder, …).
- Coordinate units differ by render path: world **tiles** on the isometric path,
  **pixels** on the flat path.
