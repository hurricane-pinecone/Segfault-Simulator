# Sprites & sprite sheets

A sprite is an image drawn at an entity: a character, a tile, a pickup. It
works in every [style](../styles/index.md): lying on a flat game's plane,
standing on an isometric grid, or standing upright in a 3D world, facing the
camera. `SpriteModule` draws them, lit like any mesh.

## SpriteComponent

Give an entity a `TransformComponent` and a `SpriteComponent`:

```cpp
sfs::SpriteComponent sprite;
sprite.image = "asset:7b957a18b3f06184fe71ec1a0c92e581"; // a texture asset
sprite.frame = 2;                                         // its third frame
sprite.anchor = {0.5f, 1.0f};                             // stand on its feet

scene.createEntity()
    .addComponent<sfs::TransformComponent>(glm::vec2{4.0f, 2.0f})
    .addComponent<sfs::SpriteComponent>(sprite);
```

| Field | Meaning |
| --- | --- |
| `image` | What to draw from: a texture asset (`"asset:<id>"`), or an image added to the `AssetStore` by name |
| `frame` | Which frame of the image's sprite sheet (0: the first). An image that isn't a sheet is one frame. |
| `anchor` | The sprite's pivot, in 0..1 of its size. `{0.5, 1}` is the bottom centre, so the transform sits at the sprite's feet. |
| `renderOffset` | A fixed nudge, in pixels, after anchoring |
| `tint` | Multiplies its colours (RGBA); alpha below 1 fades it. White by default. |
| `normalMap` | An image of the same layout holding its normals, so lights shade it per pixel. None lights it flat. |
| `castsShadow` | It shadows what's behind it, from the sun and from point lights that cast shadows. Off by default. |

A sprite's size is its frame's size in pixels, divided by its image's
`pixelsPerUnit` import setting (else the style's: a 2D game's `Space2D`, or
`SpriteModule::setPixelsPerUnit` in 3D), times the transform's `scale2D()`. A
negative scale mirrors it. A flat game's sprite turns with its transform's
`yaw()`; an isometric one always faces the view. A child sprite is drawn where
its parents put it (its world transform).

Scenes saved with the retired `SpriteTint` and `ShadowCasterComponent`
components load with them folded into the sprite's `tint` and `castsShadow`.

`SpriteComponent` is reflected, so it's saved with the scene and edited in the
Inspector, where `image` is a texture picker.

## Sprite sheets

A texture asset becomes a sprite sheet through its import settings, kept in
its `.meta` file (see [Asset database](../../../core/assets/index.md#textures)):

```json
"settings": { "filter": "nearest", "spriteCell": [16, 16] }
```

| Setting | Meaning |
| --- | --- |
| `"filter"` | `"nearest"` draws texels with hard edges (pixel art); the default is smooth |
| `"spriteCell"` | `[w, h]`: the image is a grid of frames this many pixels each, numbered across then down from the top left |
| `"pixelsPerUnit"` | How many of its pixels make one world unit when it's drawn as a sprite (a 64 pixel sprite at 32 is 2 units tall). Left out, the scene's. |

A `SpriteComponent`'s `frame` picks one; frames past the last wrap around, so
an animation can just count up. In the editor these are a texture's
**Filter**, **Sprite Sheet**, **Frame Size** and **Pixels Per Unit** in the
Inspector.

From code, `AssetStore::spriteFrame(image, frame)` gives a frame's image, its
rectangle on it (`uvRect`) and its size in pixels, and
`AssetStore::imageTexture("asset:<id>")` the image itself.

## Images from code

A game can also add an image to the `AssetStore` by name and draw from it:

```cpp
assetStore().addTexture("sheet", "assets/sheet.png");

sfs::SpriteComponent sprite;
sprite.image = "sheet"; // the whole image
entity.addComponent<sfs::SpriteComponent>(sprite);
```

Images added by name are drawn without smoothing (pixel art), and are one frame
(only a texture asset's import settings make a sheet). Use texture assets for
sprites placed in the editor.

## Drawing order

`RenderLayerComponent` sets a flat game's draw layer: higher layers draw in
front (layer 0 without one). An isometric game needs no layers: the depth buffer puts each sprite in front
of what's behind it.

Sprites whose image has soft edges (many part-transparent texels) draw
see-through, after everything solid; the rest are cut out at half alpha, so
pixel art stays crisp and sorts by depth.

## SpriteModule

`StandardScene` registers `SpriteModule`; a scene of your own registers it
with the asset store:

```cpp
render.withModule<sfs::SpriteModule>(assetStore());
```

| Call | Does |
| --- | --- |
| `setFacing(Facing)` | `Auto` (default): flat in a 2D game, upright in 3D. Or force `Flat` or `Upright`. |
| `setPixelsPerUnit(pixels)` | In a 3D world, the pixels per world unit of sprites whose image doesn't set its own (default 1; a 2D game uses its `Space2D`'s) |
| `placed()` | Where each sprite was drawn last frame, by entity id, for picking and outlines |
