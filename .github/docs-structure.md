# Documentation structure

How pages under `docs/` are laid out and what a docs change may touch. The
docs-sync and docs-iterate workflows both tell Claude to read this, so editing
this file changes the structure rules for every future run. Pair it with
`.github/docs-style.md`, which covers prose style.

## Layout

- Docs mirror the source layout: `docs/core/**` mirrors engine-core and
  `docs/runtime/**` mirrors the runtime. The mirroring alone decides where a page
  goes.
- `docs/editor/**` is the exception: it documents the editor (SfsEditor) for
  the people using it, by what they do in it (panels, menus, workflows), not by
  its source layout. See "Editor docs" below.
- Every page is its own folder with an `index.md` (for example
  `docs/core/ecs/index.md`). There are no raw `<name>.md` pages.
- A section's `index.md` stays brief. Depth lives in its sub-page folders.

## Cross-links

- Contextual "Read more" links between related pages are fine, especially
  between a feature's core and runtime pages.
- Do not add navigation lists that just duplicate the sidebar.

## Navigation

- When, and only when, you add or remove a page, edit the `nav` in `mkdocs.yml`
  to match.
- Place the entry in the section that mirrors its source location, and follow the
  existing nav style: a folder's `index.md` is listed first, then its sub-pages.

## Editor docs

- Pages under `docs/editor/` are for game makers using the editor, not engine
  programmers. Explain what a feature does, where to find it, and how to use it
  (the clicks, menus, keys and what you see), with the reasons a user cares
  about.
- Don't deep-dive into engine internals there. Mention the engine side only
  where it matters to using the feature (say, why a script must be named after
  its class), and link to the engine page under `docs/core/` or `docs/runtime/`
  for anyone who wants the detail. Never duplicate engine reference material.
- Write each feature's section so it stands on its own: the editor will show
  these pages as its built-in help (a knowledge base the user opens from the
  feature itself), so a reader may land on any section without the rest.

## Scope

- Edit only files under `docs/`, and `mkdocs.yml` when a page was added or
  removed. Nothing else.
