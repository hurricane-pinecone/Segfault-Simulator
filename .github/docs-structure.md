# Documentation structure

How the docs are laid out and what a docs change may touch. The docs-sync and
docs-iterate workflows both tell Claude to read this, so editing this file
changes the structure rules for every future run. Pair it with
`.github/docs-style.md`, which covers who each set is for and how to write it.

## Two sets of docs

- `docs/` is the public site for game developers (MkDocs, published to GitHub
  Pages). Everything a game developer needs to make a game with SFS.
- `engine/docs/` is the internal documentation for engine developers. It is
  not published. Everything someone changing SFS needs that a game developer
  doesn't (see "Who reads what" in `.github/docs-style.md`).
- A feature usually has a page in each: the public page says what it's for and
  how to use it, the internal page how it works. When a change affects both,
  update both. When public text would hold internals, move them to the
  internal page instead.
- The public page may link to nothing internal (the site can't reach it). The
  internal page starts with a link to its public page.

## Layout

- Both sets mirror the source layout: `core/**` mirrors engine-core and
  `runtime/**` mirrors the runtime (`docs/core/ecs/index.md` and
  `engine/docs/core/ecs/index.md`). The mirroring alone decides where a page
  goes.
- `docs/editor/**` is the exception: it documents the editor (SfsEditor) for
  the people using it, by what they do in it (panels, menus, workflows), not by
  its source layout. See "Editor docs" below.
- Every public page is its own folder with an `index.md` (for example
  `docs/core/ecs/index.md`). There are no raw `<name>.md` pages. Internal pages
  follow the same rule.
- A section's `index.md` stays brief. Depth lives in its sub-page folders.

## Cross-links

- Contextual "Read more" links between related pages are fine, especially
  between a feature's core and runtime pages.
- Do not add navigation lists that just duplicate the sidebar.

## Navigation

- When, and only when, you add or remove a public page, edit the `nav` in
  `mkdocs.yml` to match. Internal pages have no nav.
- Place the entry in the section that mirrors its source location, and follow the
  existing nav style: a folder's `index.md` is listed first, then its sub-pages.

## Editor docs

- Pages under `docs/editor/` are written by hand, locally, so they can include
  screenshots of the editor. The docs workflows never edit them. When a change
  alters something the editor's users see or do, list it in
  `docs-editor-notes.txt` at the repository root instead (one line each: what
  changed and which editor page it affects). The workflow puts the list in the
  pull request for the owner to follow up.
- When writing them by hand: pages under `docs/editor/` are for game makers
  using the editor, not engine programmers. Explain what a feature does, where
  to find it, and how to use it (the clicks, menus, keys and what you see),
  with the reasons a user cares about.
- Don't deep-dive into engine internals there. Mention the engine side only
  where it matters to using the feature (say, why a script must be named after
  its class), and link to the engine page under `docs/core/` or `docs/runtime/`
  for anyone who wants the detail. Never duplicate engine reference material.
- Write each feature's section so it stands on its own: the editor will show
  these pages as its built-in help (a knowledge base the user opens from the
  feature itself), so a reader may land on any section without the rest.
- Screenshots show only the editor and the engine's own sample projects,
  never a game built on SFS by anyone else.

## Scope

- Edit only files under `docs/` (except `docs/editor/`), under `engine/docs/`,
  and `mkdocs.yml` when a public page was added or removed. Nothing else, apart
  from the `docs-pr-title.txt` and `docs-editor-notes.txt` notes the workflow
  asks for.
