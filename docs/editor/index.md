# Editor

SfsEditor is the engine's editor application. It opens a project folder, and
edits scenes in a docking UI. Any panel can be docked, tabbed, or dragged out
into its own OS window.

The editor is built with the engine (`ENGINE_BUILD_EDITOR`, on when the engine
is the top-level project). It uses the engine only through its public `sfs::`
package targets.

```bash
SfsEditor <project-folder>
```

## Projects

A project is a folder with a `project.sfsproj` file and standard content folders:

```
MyGame/
  project.sfsproj   # {"formatVersion": 1, "name": "MyGame", "startScene": "scenes/main.scene"}
  scenes/           # *.scene files
  assets/
  scripts/
```

The editor adds `CMakeLists.txt`, `.gitignore` and `.clangd` on the first
build. Build output (`.sfs/`) and exports (`builds/`) stay out of version
control.

**File → New Project** creates this layout. **File → Open Project** opens an
existing one. A project opens on its `main.scene`, or on its first scene if it
has no `main.scene`.

## Scenes

| Command | Effect |
| --- | --- |
| File → New Scene | A fresh scene (ground, cube, light), not yet saved |
| File → Open Scene | Pick from the project's `scenes/` folder |
| File → Save Scene (Ctrl+S) | Save to the current file; a new scene asks for a name |
| File → Save Scene As | Save under a new name in `scenes/` |

Scene files are plain JSON and diff cleanly. See
[Scene files](../runtime/serialization/index.md).

## Panels

| Panel | Purpose |
| --- | --- |
| Viewport | The scene, with a gizmo on the selection (see below). |
| Hierarchy | The scene's entities. Click to select; use the **+** button or right-click to create entities, and right-click an entity to delete it. The filter box narrows the list by name, and picking in the viewport scrolls the list to the selection. |
| Inspector | The selected entity's name and components, with **Add Component**. |
| Scene Settings | Game systems, sun, sky, ambient light, shadows, camera projection and render stats. |
| Assets | The project's `scenes/`, `scripts/` and `assets/` folders. Double-click a scene to open it, a script or other text file to edit it in the Code panel, or anything else to open it in its default app; right-click `scripts` for **New Script**. |
| Code | Neovim, embedded, for editing scripts (see below). |
| Console | The engine log; errors are highlighted. |

## Viewport

| Input | Action |
| --- | --- |
| Left-click | Select the entity under the cursor (nothing, if empty space) |
| W / E / R | Move / rotate / scale gizmo (also in the viewport toolbar) |
| World / Local (toolbar) | Gizmo axes aligned to the world or to the object |
| Snap (toolbar), or hold Ctrl while dragging | Snap to 0.5 units, 15° or 0.1 scale |
| Right-drag | Orbit |
| Middle-drag, or Shift+right-drag | Pan |
| Wheel | Zoom |
| F | Frame the selection |

The selection is outlined, and lights are drawn as small sun icons, so entities
without a mesh can be seen and clicked.

## Undo and editing

Every scene edit can be undone: inspector fields, gizmo moves, creating,
duplicating and deleting entities, adding and removing components, and game
systems. A whole drag is one step. Undo leaves the camera where it is and
restores the selection.

| Command | Shortcut |
| --- | --- |
| Edit → Undo / Redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| Edit → Duplicate | Ctrl+D |
| Edit → Delete | Del |

History is cleared when another scene is opened. Edits made while playing aren't
recorded, because Stop discards them anyway.

The inspector is generated from [reflection](../core/reflection/index.md). Every
reflected component gets an editor built from its fields and attributes, so a
component you write appears in **Add Component** with no editor code.

## Code panel

The Code panel is a real Neovim, embedded: the editor starts `nvim --embed` and
draws its screen in the panel, so everything Neovim does works there. New
scripts, and scripts double-clicked in **Assets**, open in it. While it has
focus it gets the whole keyboard, editor shortcuts included: Ctrl+S saves the
file, not the scene.

Neovim (0.11 or later) runs where the game module builds. For a Windows editor
cross-built from WSL, that is Neovim in WSL, with your WSL setup.

The toolbar picks the configuration (applied on **Restart**):

| Config | Starts Neovim with |
| --- | --- |
| Auto (default) | Your own config (`~/.config/nvim`) if you have one, otherwise the engine's |
| Engine config | The engine's, kept apart from yours (its own config and data folders) |
| Your config | Your own config |

The engine's config is `editor/nvim/init.lua`, shipped next to the editor
executable. It is deliberately small: no plugin manager or
plugins, a built-in colour scheme, 2-space indents, and C++ support through
Neovim's built-in LSP client when `clangd` is installed. That gives go to
definition (`gd`), hover (`K`), completion, diagnostics, rename (`grn`) and code
actions (`gra`); `Space h` switches between header and source.

The editor sets `g:sfs_editor = 1` before any config runs, so your own config
can check `vim.g.sfs_editor` and leave out what an embedded editor doesn't need
(a dashboard, a file finder):

```lua
if vim.g.sfs_editor then
  -- inside SfsEditor
end
```

Projects carry a `.clangd` that points clangd at the game module's compile
database (each build links it at `.sfs/compile_commands.json`), so clangd finds
the engine headers with any config, in any editor.

The panel draws with a monospace font: a Nerd Font Mono if one is installed
(for the icons many configs use), otherwise Cascadia Mono or Consolas on
Windows and DejaVu Sans Mono on Linux. Set `SFS_EDITOR_CODE_FONT` to a `.ttf`
file to choose another. Ctrl+wheel changes the size; the size and the config
choice are saved with the editor layout.

## Scripts and Play

**Scripts → New ...** creates GameObject scripts, systems and components from
templates. **Play** (Ctrl+P) builds them into the project's game module and runs
the scene, and **Stop** restores it. See [Game scripts](scripting/index.md).

## Exporting a game

**File → Export Game...** builds the scripts and writes a playable copy of the
game into a folder (by default the project's `builds/<platform>/`). Choose the
**start scene** the game opens on; it's saved with the project. The folder can
be zipped and handed to anyone, with no editor or engine install needed:

```
builds/windows/
  MyGame.exe          the engine's player (SfsPlayer), named after the game
  *.dll               engine and runtime libraries
  MyGame.dll          the game module (your scripts)
  scenes/  assets/    the project's content
  game.json           what to load: module, start scene, window size
  log.txt             written when the game runs
```

The exported game plays the start scene exactly as **Play** does: same scene
setup, same scripts and game systems, with no editor UI. It starts from the
camera the scene was saved with. If it can't start (a missing file, a module
that fails to load), it says so in a message box, with details in `log.txt`.

Export replaces the folder's contents, so it only writes into an empty folder
or a previous export (one with a `game.json`). It never overwrites anything
else.

Exports can also be made without opening the editor, e.g. from a script or CI:

```
SfsEditor --export <project-folder> [output-folder]
```

A Windows editor exports Windows games. A Linux export is the game plus the
engine's libraries; system libraries (SDL and the rest) must be installed where
it runs.

## Built-in meshes

New entities use the editor's built-in meshes: `builtin:cube`, `builtin:sphere`,
`builtin:cylinder` and `builtin:plane`. They're named, so scenes that use them
save and load. A mesh field's picker in the inspector lists every named mesh.
