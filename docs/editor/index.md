# Editor

SfsEditor is the engine's editor application. It opens a project folder, and
edits scenes in a docking UI. Any panel can be docked, tabbed, or dragged out
into its own OS window. It edits games of every style, flat 2D, isometric 2D
and 3D (see [Styles: 2D and 3D](#styles-2d-and-3d)), and draws them with the
engine's own renderer, so the viewport looks like the game.

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
| Hierarchy | The scene's entities, as a tree of parents and children (see below). Click to select; **Ctrl**+click adds or removes a row, **Shift**+click selects every row from the selected one to it (see [Editing several at once](#editing-several-at-once)); double-click, the row's crosshair button or right-click → Focus frames it in the viewport (as F does there), and a double-click also opens or closes a row with children. To rename, click the selected row's name again, press **F2**, or right-click → **Rename**: a box opens over the row (Enter or clicking away keeps it, Esc cancels). New objects open it straight away, so you can type their name. Use the **+** button or right-click empty space to create entities. The filter box narrows the tree to a flat list of matching names, and picking in the viewport opens the selection's parents and scrolls to it. |
| Inspector | The selected entity's name and components, with **Add Component** (with several selected: what they share, edited on all of them). Each field's label is on the left and its value on the right. Settings that don't apply are hidden, such as a Mesh Renderer's emission light settings when none of its materials can glow. |
| Scene Settings | The scene's systems (yours, and the engine's optional ones such as physics), its **Style** (3D, Flat 2D or Isometric 2D), the game camera, sun, sky, ambient light, shadows, the viewport's projection and render stats. |
| Assets | The project's `scenes/`, `scripts/`, `prefabs/` and `assets/` folders: a folder tree, and the open folder's contents (see below). Double-click a scene to open it, a script or other text file to edit it in the Code panel, or anything else to open it in its default app; right-click `scripts` for **New Script**. |
| Code | Neovim, embedded, for editing scripts (see below). |
| Console | The engine log; errors are highlighted. |

## Styles: 2D and 3D

A scene's style is in Scene Settings → **Style**:

| Style | For | Settings |
| --- | --- | --- |
| 3D | A world in metres, seen through a camera you place | none |
| Flat 2D | Sprites on a plane, seen straight on: platformers, top-down games | **Pixels Per Unit**, **Y Runs Down**, **Sort By Y** |
| Isometric 2D | A tile grid seen at an angle | **Pixels Per Unit**, **Tile Pixels**, **Height Pixels** |

Changing it is one undo step, and it's saved with the scene (see
[Styles](../runtime/rendering/styles/index.md)). Only the settings the chosen
style uses show.

In a 2D scene:

- The viewport looks straight down at the game's plane, orthographic.
  Right-drag or middle-drag pans, and the wheel zooms.
- The gizmo moves things across the plane (and, in a flat scene, turns them
  around it); a moved thing keeps its height. With snapping on, moves snap to
  whole pixels (flat) or half tiles (isometric). An isometric sprite always
  faces the view, so it doesn't turn.
- Clicking a sprite selects it.
- **+ → Sprite** adds an entity with a Sprite component. Pick its **Image**
  (a texture) and **Frame** in the Inspector. A texture's import settings make
  it a sprite sheet (see [Assets](#assets)).

## Viewport

| Input | Action |
| --- | --- |
| Left-click | Select the entity under the cursor (nothing, if empty space) |
| Gizmos (toolbar) | Show or hide the overlays below and the gizmo; on while editing, off during Play by default |
| Animate (toolbar, while editing) | Untick it to hold animated materials and grass's wind still while you edit; Play always animates |
| W / E / R | Move / rotate / scale gizmo (also in the viewport toolbar) |
| World / Local (toolbar) | Gizmo axes aligned to the world or to the object |
| Snap (toolbar), or hold Ctrl while dragging | Snap to 0.5 units, 15° or 0.1 scale |
| Right-drag | Orbit (in a 2D scene: pan) |
| Middle-drag, or Shift+right-drag | Pan |
| Wheel | Zoom |
| F | Frame the selection |
| B, or Grass (toolbar), with grass selected | The grass brush (see [Grass](#grass)) |

A 2D scene's viewport is described in [Styles: 2D and 3D](#styles-2d-and-3d).

The selection is outlined, and lights are drawn as small sun icons, so entities
without a mesh can be seen and clicked. These overlays are editor-only: exported
games never draw them, and during Play the viewport hides them so it looks like
the game. Tick **Gizmos** during Play to see them (and move things) anyway; the
editing and Play settings are remembered separately.

## Assets

Everything under `assets/` and `prefabs/` is managed by the project's
[asset database](../core/assets/index.md): each file has a stable id in a
`.meta` file beside it, and the Assets panel keeps the two together.

| Action | Result |
| --- | --- |
| Click an asset | The Inspector shows its kind, path, size and id |
| Move, rename or delete a `.gltf` model | Its `.bin` (the geometry it keeps beside it, not shown in the panel) goes with it, and the paths inside it to its data and textures are updated |
| Drag a file or folder onto a folder (in the tree, the contents or the breadcrumbs) | Moves it, within its project folder (assets and prefabs together) |
| Right-click → Rename, or F2 | Renames it in place; leaving the extension off keeps the old one |
| Right-click → Delete..., or Del | Deletes it (and a folder's contents) after asking; Undo brings it back |
| Right-click → Copy (Ctrl+C), then a folder → Paste (Ctrl+V) | Copies it there (as "Name Copy" where the name's taken) |
| Right-click → Duplicate, or Ctrl+D | A copy beside it |
| Right-click a folder, or empty space → New Folder | Makes "New Folder" with its name ready to type |
| Ctrl+Z / Ctrl+Y (with the Assets panel focused) | Undoes and redoes the panel's file operations |
| Drop files from Explorer onto the editor window | Imports copies into the open folder (or `assets/` itself) |
| Right-click a folder, or empty space → New Material | A material asset to edit in the Inspector |
| Right-click a folder, or empty space → New Material Graph | A material built from nodes; double-click it to edit them |

This works the same in every folder, scripts and scenes too. The project's own
folders (`scenes/`, `scripts/`, `prefabs/`, `assets/`) can't be renamed, moved
or deleted. What a change takes care of depends on the folder:

- **Assets and prefabs** keep their ids: references to them keep working when
  they're moved or renamed, and a deleted one comes back as itself when undone.
  A copy is a new asset with the same import settings.
- **Scripts** keep building: `#include`s of a moved or renamed script are
  rewritten to find it. Renaming a script renames its class (its `.h` and `.cpp`
  together, in code, not in comments or strings); scenes follow when the
  scripts are built. A copied script becomes a class of its own (`NameCopy`).
- **Scenes**: the open scene, and the project's start scene, follow a move or
  rename.

Deleted files wait in the project's `.sfs/trash` until it's next opened, so a
delete can be undone while it's open.

Tiles show previews of textures, materials, models and prefabs. Previews are
drawn in the background and appear as they're ready.

The **status bar** along the bottom of the editor shows what's happening in
the background, each with a progress bar: loading a project (its files are
scanned in the background), checking for changed files (when you come back to
the editor from another app, or press Refresh), loading a scene (its objects come in over a few
frames), loading models (they appear as they load), building scripts and making
previews. It reads *Ready* when nothing is. The editor's panels appear straight
away and stay responsive while all of this happens; saving, Play or an edit
during a scene's loading waits for the rest of it first.

Models (`.glb`, `.gltf`) appear in a Mesh Renderer's **Mesh** list, under
*Assets*; picking one loads it. The scene refers to it by id, so it stays
attached when the file is moved or renamed.

Selecting a texture shows its import settings in the Inspector: **Filter**
(Smooth, or Pixel Art for crisp texels) and **Sprite Sheet**, with its
**Frame Size**, to use it as a grid of sprite frames, numbered across then
down from the top left. They're saved in its `.meta` and show straight away
(see [Sprites & sprite sheets](../runtime/rendering/sprites/index.md)).

Selecting a material asset (`.mat`) edits it in the Inspector: base colour,
texture, tiling and offset (how often the texture repeats, and where it
starts), metal, roughness, a normal map (bumps the light catches; the bluish
image that comes with many textures) with its strength, and transparency. Every change is saved and shows in
the scene straight away.

For a look those settings can't make (patterns, masks, glows, textures blended
together), build the material from nodes instead: see
[Material graphs](./material-graphs/index.md).

A Mesh Renderer lists its mesh's **Materials**, one slot per section (a model
has one per material it was made with). Each slot shows the mesh's own
material until you pick a material asset for it.

A slot with a material asset in it has two icons beside it, as a script field
does:

- **Edit** (the pencil) opens a node-graph material in the node editor. A
  standard material is shown in the Inspector instead, where its fields are,
  along with **Convert to Node Graph**.
- **Find** (the target) shows the material highlighted in the Assets panel,
  leaving the Inspector on the object.

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

## Prefabs

A prefab is a reusable object (or branch of objects) saved as an asset; the
scene places copies of it, and editing the prefab updates every copy.

| Action | Result |
| --- | --- |
| Drag an object from the Hierarchy into the Assets panel, or right-click it → Create Prefab | Saves it, and everything under it, as a `.prefab`; the object becomes a copy of it |
| Drag a prefab (or a model) into the viewport | Places a copy where the pointer meets a surface or the ground |
| Drag a prefab onto a Hierarchy row, or below the list | Places a copy under that object, or at the top |
| Double-click a prefab, or Edit in the Inspector | Opens the prefab in its own viewport tab, named after it |
| **Apply** on a prefab's tab (or Ctrl+S there) | Writes the prefab; every copy updates, except where a copy changed something itself |

A prefab's tab shows it alone, on a floor grid (metre lines, every tenth
stronger, the X axis red and Y green). The tab you last clicked is the one being
edited: the Hierarchy, Inspector and undo follow it, and each tab keeps its own
selection, undo history and Hierarchy scroll. Unapplied changes put a dot on the
tab; closing it then asks whether to apply them. Play always plays the scene.

Copies show in sage in the Hierarchy (their parts a little softer). Changing a
copy (moving it, a tint, a material, a script value, adding or deleting
objects in it) is kept as that copy's own: changed fields get a sage bar in the
Inspector, and right-click → Revert to Prefab drops one. On a copy's root,
**Apply** writes its changes into the prefab (every copy takes them, except
where another copy changed the same thing), and **Revert** drops them all
(except where it's placed).

Prefabs can contain other prefabs. A model placed in the scene is a prefab of
its own node tree (so a part can be animated or given a script), but can't be
changed itself: make a prefab containing it (as with `Windmill.prefab` in the
sample farm) to keep changes.

## Parents and children

Any entity can be the child of another, and moves, turns and scales with it
(see [Parents & children](../core/hierarchy/index.md)). In the Hierarchy:

| Action | Result |
| --- | --- |
| Drag an entity onto another | It becomes that entity's child, staying where it is in the world |
| Drag an entity onto the empty space below the list | It becomes a root, staying where it is |
| Right-click → Create Child | A new entity under this one, at its origin |
| Right-click → Unparent | It becomes a root, staying where it is |
| Right-click → Delete, or Del | Deletes it and everything below it (everything selected) |
| Ctrl+D | Duplicates it and everything below it, beside the original (each selected one) |

The Inspector shows a child's transform relative to its parent, and says which
parent. The viewport gizmo works in world space. An empty entity makes a good
folder: its children keep their places when it sits at the origin.

## The game camera

The viewport is the editor's own view: looking around while editing never moves
the game's camera. The game sees through a **camera entity** instead, an entity
with a Camera component (see
[Camera entities](../runtime/rendering/meshes-and-lighting/index.md#camera-entities)):

- **+ → Camera** adds one, looking the way the viewport is looking (in a 2D
  scene: an orthographic camera over the middle of the view, as tall as it).
- **Edit → Align With View** (Ctrl+Shift+F) moves the selection to the
  viewport's view, to re-aim a camera.
- **Edit → Align View With Selected** looks through the selection, to see what
  a camera sees.
- Cameras are drawn in the viewport as outlines of what they see. The game
  camera is drawn brighter.
- Scene Settings names the game camera: the first active one.
- Bloom and **Depth Of Field** are the camera's, in the Inspector, with
  their settings showing only while they're on. The viewport shows the game
  camera's while you look around. Depth of field is then focused on the
  viewport's orbit target.

Play and exported games see through the game camera, and scripts move it by
moving its entity. A scene without one starts from the viewport's view as last
saved, as before.

## Editing several at once

Select several objects with **Ctrl**+click (add or remove one) or **Shift**+click
(a range) in the Hierarchy, or **Ctrl**+click in the viewport. They're all
outlined, the one clicked last a little bolder.

- The **Inspector** shows the components they all have, and the fields of a
  script they all run. Changing a field sets
  it on every one of them. A field whose values differ between them is dimmed
  until you set it. **Add Component** adds one to each that hasn't one, and
  right-click → **Remove Component** removes it from them all. Each of these is
  one undo step.
- The **gizmo** sits in the middle of them (the average of where they are).
  Moving it moves them all as far; rotating or scaling it turns or scales
  each one as much, about its own origin. To turn a set of objects about
  their shared middle instead, put them under one parent and turn that.
- **Delete** and **Duplicate** work on all of them.

To give every wheel of a vehicle a Grass Bender, for example, select the wheels
and add it once.

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

While the Assets panel was the last panel used, the Edit menu and these keys
work on its file operations instead (see [Assets](#assets)). History is cleared
when another scene is opened. Edits made while playing aren't
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

With your own config in Auto mode, finding files and searching text look only
in the project's `scripts/` folder (not assets or builds): `Space f f` and
`Space Space` find files (with Telescope if you have it), `Space s g` and
`Space /` search (with LazyVim's picker, else Snacks or Telescope). Set
`vim.g.sfs_scripts_pickers = false` in your config to keep your own.

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

While playing, the Viewport takes focus as the game starts, so the game has the
keyboard straight away; click another panel to type there instead. The game
gets mouse clicks and the wheel only while the pointer is over its view, so
scrolling the Hierarchy or Inspector never zooms the game's camera.

Saving a script in the Code panel rebuilds the game module in the background
(so does coming back to the editor after changing scripts elsewhere); the
status bar shows the build. When it loads, fields you renamed, added or removed
are handled for you:

- **Renamed**: saved values follow the new name, in the open scenes and their
  undo history and in every scene and prefab file. A rename is a field that's
  gone while a new one of the same kind appeared in its place (the same
  position, or the only such pair), e.g. `float degreesPerSecond` becoming
  `float speed`.
- **Added**: everything gets the field's default from the code.
- **Removed**: its saved values are dropped from the scenes and files.

- **A script, component or system renamed** (its class and file, say `Spin`
  to `Spinner`): scenes and prefabs follow it too, when the new one has the
  same fields as the old, or it's the only one of its kind gone and the only
  one new.

Changing a field's kind (say `float` to `glm::vec3`) along with its name counts
as removing one field and adding another.

If the engine has changed since the project's game module was built, opening
the project rebuilds the module before loading it (an old module may not match
the engine and could crash it).

## Exporting a game

**File → Export Game...** builds the scripts and writes a playable copy of the
game into a folder (by default the project's `builds/<platform>/`). Choose the
**start scene** the game opens on; it's saved with the project. Exporting first
saves the open scene and applies any prefab tabs with changes, so the game has
everything as you're editing it. The folder can be zipped and handed to anyone,
with no editor or engine install needed:

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

## Grass

Add a **Grass** component to anything with a Mesh Renderer, such as a plane, a
terrain or a hillside model, and grass grows on every part of it that faces up.
The blades are real geometry, curved and tapering, varied in height and tone.
They sway in the scene's wind, and flatten where things move through them.

| Setting | What it does |
| --- | --- |
| Material | What the blades look like (none: a plain green). A node graph sees each blade's UV (U across it, V up it, 1 at the root and 0 at the tip). Its **Grass** node also gives each blade's length, a random number and the maps' Data channels (see [Material graphs](material-graphs/index.md#the-nodes)). |
| Density | Blades per square unit |
| Height, Height Variation, Width | The blades' size, and how much heights vary |
| Stiffness | How little they sway in the wind |
| Recovery | Seconds flattened grass takes to stand back up |
| Draw Distance | How far away grass is drawn. It's densest near the camera and thins out toward this distance. |

**Wind** is the scene's, in Scene Settings: its direction (degrees), strength
and gustiness. Gusts roll across the grass the way the wind blows. Long grass
waves in it, and grass mown short (painted with **Length**) hardly moves.

The grass's colour also tints the ground under it, so the gaps between blades
look like grass. To hide where grass stops being drawn, add **Haze** in Scene
Settings to fade the distance into the sky. You can also turn on the camera's
**Depth Of Field** to soften what's far away.

**What flattens grass:** anything with a Rigid Body, sized by its colliders,
unless its **Bends Grass** is off. A **Grass Bender** component does the same
for things without physics, such as a character moved by a script or a
vehicle's wheels. With **Fit Mesh** on (the default) it's sized to its entity's
own mesh: put one on each wheel and only the wheels bend grass. Turn it off,
or use it on something without a mesh, to set its **Radius** yourself. A
moving thing lays grass down the way it's going, fanning out a little at its
sides, all along its path. A still one presses it outward instead. Either
way, it springs back afterwards.

**The grass brush:** with grass selected, press **B** or click **Grass** in the
viewport toolbar, then drag over the grass. Hold **Shift** (or tick **Erase**)
to take away instead. The menu next to **Strength** picks what the brush
paints:

- **Coverage** paints grass in where it's thin; erasing clears it.
- **Length** grows it full length; erasing mows it short.
- **Data 0** to **Data 3** are numbers for the grass's material to read, such
  as moisture for a game to colour.

**Size** sets the brush's width in world units and **Strength** how fast it
paints. **Fill** sets all of it back to unpainted: full, full-length grass.
**Clear** removes it all, so you can paint it in only where you want it. Each
stroke is one undo step, and what's painted saves with the scene. A game can
change the same maps as it plays (see
[Meshes, materials & lighting: Grass](../runtime/rendering/meshes-and-lighting/index.md#grass)).

## Built-in meshes

New entities use the editor's built-in meshes: `builtin:cube`, `builtin:sphere`,
`builtin:cylinder` and `builtin:plane`. They're named, so scenes that use them
save and load. A mesh field's picker in the inspector lists every named mesh.
