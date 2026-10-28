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
| Hierarchy | The scene's entities, as a tree of parents and children (see below). Click to select; use the **+** button or right-click empty space to create entities. The filter box narrows the tree to a flat list of matching names, and picking in the viewport opens the selection's parents and scrolls to it. |
| Inspector | The selected entity's name and components, with **Add Component**. |
| Scene Settings | The scene's systems (yours, and the engine's optional ones such as physics), the game camera, sun, sky, ambient light, shadows, the viewport's projection and render stats. |
| Assets | The project's `scenes/`, `scripts/` and `assets/` folders: a folder tree, and the open folder's contents (see below). Double-click a scene to open it, a script or other text file to edit it in the Code panel, or anything else to open it in its default app; right-click `scripts` for **New Script**. |
| Code | Neovim, embedded, for editing scripts (see below). |
| Console | The engine log; errors are highlighted. |

## Viewport

| Input | Action |
| --- | --- |
| Left-click | Select the entity under the cursor (nothing, if empty space) |
| Gizmos (toolbar) | Show or hide the overlays below and the gizmo; on while editing, off during Play by default |
| W / E / R | Move / rotate / scale gizmo (also in the viewport toolbar) |
| World / Local (toolbar) | Gizmo axes aligned to the world or to the object |
| Snap (toolbar), or hold Ctrl while dragging | Snap to 0.5 units, 15° or 0.1 scale |
| Right-drag | Orbit |
| Middle-drag, or Shift+right-drag | Pan |
| Wheel | Zoom |
| F | Frame the selection |

The selection is outlined, and lights are drawn as small sun icons, so entities
without a mesh can be seen and clicked. These overlays are editor-only: exported
games never draw them, and during Play the viewport hides them so it looks like
the game. Tick **Gizmos** during Play to see them (and move things) anyway; the
editing and Play settings are remembered separately.

## Assets

Everything under `assets/` is managed by the project's
[asset database](../core/assets/index.md): each file has a stable id in a
`.meta` file beside it, and the Assets panel keeps the two together.

| Action | Result |
| --- | --- |
| Click an asset | The Inspector shows its kind, path, size and id |
| Drag an asset or folder onto a folder (in the tree, the contents or the breadcrumbs) | Moves it |
| Right-click → Rename, or F2 | Renames it in place; leaving the extension off keeps the old one |
| Right-click → Delete... | Deletes it (and a folder's contents) after asking |
| Right-click a folder, or empty space → New Folder | Type a name and press Enter |
| Drop files from Explorer onto the editor window | Imports copies into the open folder (or `assets/` itself) |
| Right-click a folder, or empty space → New Material | A material asset to edit in the Inspector |

Models (`.glb`, `.gltf`) appear in a Mesh Renderer's **Mesh** list, under
*Assets*; picking one loads it. The scene refers to it by id, so it stays
attached when the file is moved or renamed.

Selecting a material asset (`.mat`) edits it in the Inspector: base colour,
texture, tiling and offset (how often the texture repeats, and where it
starts), metal, roughness and transparency. Every change is saved and shows in
the scene straight away.

A Mesh Renderer lists its mesh's **Materials**, one slot per section (a model
has one per material it was made with). Each slot shows the mesh's own
material until you pick a material asset for it.

To texture something, drag a texture from the Assets panel onto it in the
viewport (onto the part you want, for a model with several materials), or
onto a slot in the Inspector. That makes a material for the texture in
`materials/` (or reuses the one it made before) and puts it in the slot.
Dragging a material works the same way.

A model's own materials live inside its file. To change one (make the barn's
roof red, say), right-click its slot → **Extract to Edit**: that saves it,
with its textures, as a material asset in `materials/` and uses it there.

Commit the `.meta` files along with the assets. A file moved outside the
editor without its `.meta` gets a new id.

## Parents and children

Any entity can be the child of another, and moves, turns and scales with it
(see [Parents & children](../core/hierarchy/index.md)). In the Hierarchy:

| Action | Result |
| --- | --- |
| Drag an entity onto another | It becomes that entity's child, staying where it is in the world |
| Drag an entity onto the empty space below the list | It becomes a root, staying where it is |
| Right-click → Create Child | A new entity under this one, at its origin |
| Right-click → Unparent | It becomes a root, staying where it is |
| Right-click → Delete, or Del | Deletes it and everything below it |
| Ctrl+D | Duplicates it and everything below it, beside the original |

The Inspector shows a child's transform relative to its parent, and says which
parent. The viewport gizmo works in world space. An empty entity makes a good
folder: its children keep their places when it sits at the origin.

## The game camera

The viewport is the editor's own view: looking around while editing never moves
the game's camera. The game sees through a **camera entity** instead, an entity
with a Camera3D component (see
[Camera entities](../runtime/rendering-3d/index.md#camera-entities)):

- **+ → Camera** adds one, looking the way the viewport is looking.
- **Edit → Align With View** (Ctrl+Shift+F) moves the selection to the
  viewport's view, to re-aim a camera.
- **Edit → Align View With Selected** looks through the selection, to see what
  a camera sees.
- Cameras are drawn in the viewport as outlines of what they see. The game
  camera is drawn brighter.
- Scene Settings names the game camera: the first active one.

Play and exported games see through the game camera, and scripts move it by
moving its entity. A scene without one starts from the viewport's view as last
saved, as before.

## Undo and editing

Every scene edit can be undone: inspector fields, gizmo moves, creating,
duplicating and deleting entities, adding and removing components, and game
systems. A whole drag is one step. Undo leaves the viewport's view where it is and
restores the selection.

| Command | Shortcut |
| --- | --- |
| Edit → Undo / Redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| Edit → Duplicate | Ctrl+D |
| Edit → Delete | Del |
| Edit → Align With View | Ctrl+Shift+F |

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
setup, same scripts and game systems, with no editor UI. It sees through the
scene's [game camera](#the-game-camera). If it can't start (a missing file, a module
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
