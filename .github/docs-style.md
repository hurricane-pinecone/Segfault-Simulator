# Documentation writing style

House style for the prose in everything under `docs/`. The docs-sync and
docs-iterate workflows both tell Claude to read this first, so editing this file
changes the style of every future run. This is the place to record style
feedback once instead of repeating it on each PR.

## Voice

- Write like clear product documentation aimed at a game developer using the
  engine. Plain, direct, present tense.
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

- Keep examples game-agnostic. Never reference the sample projects (`sampleGame`,
  `samplePlatformer`, `SampleGame`, `GameScene`, `getSystem<TerrainGeneratorSystem>`,
  and so on). Use neutral placeholders.
