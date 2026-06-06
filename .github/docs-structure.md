# Documentation structure

How pages under `docs/` are laid out and what a docs change may touch. The
docs-sync and docs-iterate workflows both tell Claude to read this, so editing
this file changes the structure rules for every future run. Pair it with
`.github/docs-style.md`, which covers prose style.

## Layout

- Docs mirror the source layout: `docs/core/**` mirrors engine-core and
  `docs/runtime/**` mirrors the runtime. The mirroring alone decides where a page
  goes.
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

## Scope

- Edit only files under `docs/`, and `mkdocs.yml` when a page was added or
  removed. Nothing else.
