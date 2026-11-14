# Code checks: sfs-lsp

`sfs-lsp` is the engine's language server. Your editor runs it next to clangd,
and merges what both say:

- **Completion and hover for `[[sfs::...]]` attributes.** After `[[sfs::` in a
  class body it lists the field attributes that fit the field's type and
  access (`color` only on `glm::vec3` and `glm::vec4`, `show` only on a private
  field), with argument snippets; in `[[sfs::showIf(` it lists the class's
  `bool` fields. Hovering an attribute shows its docs.
- **sfs-codegen's diagnostics as you type.** For a header sfs-codegen reads (a
  project's `scripts/` headers, and any header with an `[[sfs::...]]`
  attribute), you see the build's warnings and errors before you build, with the
  same text: they come from the same code.
- **Lints of the SFS API and architecture rules**, such as a query term that can
  never match, a system reading another system, a component given its own
  entity, or an old form of the entity API. Several come with a fix, and
  `sfs-lsp --fix` applies fixes across a tree.
- **Ignoring a warning.** Every warning can be hidden where it's reported with
  an `sfs-ignore` comment, and the code actions insert one for you.

clangd still does everything C++: types, go to definition, rename, compile
errors. sfs-lsp only adds what clangd can't know.

## Rules

Every diagnostic belongs to a rule. The rule's id is the diagnostic's code in
your editor, is printed at the end of sfs-codegen's build lines
(`... [no-script-type]`), and is what `sfs-ignore` names. Errors mean
sfs-codegen can't generate correct code, so they fail the build and can't be
ignored; everything else is a warning.

<!-- sfs-rules:begin -->
| Rule | Tool | Severity | What it says |
| --- | --- | --- | --- |
| `no-script-type` | sfs-codegen | warning | A header codegen reads should declare a class or struct named after the file. Name the script type after the file, or mark a helper header with `sfs-ignore-file(no-script-type)`. |
| `duplicate-type` | sfs-codegen | error | Two types should not have one registered name. Give each type a name of its own. |
| `script-base` | sfs-codegen | error | A script type has at most one base, and it must be one codegen knows. Derive from one engine base or one reflected type. |
| `component-base` | sfs-codegen | error | An `[[sfs::component]]` type should not have a base. Give it the fields itself, or put shared data in a component of its own. |
| `component-not-registered` | sfs-codegen | warning | An unnamed, nested or template type can't be registered as a component. Declare the component as a plain named type at namespace scope. |
| `field-type` | sfs-codegen | warning | The editor can't edit pointer, reference, array or unknown-type fields, so they're skipped. Store a value the editor can edit instead. |
| `entity-field` | sfs-codegen | warning | Entity reference fields belong on components, so they're skipped on other types. Move the field to a component. |
| `unknown-sfs-attribute` | sfs-codegen | warning | An `[[sfs::...]]` attribute should be one the attribute table knows. Fix the name, or remove the attribute. |
| `attribute-target` | sfs-codegen | warning | A field attribute should not be on a type, nor a type attribute on a field. Move the attribute to where it belongs. |
| `attribute-arguments` | sfs-codegen | warning | An attribute's arguments should have the shape it takes. Write them as the attribute table shows. |
| `attribute-field-type` | sfs-codegen | warning | An attribute should only be on the field types it applies to. Remove it, or change the field's type. |
| `show-if-target` | sfs-codegen | warning | `[[sfs::showIf]]` should name a reflected `bool` field of its type. Name one of the type's bool fields. |
| `single-any` | sfs-lsp | warning | An `sfs::Any` group of one component is just that component. Write the component instead. |
| `any-always-met` | sfs-lsp | warning | An `sfs::Any` group that shares a component with the required ones is always met. Remove the group. |
| `duplicate-term` | sfs-lsp | warning | A term list should not name a component twice in one role. Remove the repeat. |
| `excluded-any-member` | sfs-lsp | warning | A member of an `sfs::Any` group should not also be excluded, since it then never matches. Remove it from the group. |
| `matches-nothing` | sfs-lsp | warning | A term list should not require and exclude one component, or exclude every member of its `sfs::Any` group, since it then matches nothing. Keep each component in one role. |
| `second-any` | sfs-lsp | warning | A system takes one `sfs::Any` group, and a second one is ignored at run time. List every alternative in one group. |
| `late-register` | sfs-lsp | warning | `registerComponent()` should not run once the system is in a registry, where it's ignored. Register components in `create()`. |
| `hook-mutates` | sfs-lsp | warning | `onEntityAdded()`, `onEntityRemoved()` and `observe` callbacks should change only their own entity's components. Change other entities, and create or destroy entities and systems, in `update()`. |
| `system-reads-system` | sfs-lsp | warning | Systems should not read other systems. Put what one system needs on a component, which the other writes and it reads, or have it write a request field there that the other fills in. |
| `per-entity-map` | sfs-lsp | warning | A system should not keep per-entity data in a map or set keyed by entity. Put the data on a component, in a hidden field. |
| `own-entity` | sfs-lsp | warning | A component should not store the entity it's on. Drop the field, and use the entity the system already has. |
| `unknown-attribute` | sfs-lsp | warning | An attribute should be standard C++, an `sfs::` one, or in a vendor's namespace. Fix the name or the namespace. |
| `has-then-get` | sfs-lsp | warning | A component should not be looked up twice, with `hasComponent<T>()` and then `getComponent<T>()`. Use `getComponent<T>()` alone, which gives null when T is missing. |
| `is-alive` | sfs-lsp | warning | `Entity::isAlive()` is gone, because a handle is now valid only while its entity lives. Use `isValid()`, or test the handle with `if (entity)`. |
| `row-lookup` | sfs-lsp | warning | A query loop should not look up a component its row already has. Unpack the row with `for (auto [e, ...] : ...)`. |
| `setup-in-constructor` | sfs-lsp | warning | A system should not set itself up in its constructor. Register components and observers, and set the time step, in `create()`. |
| `component-pointer` | sfs-lsp | warning | `getComponent<T>()` gives a pointer, which is null when T is missing, so it should not be used as a reference. Use `->`, check it for null, and pass what it points to where `T&` is wanted. |
| `filter-in-loop` | sfs-lsp | warning | A loop should not go through a query, or every entity, only to skip the entities that lack a component. Go through a query that lists the component, so the loop only visits those that have it. |
| `unused-row-entity` | sfs-lsp | warning | A query loop should not unpack an entity it never uses. Loop over the components alone, with `T& t` for one or `.components()` for several. |
| `row-component-copy` | sfs-lsp | warning | A query loop should not copy a row's component only to read it. Read it in place with `const T&`, or, for a copy it changes, keep it as scratch or use `T&`. |
| `bad-ignore` | sfs-codegen, sfs-lsp | warning | An `sfs-ignore` comment should name known warning rules in the right form. Fix the comment. |
<!-- sfs-rules:end -->

The sfs-codegen rules are described with [reflection](../../core/reflection/index.md#attributes).
The lints follow.

### Query terms

These read the terms of one `query<...>()` or `observe<...>()` call, or a
system's `registerComponent<T>()` calls in its constructor and `create()`
(together they are its one term list). They follow the rules of
[queries](../../core/ecs/index.md#any-and-excluding), so each flags terms that
don't change what's listed, or a list that's always empty. A query alias
(`using Served = sfs::Query<...>;`, in the file or a project header) counts as
the terms it stands for; what's wrong inside it is reported where the list
names it, without a fix, since its terms are written elsewhere.

- `single-any`: `query<A, sfs::Any<B>>()` is `query<A, B>()`. The fix writes `B`.
- `any-always-met`: in `query<A, sfs::Any<A, B>>()` the group is always met.
  The fix removes it.
- `duplicate-term`: `query<A, B, A>()`. The fix removes the repeat.
- `excluded-any-member`: in `query<sfs::Any<A, B>, sfs::Excluding<A>>()`, `A`
  never counts. The fix removes it from the group.
- `matches-nothing`: `query<A, sfs::Excluding<A>>()` is always empty. There's
  no fix: which term was meant is up to you.

Faded text marks the terms the first four rules call redundant. `sfs::Any` and
`sfs::Excluding` count when written with `sfs::`, after `using namespace sfs;`
or `using sfs::Any;`, or inside `namespace sfs`; a game's own `Any` is never
taken for SFS's.

### Systems

- `second-any`: a system's second `registerComponent<sfs::Any<...>>()` is
  ignored at run time (it logs an error). List every alternative in one group.
- `late-register`: `registerComponent()` in `update()`, `lateUpdate()`,
  `render()`, `onEntityAdded()`, `onEntityRemoved()` or an `observe` callback
  does nothing: the system's entities are already set. Register in
  `create()`.
- `hook-mutates`: `onEntityAdded()`, `onEntityRemoved()` and `observe`
  callbacks run while the registry's lists are being updated. They may add
  components to their own entity or remove them (as
  [the ECS docs](../../core/ecs/index.md) describe, and `TransformSystem`
  does), but changes to other entities, and creating or destroying entities and
  systems, belong in `update()`.
- `system-reads-system`: `getSystem<T>()`, `tryGetSystem<T>()` or
  `hasSystem<T>()` in a system's method, where `T` is one of the project's
  systems. Systems hand data on through components. The engine's own systems
  (`RenderSystem::debugLines()`, raycasts) are its service API and aren't
  flagged, and game objects and UI may read systems.
- `per-entity-map`: a system member that's a map or set keyed by `sfs::Entity`
  or its id. Per-entity data goes on a component, in a hidden field.
- `own-entity`: `state.entity = entity`, where `state` is a component of
  `entity`. A component doesn't store the entity it's on: systems are given it.

### Entity API

These flag the entity API's old forms and the lookups it makes unnecessary:
`getComponent<T>()` gives a pointer (null when T is missing), a handle is valid
only while its entity lives, a query's rows unpack its components (so a loop
lists the components it needs rather than skipping entities without them), and
a system sets itself up in `create()` (see [the ECS docs](../../core/ecs/index.md)). Each
fix is exact on the code it rewrites; where one can't be written safely, the
warning stands without it.

| Rule | Reported for | Fix |
| --- | --- | --- |
| `has-then-get` | `E.hasComponent<T>()` (or `R->hasComponent<T>(E)`) as an `if` or `while` condition, a `&&` operand or a ternary's condition, then `E.getComponent<T>()` in the code that runs only when it's true; or `if (!E.hasComponent<T>()) return;` (or `continue`, `break`) then a lookup later in the block. `E` is a name or a chain of fields, not assigned in between. Not where a component being removed can still be looked up but `hasComponent` says no: inside `onEntityRemoved()`, an `observe` removed callback, or what runs as an excluded component goes (an `observe` added callback of terms with an `Excluding<...>`, or `onEntityAdded()` of a system registering one) | `has ? &get : nullptr` becomes the lookup alone; `if (E.has<T>()) S` becomes `if (T* t = E.getComponent<T>()) S`; `if (E.has<T>() && REST)` (the test first, or after `E.isValid()`) becomes `if (T* t = E.getComponent<T>(); t && REST)`; the early-out becomes `T* t = E.getComponent<T>(); if (!t) return;` (dropping `!E.isValid()`), only when the test comes first or after `!E.isValid()`, so the lookup never runs ahead of a term that cut the condition short. The lookups read `t` (`t->`, `t`, `*t`); a guarded `T& x = E.getComponent<T>()` lends its name. The name is T's in lower camel case without `Component` (`sfs::TransformComponent` gives `transform`); no fix when it's taken |
| `is-alive` | `X.isAlive()` where `X` is declared `sfs::Entity` in the file (a parameter, local or field of a class in the file) or paired with `X.isValid()`; `R->isAlive(E)`. A game type's own `isAlive()` isn't touched | `X.isValid() && X.isAlive()` and `!X.isValid() \|\| !X.isAlive()` lose the second test; `X.isAlive()` becomes `X.isValid()`; `R->isAlive(E)` becomes `R->isValid(E)`. `X.isValid() && !X.isAlive()` (a handle that was an entity, now dead) becomes `X != sfs::Entity{} && !X.isValid()`; under an earlier validity test (an `if`, or an early-out) it's reported for a hand fix, since the renamed test would never hold |
| `row-lookup` | in `for (DECL v : RANGE)` over `query<Terms>()`, `getEntities<Terms>()`, a system's `getEntities()` (its `registerComponent` terms read from the file) or a typed hierarchy walk (`hierarchy::childrenOf<Terms>(e)`, `hierarchy::descendants<Terms>(e)`, whose rows are a query's), a lookup on `v` of a component its row has (aliases expanded). A typed system list's rows also have every term the system registers. A loop over a list variable, or over a helper's list (a function of the file without parameters returning `const std::vector<sfs::Entity>&` whose body is only `return` of such a call: a member of the loop's class, or a free function the class doesn't hide), is reported without a fix. The variable must be the list itself: one declared in the same function as a const reference or a view (`auto`, `sfs::QueryView<...>`), not assigned, address-taken or passed on before the loop; a copy isn't a row loop | the header becomes `for (auto [v, a, b] : RANGE)`, binding the row in term order, or, when the body reads `v` only in the lookups that go, the components alone as `unused-row-entity` writes them (`for (A& a : RANGE)`, `for (auto [a, b] : RANGE.components())`) (`getEntities()` asks for the terms used: `getEntities<A, B>()`; a typed system list gains the registered terms used: `getEntities<A>()` becomes `getEntities<A, B>()`); a C++20 init-statement stays; a `T& name = v.getComponent<T>()` lends its name and goes; other lookups read the binding (an `Any` member's is a pointer). Not when `v` is assigned or its address taken, a lookup is in a lambda, or a name is taken; nor for a by-value `v` passed to a call or given a component (a row's entity is a const reference) |
| `setup-in-constructor` | `registerComponent<...>()`, `observe<...>(...)` or `setTimeStep(...)`, unqualified or through `this`, in a system's constructor | the calls (with the comments above and beside them) move, in order, to the start of `create()`; with none, one is made: `void create() override;` in the class's `protected:` section and its definition where the constructor is. A constructor left empty, with no parameters, initialisers or other constructor, goes with its declaration (and the comments above it). It edits the header and the `.cpp` together. No fix for a call in a nested block, lambda or condition, a call naming a constructor parameter or a local declared before it, a class template, or a `create()` in a third file |
| `component-pointer` | the reference forms: `.member` after the lookup, `&` before it, a reference bound to it, a `T x =` copy of it, the whole component assigned (`E.getComponent<T>() = v`); and a lookup passed to a call that sfs-lsp can see takes `T&` at that place (found by name and argument count in the file and its `"..."` includes), which also catches an overload taking a pointer beside the `T&` one that a pointer would quietly pick. A callee taking `T*`, a generic one, or one it can't find gets no warning | `.f` becomes `->f`; the `&` goes; `T& x = ...` becomes `T* x = ...` with `x`'s uses rewritten (`x.` to `x->`, `&x` to `x`, others `*x`), not when a lambda copies `x`; `T x = ...` becomes `T x = *...`; `= v` becomes `*E.getComponent<T>() = v`. A lookup passed to a call has no fix: check the pointer and pass what it points to. Returns are left to the compiler |
| `filter-in-loop` | a loop over what `row-lookup` reads as a row, or over every entity (`entities()` of a registry or scene: a name ending in `registry` or `scene`, or `scene()`), that skips the entities without a component `T` its rows don't have (a typed system list's include the system's registration; one whose registration can't be read isn't reported). It's reported on the loop's `for`, so an `sfs-ignore` beside or above the loop hides it. Either the whole body is one `if`, with no `else`, that runs only when the loop's own entity has it: `if (T* t = v.getComponent<T>())`, `if (T* t = v.getComponent<T>(); t && REST)`, or `v.getComponent<T>()` or `v.hasComponent<T>()` (or `!= nullptr`) as `&&` operands; or the body is a block that opens with lookups of `v` (`const T* t = v.getComponent<T>();`) and then `if (...) continue;` with `!t`, `t == nullptr`, or a negated lookup or `hasComponent` among its `\|\|` operands. Every such component is named. The operands evaluated before the last such test must be free of side effects (no assignment, `++`, `--`, `new` or call but an entity's lookups and tests), since they run for the entities a query naming it leaves out. `v` may be a structured binding's first name. Not for a list that isn't a query (a parameter, a braced list, a query view's own `entities()`, another type's), a body doing anything else before the test, a lookup on another entity, an `sfs::expect`ed lookup, a test that skips the entities that have a component, nor when the body adds or removes one of them (written there: a call that does it out of sight isn't seen), nor, for a lookup (not `hasComponent`), where a component being removed can still be found, as for `has-then-get`. A list written in the loop is told to take `T`; a shared one (a helper's or a variable's) or a system's own, to go through a query that also lists it. Over every entity the message says to check the loop doesn't need entities not yet added to queries | only for one component, tested alone by the one-`if` form, over a `query<Terms>()` or typed hierarchy walk written in the loop: `T` joins the terms (`query<Terms, T>()`, or `query<T>()` for an empty list), the `if` goes and its body becomes the loop's. A pointer `t` the condition declared becomes the row's binding (`for (auto [v, a, t] : ...)`, or `, t` added to a structured binding; without `v` when the body left doesn't read it, as `unused-row-entity` writes it): `t->` becomes `t.`, `*t` becomes `t`, other uses `&t`. Not when `t` is assigned, its address taken, used in a lambda or redeclared, a `const T*` is passed on, a comment would go, the body has a raw string or a continued line (re-indenting would change them), or a name is taken. A C++20 init-statement stays. No fix over a helper's or a variable's list, a system's own list (its terms can only narrow what it registered), or every entity |
| `unused-row-entity` | a loop over what `row-lookup` reads as a typed row (not a system's own `getEntities()`, whose rows bind nothing) that unpacks it as `[v, components...]` (any `auto` qualifier, the row's number of names) and never reads `v` in its body: no token spelled `v` anywhere in it, lambdas, macro arguments, `sizeof` and `decltype` included (a member named alike, `x.v`, doesn't count). `_` is a name like any other. Reported on `v`, which fades; the message shows the loop's own form (`T& t`, or `.components()`). Not seen: a parenthesised range, or a `.components()` one | one required component: `for (T& t : RANGE)` (`const T&` for a `const T` term), which the row converts to; several, or an `Any` member: `v` leaves the binding and `.components()` follows the range, `for (auto [a, b] : RANGE.components())`, when the range is the list's call or a variable holding its view (`auto`, `sfs::QueryView<...>`); no fix over a helper's list or a vector |
| `row-component-copy` | a loop over a query's view or a typed walk (written in the loop, or a variable holding the view; not a `const std::vector<sfs::Entity>&`) of exactly one required component (an `Excluding` aside; no `Any` member) whose loop variable copies it, `T t` or `const T t` (the row converts to `T`). Not for `auto` (that's the row), a reference, the entity or another type. A copy the body may write (an assignment, `++`/`--`, a member call, `std::move`, a call argument, `>>`, a branch of `?:`, `&`, a reference bound to it) may be scratch on purpose: it's reported without a fix, saying to keep it as scratch or use `T&`, and not at all for a `const T` term, whose rows can't be written. Reported on `t` | for a copy that was const or that the body only reads (its uses go through members, parentheses or an index into an operand, a condition, a return or a copy's initialiser): `const T& t`. It then sees later changes to the component made another way in the body, where the copy kept the old value |

`has-then-get`, `row-lookup` and `unused-row-entity` fade the code they call
redundant; `is-alive` and `component-pointer` strike out the old form.

### Attributes outside codegen

- `unknown-attribute`: an unscoped attribute that isn't standard C++, with a
  did-you-mean fix (`[[nodiscrad]]`), and an sfs attribute written without its
  namespace or under a misspelt one (`[[hide]]`, `[[sf::hide]]`). Vendor
  attributes (`gnu::`, `clang::`) aren't checked.

## sfs-ignore

A comment hides the warnings of the rules it names:

```cpp
#pragma once
// sfs-ignore-file(no-script-type)

auto& fields = registry->getSystem<FieldSystem>(); // sfs-ignore(system-reads-system): until the field rework

// sfs-ignore-next-line(own-entity)
state.entity = entity;
```

| Form | Hides |
| --- | --- |
| `// sfs-ignore(rule, ...)` | the rules' warnings on the comment's own line (a trailing comment) |
| `// sfs-ignore-next-line(rule, ...)` | the rules' warnings on the line after the comment |
| `// sfs-ignore-file(rule, ...)` | the rules' warnings anywhere in the file; by convention below `#pragma once` |

- The directive is the first thing in the comment (`//` or `/* */`). An
  optional `: reason` after the list is for readers; the tools ignore it.
- A warning belongs to the line its underline starts on.
- Every directive names at least one rule: there is no blanket form.
- Errors can't be ignored, and neither can `bad-ignore`.
- A malformed comment is a `bad-ignore` warning: an unknown rule (with a fix
  when it's a typo), a misspelt directive (`sfs-ignore-nextline(...)`, with a
  fix), an empty list, an error rule, an unclosed list, or `sfs-ignore(...)`
  alone on its line (it covers only its own line: the fix makes it
  `sfs-ignore-next-line`).
- sfs-codegen honours the same comments in the headers it reads, so the build
  stays quiet too.

In your editor, the code actions on a warning offer **Ignore <rule> on this
line**, which adds an `sfs-ignore-next-line` comment above the line (or adds the
rule to one that's there), and **Ignore <rule> in this file**, which adds an
`sfs-ignore-file` comment below `#pragma once` (or adds the rule to it).
Inside the comment's parentheses, completion lists the rules, those reported
where the comment applies first, and hovering a rule shows what it's for.

## Where it is

`sfs-lsp` is built and installed with the engine, beside `sfs-codegen`:

| Build | sfs-lsp |
| --- | --- |
| Linux | `bin/sfs-lsp` in the build folder, and in the editor's SDK (`<build>/sdk/bin`) and an installed engine's `bin/` |
| Windows (cross-built from WSL) | `bin\sfs-lsp.exe` and the SDK's `bin\sfs-lsp.exe`, one file with no DLLs, for Windows-native IDEs |
| Windows editor's Code panel | Neovim runs in WSL, so it uses the WSL build's Linux `sfs-lsp` |

`sfs-lsp --version` prints the engine version it belongs to. Use the one that
matches your engine: its attribute table is the engine's.

The editor sets `SFS_LSP` to the path of the matching `sfs-lsp` for the Neovim
it starts, whatever config that uses. Outside the editor, set `SFS_LSP` in your
shell, or put the engine's `bin/` on your `PATH`.

The server attaches in an SFS project (the folder with `project.sfsproj`) or,
failing that, the SFS source tree, whose root has a `.sfs-engine` marker file.
The engine's samples have their own `project.sfsproj`, so they are projects of
their own. Outside a project (the SFS source tree included) the
`system-reads-system` lint is off, as it needs the project's list of systems.

## Neovim

The engine's own config (the Code panel's **Engine config**) runs sfs-lsp
already. For your own config, plain Neovim 0.11 or later (and nvim-lspconfig
users, who need nothing more):

```lua
-- sfs-lsp: [[sfs::...]] completion and checks, and SFS API lints, beside clangd.
-- Inside SfsEditor, SFS_LSP names the sfs-lsp matching the engine; elsewhere it's found on PATH.
local sfs_lsp = vim.env.SFS_LSP or "sfs-lsp"
if vim.fn.executable(sfs_lsp) == 1 then
  vim.lsp.config("sfs_lsp", {
    cmd = { sfs_lsp },
    filetypes = { "c", "cpp" },
    root_dir = function(bufnr, on_dir)
      -- An SFS project, or else the SFS source tree.
      local root = vim.fs.root(bufnr, { "project.sfsproj", ".sfs-engine" })
      if root then
        on_dir(root)
      end
    end,
  })
  vim.lsp.enable("sfs_lsp")
end
```

LazyVim, as `lua/plugins/sfs-lsp.lua`:

```lua
-- sfs-lsp: [[sfs::...]] completion and checks, and SFS API lints, beside clangd.
-- Inside SfsEditor, SFS_LSP names the sfs-lsp matching the engine; elsewhere it's found on PATH.
local sfs_lsp = vim.env.SFS_LSP or "sfs-lsp"

return {
  {
    "neovim/nvim-lspconfig",
    opts = {
      servers = {
        sfs_lsp = {
          enabled = vim.fn.executable(sfs_lsp) == 1,
          cmd = { sfs_lsp },
          filetypes = { "c", "cpp" },
          root_dir = function(bufnr, on_dir)
            -- An SFS project, or else the SFS source tree.
            local root = vim.fs.root(bufnr, { "project.sfsproj", ".sfs-engine" })
            if root then
              on_dir(root)
            end
          end,
        },
      },
    },
  },
}
```

- `vim.fs.root` tries `project.sfsproj` in every folder above the file before
  `.sfs-engine`, so a sample inside the SFS tree is its own project.
- The `root_dir` function attaches only there. `root_markers` alone would start
  the server in any C or C++ file.
- LazyVim gives a server Mason doesn't know straight to `vim.lsp.config`; no
  `mason` key is needed. blink.cmp merges sfs-lsp's completion with clangd's.
- When no `sfs-lsp` is found, `executable()` keeps the config quiet.

## Other editors

Any LSP client can run `sfs-lsp` (no arguments) over stdio for C and C++ files,
with the folder holding `project.sfsproj` (or `.sfs-engine`) as the root.

- **VS Code**, with the clangd extension (on Windows, in a Remote-WSL window,
  where the toolchain and the compile database live): install **Generic LSP
  Client** (`llllvvuu.glspc`), then in the workspace's `settings.json`:

  ```json
  {
    "glspc.serverCommand": "/path/to/sdk/bin/sfs-lsp",
    "glspc.languageId": "cpp"
  }
  ```

  VS Code treats `.h` as `cpp` with the C/C++ extensions; otherwise add
  `"files.associations": {"*.h": "cpp"}`.
- **CLion**: install **LSP4IJ** (Red Hat). Then **Settings > Languages &
  Frameworks > Language Servers > +**: name `sfs-lsp`, the command as the full
  path (`<sdk>\bin\sfs-lsp.exe` on Windows; it reads `\\wsl.localhost\...`
  project paths itself), and under Mappings the C/C++ file type (or the
  patterns `*.h;*.cpp`). CLion's own C++ engine may still mark
  `[[sfs::...]]` attributes as unknown.

### Turning rules off

`initializationOptions: { "disabledRules": ["system-reads-system"] }` turns
rules off for you, in every file; it's the per-user counterpart of
`sfs-ignore`. Errors and `bad-ignore` can't be turned off. In Neovim, add
`init_options = { disabledRules = { "system-reads-system" } }` beside `cmd`.

## Checking from the command line

```bash
sfs-lsp --check [--root DIR] [--disable RULE]... PATH...
```

checks files and folders (the C++ sources and headers under them: `.cpp`,
`.cc`, `.cxx`, `.h`, `.hh`, `.hpp`, `.hxx`) as the server would and prints each
finding as `file:line:col: severity: message [rule]`. The root defaults to the
project (or SFS tree) the first path is in; relative paths are the root's when
`--root` is given (`--root ~/MyGame scripts` checks `~/MyGame/scripts` from
anywhere), else the working folder's. It exits with 1 when it finds an error,
so CI can run it, with 2 for bad arguments or paths with no C++ files under
them, and with 0 otherwise.
`--disable` takes rule ids, like `disabledRules`.

## Fixing from the command line

```bash
sfs-lsp --fix RULE[,RULE...] [--root DIR] PATH...
```

applies the named rules' fixes to the files under the paths: each rule in the
order given, file by file, its fixes that don't overlap taken and the file
checked again, until none is left (at most 8 passes). A fix may edit another
file too (`setup-in-constructor` edits a class's header). It prints each fix as
`file:line:col: fixed [rule] title`, then the named rules' findings it couldn't
fix (and any error) as `--check` does: that listing is what's left to do by
hand. Files are written whole and atomically, and never formatted: wrap a line a
fix made too long yourself. Run it again and nothing changes. For the entity
API's migration the order is
`setup-in-constructor,is-alive,has-then-get,row-lookup,component-pointer`: each
rule leaves the new forms, which the later ones skip.

## The .clangd files

Clang has no switch that silences unknown attributes in one namespace, so the
`.clangd` the editor writes for a project silences them all, and sfs-lsp reports
misspelt standard attributes (and misspelt `sfs` ones) in their place:

```yaml
Diagnostics:
  Suppress: [unknown-attributes]
```

The editor adds this to a project's `.clangd` while it's still as an older
editor wrote it; a `.clangd` you changed is left alone, so add the lines
yourself. The SFS source tree has the same lines in the `.clangd` at its root,
so engine headers read the same way (a test keeps the two alike). For a build outside `find_package(engine)`, which passes these flags
for you, compile with `-Wno-attributes=sfs::` (GCC 12 or later) or
`-Wno-unknown-attributes` (Clang).
