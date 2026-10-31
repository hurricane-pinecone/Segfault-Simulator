# Visual styles, tone mapping & grading

A scene's *look* is everything about how it's drawn that isn't what's in it:
how light is turned into what the screen shows (tone mapping), the colour
grade, how strong the sun is against the sky light, bloom, haze, how soft
shadows are, and which version of each material is drawn. This page covers:

- **Tone mapping and colour grading** (`ColorGrading`), the last step of
  every frame.
- **Visual styles** (`VisualStyle`, `.style` assets): a whole look in one
  file, which a scene names and can switch while it runs. A game can keep a
  stylised look and a realistic one side by side and compare them in play.
- **Material variants**: another material a material draws as while a
  style's tag is on.
- **A physical sky with image-based lighting**: an atmosphere the sun lights,
  which lights the scene in turn, physically based.
- **Cascaded sun shadows**: shadows reaching hundreds of metres, sharp near
  the camera.
- **Ambient occlusion**: the sky's light kept out of creases and the foot of
  walls.
- **Contact shadows**: small, sharp sun shadows where things meet the ground.
- **Height fog**: mist lying low, coloured by the sky and glowing toward the
  sun.
- **Terrain materials**: big ground textured with several layers (grass,
  dirt, paths) painted with a splat map.
- **Switching** in the editor and with the development key (F8).

## Tone mapping and grading

The scene is lit into an HDR texture. The final pass (`fs_composite` in
`post.wgsl`) adds [bloom](../meshes-and-lighting/index.md#bloom) to it, then
grades it, all in one full-screen pass:

1. **On the light** (linear, before tone mapping): exposure, white balance
   (temperature and tint, in LMS cone space from D65, as cameras do it),
   contrast (around mid-grey, in stops) and saturation.
2. **The tone mapper**: light into the screen's 0..1.
3. **On what the screen shows**: lift / gamma / gain, then the vignette.

`ColorGrading` (`engine/core/rendering/colorGrading.h`):

| Field | Meaning (default: changes nothing) |
| --- | --- |
| `toneMapper` | `None`, `Aces`, `AgX` or `Neutral` (below) |
| `exposure` | In stops (EV): +1 doubles the light, -1 halves it (0) |
| `temperature`, `tint` | White balance, -1..1: cooler..warmer, greener..more magenta (0) |
| `contrast` | Around mid-grey: 1 as is, above punchier (1) |
| `saturation` | 0 grey, 1 as is, above more colourful (1) |
| `lift`, `gamma`, `gain` | Per channel: added to the shadows (0), bends the midtones (1; above brighter), multiplies the highlights (1) |
| `vignette`, `vignetteSoftness` | How much the corners darken (0), and how far in it starts (0.5) |
| `autoExposure` | The exposure follows what's on screen, as eyes do: a view brighter than the target darkens, a dimmer one brightens, easing there over a second or so (false) |
| `exposureTarget` | The average brightness auto exposure aims for, in stops from mid-grey (0) |
| `exposureDarken`, `exposureBrighten` | The most it darkens a bright view and brightens a dim one, in stops (1.5, 0.5). On top of a physical sky's own adaptation to dusk and night: 0 brightening keeps nights as dark as the sky makes them |
| `exposureSpeed` | How fast it adapts, about stops a second (1.5) |

The tone mappers:

| Tone mapper | Look |
| --- | --- |
| `None` | Clamped at white: the engine's classic look, and the default. Lighting is clamped at white too (lit surfaces never go above 1), so only emission blooms. |
| `Aces` | Stephen Hill's fit of the ACES RRT/ODT (film): punchy contrast, highlights rolled off, a little warmth. |
| `AgX` | Troy Sobotka's filmic curve (Blender 4's): bright colours fade toward white as on film, mid-grey kept. Flatter than ACES; give it some exposure. |
| `Neutral` | Khronos PBR Neutral: the materials' colours kept as they are, only the brightest compressed. |

With any tone mapper but `None`, lighting is no longer clamped at white: a
sunlit surface can be brighter than 1, and the curve rolls it off instead.
Raise bloom's threshold to match (or a sunlit field blooms).

A scene's own grading is `RenderSystem::setColorGrading()`, saved in its
render settings as `"grading"`; Scene Settings shows it under **Look** while
no visual style is chosen. Scene files from before grading existed load with
none (`toneMapper` `None`), so they look exactly as they did. An ungraded
frame (`ColorGrading::neutral()`) skips the grading entirely: the composite
only clamps, as it always did.

## Visual styles

A visual style is a `.style` asset (JSON, with a `.meta` like every asset;
`AssetKind::VisualStyle`), loaded as a `VisualStyle`
(`engine/core/rendering/visualStyle.h`):

```json
{
  "formatVersion": 1,
  "grading": {"toneMapper": "aces", "exposure": 0.25, "saturation": 0.85, "vignette": 0.3},
  "lighting": {
    "sunIntensity": 1.6, "sunTint": [1.0, 0.93, 0.82],
    "ambientIntensity": 0.75, "ambientTint": [0.82, 0.92, 1.08],
    "skyIntensity": 1.0, "skyTint": [0.9, 0.97, 1.05]
  },
  "bloom": {"enabled": true, "intensity": 0.25, "threshold": 1.4, "softness": 0.6, "radius": 1.2},
  "haze": {"strength": 0.35, "distance": 90},
  "shadows": {"softness": 2.6, "cascades": 4, "cascadeDistance": 200, "cascadeSplit": 0.75},
  "sky": {"model": "physical", "turbidity": 2.5, "groundAlbedo": 0.25, "sunSize": 1.5,
          "intensity": 1, "nightBrightness": 1},
  "ambientOcclusion": {"enabled": true, "radius": 1.2, "intensity": 1, "samples": 12},
  "contactShadows": {"enabled": true, "length": 0.5, "thickness": 0.3, "strength": 0.8},
  "fog": {"enabled": true, "density": 0.006, "heightFalloff": 0.12, "baseHeight": 0,
          "startDistance": 15, "maxOpacity": 0.85, "sunGlow": 0.6},
  "materialVariant": "realistic"
}
```

Every section and key is optional; what's missing keeps its default, and the
defaults change nothing (an empty style looks exactly like no style).

### What a style does to the scene

While a style is on (precedence, simply):

| | |
| --- | --- |
| **Replaces** | The scene's grading and tone mapping, always. Bloom (the scene camera's), haze, the sky, each shadow setting (`softness`, `cascades`, `cascadeDistance`, `cascadeSplit`), ambient occlusion, contact shadows and fog, only when the style has them (`bloom`, `haze`, `sky`, `ambientOcclusion`, `contactShadows`, `fog`, and each key of `shadows`, are optional; left out, the scene's stay). A style's section with `"enabled": false` turns the scene's off while it's on. |
| **Multiplies** | The scene's light: the sun's share of each surface's light (what shadow and turning away take) by `sunIntensity` x `sunTint`; the ambient rest by `ambientIntensity` x `ambientTint`; the sky colour (background, haze, reflections) by `skyIntensity` x `skyTint`. A day/night cycle that sets the lighting every frame keeps working underneath. Under a [physical sky](#physical-sky-and-image-based-lighting): the sun's light by the sun balance, the sky's diffuse light and reflections by the ambient balance, the background and haze by the sky balance. |
| **Switches** | Materials with a variant for its `materialVariant` tag draw as it (below); and any material named in its `"materials"` (name -> material) draws as that one, including materials a game makes in code, which have no variants (e.g. `{"farm:field-paint": "asset:<terrain>"}`). A terrain standing in for a material with a base texture is tinted by that texture, so what the game paints there still shows. |

The scene's own settings are never changed: switch the style off and they're
back. With no style (`""`), the scene's own settings are the look.

```cpp
render.setStyleSource(...);           // StandardScene does this: .style assets by "asset:<id>"
render.setVisualStyle("asset:9c1e…"); // live: the next frame draws with it
render.setVisualStyle("");            // none: the scene's own look
render.setVisualStyle(name, style);   // as given (an edit being previewed)
render.reloadVisualStyle();           // its file changed
render.visualStyleName();             // saved with the scene as "visualStyle"
render.visualStyle();                 // null if none, or it couldn't be loaded
render.effectiveGrading();            // what the frame uses (also Bloom/Haze/Shadows)
```

A scene names its style in its render settings, `"visualStyle": "asset:<id>"`
(`""` for none). A style that can't be loaded keeps its name, so saving
doesn't lose it, and the scene looks as with none. `AssetStore::visualStyle(id)`
reads a style from its file each time it's asked for (they're small, and asked
for only when switching), so an edited file shows the next time.

### Physical sky and image-based lighting

A scene's sky (`SkySettings`, `engine/core/rendering/sky.h`;
`RenderSystem::setSkySettings()`, saved in its render settings as `"sky"`; a
style's `"sky"` section replaces it) is **flat** by default: the sky colour
behind everything, and the engine's classic lighting, exactly as before skies
had a setting. A **physical** sky is an atmosphere the sun lights, and it
lights the scene in turn:

- **Behind everything**: the sky as seen from the ground. Blue by day, paler
  toward the horizon; at sunrise and sunset, warm toward the sun and dimmer
  overhead; after dark, a dark blue night glow with stars. The sun's disk
  (darker at its edge) is bright enough to bloom.
- **The sun's light** is what gets through the air from it: near white at
  noon, orange then red as it sets, none once it's down. The scene's sun
  *direction* is all that's used: its colour, strength and the sky colour are
  worked out instead (so a day/night cycle that only moves the sun is enough;
  one that also sets the colours, as CozyFarm's does, keeps working, its
  colours simply unused).
- **The sky's diffuse light** (what lights shade and the sides turned away):
  the whole sky, and the ground's bounce below, as nine spherical harmonics
  (worked out on the CPU from the same model).
- **Reflections**: the sky prefiltered for each roughness (GGX, split-sum with
  a BRDF table), so metal and glossy paint reflect a blurred sky; plus the
  sun's own glint.
- **Haze** fades far things into the sky at the horizon behind them.

Surfaces under it are lit physically: Lambert diffuse from the sun, the sky's
diffuse light, split-sum specular reflections and GGX sun glints. Point lights
(lamps at night) light as they do under a flat sky, and the scene's
*ambient* level still fades them in as night falls.

| `SkySettings` | Meaning (default) |
| --- | --- |
| `model` | `Flat` or `Physical` (`"flat"`, `"physical"`) (Flat) |
| `turbidity` | How hazy the air is: 1 very clear, 2 a clear day, 4-6 hazy, 10 thick haze (2) |
| `groundAlbedo` | How much light the ground reflects: below the horizon, and the light it bounces up (0.3) |
| `sunSize` | The sun's disk, x the real sun's size; 0 draws none (1) |
| `intensity` | How bright the sun and sky are, x (1) |
| `nightBrightness` | The night sky's glow and its stars, x; 0 a black night (1) |
| `adaptation` | How much eyes adapt as the sky dims (dusk, night): 0 a fixed exposure (physically dark evenings), 1 fully, up to 3 stops (0.5). It brightens the sky's own light (the sun's, the sky's, its reflections, the background). Point lights and glowing things keep the brightness they were tuned to for night eyes, so by day they look dimmer by as much (an eighth at noon, adapted fully): daylight drowns them out unless they're strong, and point lights too dim to see cast no shadows (none are drawn) |

Its light is scaled so a sunlit mid-grey shows as mid-grey with no exposure
(the noon sun is about 1 on a surface facing it); the sun's disk goes far
past white, so a physical sky wants a tone mapper (Grading) and blooms. It's
a 3D scene's; a 2D game's sky is always flat.

**The model** (`PhysicalSky`): single scattering through Earth's air
(Rayleigh, Mie, ozone; Hillaire's coefficients) with an approximation of
multiple scattering, from a transmittance table worked out on the CPU. The
same model runs on the GPU (`sky.wgsl`) to render the sky into a small
octahedral texture (256 x 256, the whole sphere) and its reflections (128 x
128, five roughness mips) with fragment passes, only when the sun has moved a
quarter of a degree (or the settings changed); the CPU works the sun's light
out every frame and the diffuse light along with each re-render. So it costs
next to nothing on a frame the sun hasn't moved, and a few small passes when
it has.

What it costs (an RTX 3070, 1600 x 900, 5,000 shadow-casting objects, the
frame start to finish): with the sun still, nothing measurable over a flat sky
(1.48 ms against 1.50); on a frame the sun has moved far enough, about 2 ms
of CPU (the diffuse light, worked out again) and half a millisecond of GPU
(the sky and its reflections, rendered again). A day/night cycle with a
4-minute day does that about six times a second. Four shadow cascades over
200 m cost about 0.6 ms more than the one map (2.1 ms against 1.5 ms).

It needs nothing beyond plain 2D textures and render passes (no cube maps,
no compute), so it's the same on Vulkan, D3D12, Metal, wgpu's OpenGL fallback
and in browsers (WebGPU). Should its passes fail to build (logged), the scene
still lights from the sky's diffuse light and reflects that, in front of the
flat sky colour.

### Ambient occlusion

`AmbientOcclusionSettings` (`engine/core/rendering/ambientOcclusion.h`;
`RenderSystem::setAmbientOcclusionSettings()`, saved in the scene's render
settings as `"ambientOcclusion"` once it's not the default; a style's
`"ambientOcclusion"` replaces it) keeps the light from all around out of
creases, corners and the ground at the foot of walls: the sky's diffuse light
and what shiny things reflect under a physical sky, the ambient share of the
light under a flat one. Never the sun's light or point lights'. Off by
default, when it costs nothing and the frame is exactly as before.

| Field | Meaning (default) |
| --- | --- |
| `enabled` | Whether at all (false) |
| `radius` | How far around a point it looks for what blocks it, world units (1) |
| `intensity` | How dark it gets: 0 none, 1 normal, up to 4 (1) |
| `samples` | Points looked at around each pixel, 4 to 32: more is smoother and costs more (12) |

How: what's drawn opaque is drawn again at half the screen's size, as its
normals and depth (`meshDepthNormals.wgsl`; cut-out materials only where
solid). A full-screen pass (`ambientOcclusion.wgsl`) looks at points around
each pixel on a spiral, turned differently for each pixel, and counts how much
each rises above the pixel's surface, less the farther off it is (a
horizon-style estimate, as in McGuire's scalable AO). Two blur passes smooth
it across and down, weighting by depth so it doesn't bleed over edges, and the
lit pass reads its four texels around each pixel, again by depth. All of them
fragment passes, the same on Vulkan, D3D12, Metal, wgpu's OpenGL fallback and
browsers. It fades out between 60% and 90% of the far plane (at most 120 to
180 m), where depth is held too coarsely for it.

Grass blades don't occlude: drawn into it they'd darken whole fields. The
ground under them does, so blades near a wall share the ground's shade there
(a blade counts what's behind it only when it's at the same depth; others go
without).

### Contact shadows

`ContactShadowSettings` (`engine/core/rendering/contactShadows.h`;
`RenderSystem::setContactShadowSettings()`, saved as `"contactShadows"` once
it's not the default; a style's `"contactShadows"` replaces it) adds the
small, sharp shadows where things meet the ground and in fine crevices (a
fence post's foot, under a crate or a wheel, around feet), which the sun's
shadow maps are too coarse to hold. They take only the frame's direct light
(the sun's, or the moon's while it's in the sun's place): never the sky's,
the ambient level or point lights'. Under a physical sky only; off by
default, when they cost nothing.

| Field | Meaning (default) |
| --- | --- |
| `enabled` | Whether at all (false) |
| `length` | How far toward the light each ray goes, world units (0.5) |
| `thickness` | How thick what's drawn is taken to be behind its front, world units: the ray passing farther behind something than this isn't blocked by it (0.3) |
| `strength` | How dark they get: 0 none, 1 none of the sun's light (0.8) |

How: they're worked out in ambient occlusion's half-size pass, from the same
depth and normals (which run when either is on), into the map's third
channel. From each pixel, a little off its surface, a ray of 12 steps (its
start jittered per pixel) goes toward the light over the screen; it's blocked
where it passes behind what's drawn by less than the thickness. How far
behind is measured from that texel's surface plane (by its normal) where the
ray crosses it, not from the texel's middle, so flat ground at a low sun
doesn't shade itself. They fade toward the ray's end, at the screen's edges,
and with distance (gone by half of ambient occlusion's reach, 100 m at most).
A narrower blur than ambient occlusion's keeps them sharp, and the lit pass
reads them as it does the occlusion, multiplying them into the sun's shadow.
Grass blades aren't in the depth, so they don't cast them (no darkened lawn);
the ground under them takes them from what stands on it.

### Temporal anti-aliasing

`RenderSystem::setTemporalAntiAliasing()` (saved as `"temporalAntiAliasing"`
when on; a style's `"temporalAntiAliasing"` replaces it; Scene Settings and
the style inspector have it) smooths edges and the shimmer of fine detail,
grass and fences above all, as the camera moves. Each frame the camera is
moved a fraction of a pixel (the next point of a Halton 2, 3 sequence) and the
lit scene is blended with what came before (`taa.wgsl`): each pixel's history
is found where its point in the world was last frame (through the depth
buffer and last frame's camera), clamped to the colours around the pixel now
so what moved or came into view doesn't smear, and a tenth of the new frame is
mixed in. The result replaces the scene for bloom, depth of field and the
composite. Not on OpenGL (it reads the lit pass's depth samples, as depth of
field does), nor in 2D. Things that move on their own (swaying grass, a
driving tractor) are reprojected as if still, so the clamp is what keeps them
from ghosting. The development key **F9** turns it on or off to compare (see
[Switching](#switching)).

### Height fog

`FogSettings` (`engine/core/rendering/fog.h`; `RenderSystem::setFogSettings()`,
saved as `"fog"` once it's not the default; a style's `"fog"` replaces it) is
air thicker low down: exponential height fog, as mist lies in a valley of a
morning. What's seen through it fades toward the fog's colour: under a
physical sky, the sky at the horizon toward where you look, plus the sun's
light scattered forward by the fog (Henyey-Greenstein, g 0.75) so it glows
toward a low sun; under a flat sky, the sky colour (as haze). Opaque things,
grass, see-through things and the sky's background (out to the far plane)
are fogged alike, and what's fogged blooms less.

| Field | Meaning (default) |
| --- | --- |
| `enabled` | Whether at all (false) |
| `density` | How thick it is at its base height: how much light a metre takes (0.02) |
| `heightFalloff` | How quickly it thins going up, per metre: 0 the same at every height (0.2) |
| `baseHeight` | The height (world Z) it's `density` thick at (0) |
| `startDistance` | How far from the camera it starts (0) |
| `maxOpacity` | The most it hides anything, however far (1) |
| `sunGlow` | Its glow toward the sun, under a physical sky: 0 none, 1 about physical (0.5) |

The amount is the density integrated along the line from the camera in
closed form, 1 - e^-depth, so it's exact for any slope (`fogOpacity()` works
it out on the CPU the same way, `fog.wgsl` on the GPU).

**It takes haze's place**: while fog is on, haze (the scene's or the style's)
isn't drawn. Off, haze is exactly as it was, so scenes and styles from before
fog look the same.

### Cascaded shadows

`ShadowSettings::cascades` (a style's `"shadows": {"cascades": ...}`) of 2 to
4 replaces the one sun shadow map around what the camera looks at with
cascades along the view, out to `cascadeDistance` (200 m): each covers a
farther slice of the view at less detail, so shadows are sharp at your feet
and still there across the valley. 1, the default, is the one map as ever.
See [Shadows](../meshes-and-lighting/index.md#cascaded-shadows).

### Material variants

A material (`.mat`, standard or node graph) can name other materials to draw
as under a style's tag:

```json
{
  "formatVersion": 2,
  "baseColor": [1, 1, 1, 1],
  "roughness": 1,
  "variants": {"realistic": "asset:c0ffee…"}
}
```

While the scene's style has `"materialVariant": "realistic"`, everything using
this material (mesh renderers' material slots, and grass) draws with the
variant instead. `MeshLibrary::setMaterialVariant(tag)` does the switch (the
render system calls it as styles change): it works out each loaded material's
drawn material once, so `MeshLibrary::material(name)`, which modules call for
every entity every frame, stays one lookup, without making a string. A
variant's own variants aren't followed. A variant that can't be loaded draws
as the material itself.

Only named materials (material assets, and those games make with
`setMaterial()`) have variants. A mesh's own materials (a glTF's, or vertex
colours with no material) don't; to give such an object a variant, put a
material in its slot that draws the same as its own (for vertex colours, a
plain white, matte standard material does) and give that the variant.

## Switching

**In the editor**, Scene Settings > **Look** > **Visual Style** picks the
scene's style (None, or any `.style` in the project); it's one undo step and
saved with the scene. **Edit Style** selects the asset. Selected, a style is
edited in the Inspector, every field, saved as you change it and shown in the
scene at once when the scene uses it (**Use in Scene** if it doesn't). Create
one with right-click in Assets > **New Visual Style**. A material's
**Variants** (tag, material) are edited in the Inspector too.

**While playing**, the development key **F8** switches to the project's next
visual style (**Shift+F8**, the one before), wrapping around, in path order
(`cycleVisualStyle()`, `engine/runtime/rendering/visualStyles.h`); the log
says which. **F9** turns [temporal anti-aliasing](#temporal-anti-aliasing) on
or off, over the scene's and its style's own setting, to compare the two; the
window title and the log say which. Both are built into `Game`, so every
game has them, and aren't something a game's scripts see (games can't bind
F-keys; the dev console is the backtick). The development keys are on:

- in the editor's Play, always;
- in a debug build of the player;
- in an exported game only if it was exported as a **development build**
  (Export > **Development build**, or `SfsEditor --export --development
  <project> [folder]`), which writes `"development": true` to its
  `game.json` (`GameManifest::development`).

A release game exported normally never has them. Code can turn them on or off
with `Game::setDevKeys()`.

### Terrain materials

A terrain's look comes from a material, so it has variants like any other: a
game's ground can be one plain material in a stylised look and a
[terrain material](../meshes-and-lighting/index.md#terrain-materials) (layers
of grass, dirt and paths blended by a painted splat map) under a realistic
style's tag. Grass growing on it grows only where its layers let it, so a
dirt yard is bare in that style and grassed in the other.

## What it costs

Ambient occlusion adds a half-size pass drawing every opaque object again
(depth and normals only) and three half-size full-screen passes; off, it adds
nothing. Contact shadows share those passes (12 more texture reads a
half-size pixel when they're on; with ambient occlusion off they bring the
passes in themselves). Height fog adds a few instructions a pixel and no passes. A terrain
material samples, per layer present at a pixel, its two arrays three times
(with anti-tiling), so ground where two layers meet costs about twelve
texture reads; a `triplanar` layer twice that, but only on ground steeper than
about 35 degrees.
