# Documentation writing style

House style for everything under `docs/` (the public site) and `engine/docs/`
(the internal docs). The docs-sync and docs-iterate workflows both tell Claude
to read this first, so editing this file changes the style of every future
run. This is the place to record style feedback once instead of repeating it
on each PR.

## Who reads what

- `docs/` is for **game developers making a game with SFS**. They want to know
  what a feature is for in their game, how to use it, and what to watch out
  for. They don't care how the engine implements it.
- `engine/docs/` is for **engine developers working on SFS itself**: memory
  layouts, data structures, algorithms, invariants, benchmark targets and
  gates, implementation trade-offs, and the full edge-case rules of tools such
  as the linter.
- The test for a fact: would a game developer change their code because of
  it? Then it goes in `docs/`. Would only someone changing the engine need it?
  Then it goes in `engine/docs/`.
- Performance belongs in `docs/` whenever it changes how a game should be
  written, phrased as advice for the game: "Don't call `findEntity()` every
  frame: look the entity up once in `create()` and keep it", or "A query is
  cheap to go through every frame; building one is not". The numbers behind
  the advice (bench results, gates, cache behaviour) go in `engine/docs/`.

## Page shape (docs/)

- Open with what the feature is for in a game, in a sentence or two, before
  any API. "Events let one system tell others that something happened, like an
  enemy dying or a door opening, without either knowing about the other."
- Then how to use it: the common case first, as a short, complete example from
  a game. Then the variations and options a game developer will reach for.
- Then the things to watch out for: mistakes that are easy to make, and the
  performance advice above.
- Reference detail (every parameter, every option) comes last, in tables
  where it's a list.

## Voice

- Write like clear product documentation aimed at a game developer using the
  engine. Plain, direct, present tense. Address the reader as "you".
- Under `docs/editor/`, the reader is someone making a game in the editor, not
  programming the engine: describe what they do and see, in their words, and
  link to the engine docs rather than explaining internals (see "Editor docs" in
  `.github/docs-structure.md`).
- Read every sentence back as normal speech. If it doesn't sound like something
  you'd say out loud, rewrite it.
- `engine/docs/` may be denser and more technical, but the same plain-sentence
  rules apply.

## Punctuation

- Do not use double hyphens (`--`), em dashes, or en dashes to splice clauses
  together. They make the prose read as choppy fragments. Write complete
  sentences, and use a comma, a period, or parentheses where you would reach for
  a dash.
  - Avoid: `The renderer batches draws -- one per material -- before submitting.`
  - Prefer: `The renderer batches draws by material, then submits them.`
- One idea per sentence. Two short sentences beat one sentence spliced with a
  dash.

## Examples

- Every example in `docs/` shows the feature doing a job in a game, not the API
  in the abstract. Not `registry->query<A, B>()`, but "damage every enemy in the
  blast radius":

  ```cpp
  for (auto [enemy, transform, health] :
       registry->query<Enemy, sfs::TransformComponent, Health>())
    if (glm::distance(transform.position, blast.centre) < blast.radius)
      health.current -= blast.damage;
  ```

- Keep examples short and complete enough to read on their own: the types they
  use are declared nearby or obvious from their names.
- Keep every example generic: snippets, type and field names, log lines, and
  the examples given in the prose. The engine serves any kind of game, so its
  docs never use one genre's vocabulary or a particular game's types.
  - Never use farming or any other game's terms: no farms, farm hands,
    tractors, crops, fields of crops, harvests or seeds, and no types from a
    game built on the engine.
  - Never reference the sample projects' code (`samples/showcase3d`,
    `samples/platformer2d`, their scripts and scenes).
  - Use neutral names any game could have: `Player`, `Enemy`, `Health`,
    `Door`, `Pickup`, `Projectile`, `Spawner`, `Inventory`, `DamageTaken`,
    `ItemPicked`, `Vehicle`, `Trailer`.
  - Existing pages may still hold game-specific examples. Don't copy them:
    replace any you touch with generic ones.
