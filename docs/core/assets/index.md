# Asset database

`AssetDatabase` is the catalogue of a project's asset files: everything in
its `assets/` and `prefabs/` folders. Each file gets a stable **asset id**, kept in a small
`.meta` file beside it, so the asset keeps its identity when it's renamed or
moved. It's in engine-core and knows nothing about rendering, so games of
every style, the editor and tools all share it.

It catalogues files; it doesn't load them. Loading is the
[`AssetStore`](#how-it-fits-with-the-assetstore)'s job.

```cpp
#include "engine/core/assets/assetDatabase.h"

// The editor, read-write; paths are relative to the project.
sfs::AssetDatabase assets("MyGame", sfs::AssetDatabase::Mode::ReadWrite,
                          sfs::AssetDatabase::projectFolders()); // assets/, prefabs/
const sfs::AssetRecord* barn = assets.findByPath("assets/models/barn.glb");
sfs::AssetId id = barn->id;                             // keep this, not the path
assets.absolutePath(*assets.find(id));                  // wherever it is now
```

## .meta files

`barn.glb` gets `barn.glb.meta`:

```json
{
  "formatVersion": 1,
  "id": "63caefd5affab3338cca8085cf7c5d24",
  "settings": {}
}
```

- `id` is 128 random bits, as 32 hex digits.
- `settings` holds the asset's import settings (for example how a texture is
  filtered), for the importers to read.

**Commit `.meta` files with their assets.** A file moved outside the editor
without its `.meta` (in Explorer, say) gets a new id, and anything that
referred to the old one loses it. Moves made in the editor's Assets panel take
the `.meta` along.

## Scanning

`refresh()` rescans the folder. Read-write, it also:

- writes a `.meta` with a new id for each file that has none
- gives a copy its own id: when two files claim one id, the one that had it in
  the catalogue before keeps it (else the first by path), so `a - Copy.png`
  never takes `a.png`'s id, however the names sort

A `.meta` that can't be read (a merge conflict left in it, a bad hand edit) is
never overwritten, since that would lose the id everything refers to the asset
by: an error is logged, the file is listed without an id, and it gets its id
back once the `.meta` is fixed (or a new one, if the `.meta` is deleted). An
empty `.meta` has no id to keep and gets a new one. `.meta` files are written
atomically (a temporary file swapped in), so a crash can't leave one half
written.

Hidden files and folders (names starting with `.`) are skipped. `records()`
lists every asset sorted by path, and `folders()` lists the folders.
`version()` goes up whenever anything changes, so a UI can tell when to redraw.

`refresh()` is `apply(scan())`. For a UI that mustn't block (the editor's
Assets panel on a large project), run `scan()` on another thread and pass its
result to `apply()` back on the database's own thread: `scan()` only reads the
folders (and, read-write, writes `.meta` files for new assets); it touches
nothing of the database itself.

| Lookup | Returns |
| --- | --- |
| `find(id)` | The asset with that id, or null |
| `findByPath("assets/models/barn.glb")` | The asset at that path (relative to the root), or null |
| `absolutePath(record)` | Where its file is on disk |

Each `AssetRecord` has its `id`, `path`, `kind` and `settings`. The kind comes
from the file extension: `Texture` (png, jpg, bmp, tga), `Model` (glb, gltf,
obj), `Material` (mat), `Prefab` (prefab), `Scriptable Object` (asset), `Font`
(ttf, otf), `Audio` (wav, ogg, mp3, flac), `Data` (json, txt, csv) or `Other`.

## Editing

These change the files on disk together with their `.meta` files, then
rescan. Each returns false, and logs the reason, on failure.

| Call | Does |
| --- | --- |
| `move(id, "assets/props/box.png")` | Moves or renames an asset (creating folders); refuses to overwrite |
| `moveFolder("assets/props", "assets/world/props")` | Moves or renames a folder with everything in it |
| `remove(id)` | Deletes an asset |
| `removeFolder("assets/old")` | Deletes a folder and everything in it |
| `createFolder("assets/sounds")` | Creates a folder |
| `import(file, "assets/textures")` | Copies an outside file in as a new asset (taken names get a number: `tree 2.png`) and returns its id |
| `setSettings(id, json)` | Saves new import settings to its `.meta` |

A shipped game opens the database with `Mode::ReadOnly`. It never writes, and
the editing calls refuse.

Without a folder list (`AssetDatabase("some/folder")`) it catalogues everything
under its root, with paths relative to that.

## How it fits with the AssetStore

| | Holds | Keyed by |
| --- | --- | --- |
| `AssetDatabase` | Which files exist, their ids and import settings | `AssetId` |
| `AssetStore` | Loaded data: images (`SDL_Surface`), sprites, fonts | Names game code chooses |
| | Models (meshes, and the textures inside them) | `AssetId` |
| The renderer | GPU copies of the store's images and meshes | The store's names, or the mesh |

The two are separate on purpose: the database is the catalogue and the store is
the cache. `AssetStore::model(id)` looks the model up in the database, imports
it on first use and keeps it. The renderer uploads each of a model's
textures once, however many meshes share it.

## Models

glTF files (`.glb`, or `.gltf` with its `.bin` and images beside it) import as
meshes, Z up. A scene names a model's mesh `asset:<id>` (the whole model, as
the file places its pieces) or `asset:<id>:<mesh name>` (one of its meshes on
its own, when it has several).

Each of a model's materials becomes a *section* of its mesh, with that
material as its own; an object can swap any of them (see
[Materials](#materials)). From each material, the importer takes:

- the base colour, which multiplies the vertex colours (so untextured low-poly
  models keep their palette)
- the base colour texture, inside a `.glb` or as an image file (png, jpg, bmp,
  tga) beside a `.gltf`. A texture whose sampler asks for `NEAREST`
  magnification is drawn without smoothing, for pixel art.
- metal and roughness: the factors, and the metallic-roughness texture (metal
  in blue, roughness in green). Metal reflects the sky and catches the sun;
  matte paint looks as it always did. A file that leaves metal out gets glTF's
  default, fully metal.
- the alpha mode: `MASK` cuts out where the alpha is below the cutoff (leaves,
  fences), and `BLEND` makes it see-through by its alpha (glass, water). Both
  cast shadows only where solid.
- the normal map, read as data (not colour) along with its scale, for bumps
  that catch the light without adding geometry.

Occlusion and emissive maps are ignored for now. A texture that can't be read
is logged, and its part of the model drawn untextured.

## Materials

A material is how a surface looks: base colour and texture, metal, roughness
and transparency. Meshes are only shape; materials are separate.

- A mesh has one **material slot** per section (one for the built-in meshes,
  one per material for a model), each with the mesh's own material.
- A Mesh Renderer's **Materials** list gives each slot a material asset in
  place of the mesh's own; an empty entry keeps the mesh's own.
- A **scriptable object** (`.asset`, JSON: `{ "formatVersion", "type", "fields" }`)
  holds the field values of a scriptable object type declared in scripts
  (`sfs::ScriptableObject`); `sfs::ScriptableObjects` loads them. See
  [Scripting](../../editor/scripting/index.md#scriptable-objects).
- A **material asset** (`.mat`, JSON; see `MaterialAsset`) names its textures
  by asset id. A node-graph material (format version 2) holds its graph instead;
  see [Material graphs](../rendering/material-graphs/index.md#in-a-material-file).
  A standard one looks like this:

```json
{
  "formatVersion": 1,
  "baseColor": [1, 1, 1, 1],
  "baseTexture": "asset:7b957a18b3f06184fe71ec1a0c92e581",
  "metallicRoughnessTexture": "",
  "normalTexture": "",
  "normalScale": 1,
  "tiling": [1, 1],
  "offset": [0, 0],
  "metallic": 0,
  "roughness": 1,
  "alphaMode": "opaque",
  "alphaCutoff": 0.5
}
```

`tiling` repeats the textures (`[4, 2]`: four times across, twice up) and
`offset` shifts them, in texture widths; a model's base colour texture
transform (glTF's `KHR_texture_transform`, scale and offset) imports as these.
`alphaMode` is `opaque`, `mask` or `blend`, as in glTF. `AssetStore::material(id)`
loads one, with its textures. Either kind of material may also have
`"variants": {"<tag>": "asset:<id>"}`: materials to draw instead while the
scene's visual style has that tag (see
[Visual styles](../../runtime/rendering/visual-styles/index.md#material-variants)).

- A **visual style** (`.style`, JSON; see `VisualStyle`) is a whole look for a
  scene: tone mapping and grading, light balance, bloom, haze, shadow
  softness and a material variant tag. `AssetStore::visualStyle(id)` reads
  one. See [Visual styles](../../runtime/rendering/visual-styles/index.md).

The built-in cube and plane map a texture once onto each face.

### Textures

A texture's `.meta` settings say how it's brought in (`TextureImportSettings`):

```json
"settings": { "filter": "nearest", "spriteCell": [16, 16] }
```

- `"filter": "nearest"` draws texels with hard edges, for pixel art; the
  default is smooth.
- `"spriteCell": [w, h]` makes it a sprite sheet: a grid of frames this many
  pixels each, numbered across then down from the top left. A
  `SpriteComponent`'s `frame` picks one (see
  [Sprites & sprite sheets](../../runtime/rendering/sprites/index.md)).

`AssetStore::imageTexture("asset:<id>")` gives a texture asset's image, and
`AssetStore::spriteFrame(image, frame)` one frame of it. In the editor these
settings are the texture's **Filter**, **Sprite Sheet** and **Frame Size** in
the Inspector.
