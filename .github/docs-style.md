# Documentation writing style

House style for the prose in everything under `docs/`. The docs-sync and
docs-iterate workflows both tell Claude to read this first, so editing this file
changes the style of every future run. This is the place to record style
feedback once instead of repeating it on each PR.

## Voice

- Write like clear product documentation aimed at a game developer using the
  engine. Plain, direct, present tense.
- Under `docs/editor/`, the reader is someone making a game in the editor, not
  programming the engine: describe what they do and see, in their words, and
  link to the engine docs rather than explaining internals (see "Editor docs" in
  `.github/docs-structure.md`).
- Read every sentence back as normal speech. If it doesn't sound like something
  you'd say out loud, rewrite it.

## Punctuation

- Do not use double hyphens (`--`), em dashes, or en dashes to splice clauses
  together. They make the prose read as choppy fragments. Write complete
  sentences, and use a comma, a period, or parentheses where you would reach for
  a dash.
  - Avoid: `The renderer batches draws -- one per material -- before submitting.`
  - Prefer: `The renderer batches draws by material, then submits them.`
- One idea per sentence. Two short sentences beat one sentence spliced with a
  dash.

## Snippets

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
