# Shaders

Contributor notes on how the engine's WGSL is put together. For game-developer
documentation (using the engine), see the docs site under `docs/`.

## Where they are and how they're built

The shaders are real `.wgsl` files in `engine/src/shaders/`, embedded into the
binary at build time (`engine/cmake/embedShaders.cmake`): nothing is loaded
from disk at run time. `meshShaders.h` composes them into each pipeline's
source:

- A mesh pass is `meshPass(surface, pass)`: `meshCommon.wgsl` (the `Frame`
  uniform and the helpers every pass shares), `skyCommon.wgsl` (the sky's and
  clouds' helpers), `meshSurface.wgsl` (what a surface gives the passes), then
  the material's surface (`meshStandardSurface.wgsl`, `meshFoliageSurface.wgsl`,
  `meshTerrainSurface.wgsl`, or a material graph's compiled WGSL from
  `graphSurface()`), then the pass (`litPass()`: `meshLit.wgsl`, `fog.wgsl`
  and a vertex stage; the shadow, depth and point-shadow passes).
- Grass, rain, the sky background, debug lines, post-processing and the
  compute passes (grass growth and culling, puddles) are composed the same
  way, each from the files it needs.

A material graph's surface can read anything the `Frame` holds (its Time node
reads `in.time`; its Global Parameter node `frame.globals[slot]`), since
`meshCommon.wgsl` comes first in every pass a surface is built into.

## The frame uniform

`meshCommon.wgsl`'s `Frame` (group 0, binding 0) must match `FrameUniforms`
in `engine/include/engine/runtime/rendering/framePlan.h` field for field; a
`static_assert` there checks its size. `FramePlan::finish()` fills it from the
`FrameParams` the render system builds each frame. Among its fields:

| Field | What it holds | Where it comes from |
| --- | --- | --- |
| `wind` | xy which way it blows (unit), z how hard things sway, w gustiness: what foliage, grass and the ground's sheen sway with | the scene's `Wind` (`windSway()` of its speed; still air without one) |
| `rainClouds` | x how hard it rains, w the cloud cover (each 0..1; yz unused) | the scene's `Rain` and `Clouds` |
| `windDrift` | xy the wind's velocity (m/s, world XY), zw how far the clouds have drifted (km, within `kCloudPeriod`) | `windVelocity(Wind)`; the render system's cloud drift |
| `cloudLight`, `cloudBase` | a physical sky's clouds' light (their darkness in `cloudLight.w`) | the scene's `Clouds`, through `updateSky()` |
| `rainDrift` | how far rain's drops have drifted, and the wind they slant in (eased) | `RainDrift`, on the Wind |
| `lightning*` | the brightest strike's channel and cloud glow | `LightningComponent` entities, through `placeLightning()` |
| `globals` | `array<vec4f, 16>`: the scene's global shader parameters, each at its slot | `ShaderGlobals`, by `findShaderGlobalSlot()` |

`globals` is last. The engine's own parameters have fixed slots, named in
`meshCommon.wgsl` as `GLOBAL_WETNESS` (0: `surfaceWetness()`, meshLit) and
`GLOBAL_PUDDLES` (1: `terrainPuddles()`, meshTerrainSurface), and their x is
clamped 0..1 on the CPU. The slot count is `kMaxShaderGlobals`
(`shaderGlobals.h`); a second `static_assert` in framePlan.h ties it to the
16 written in the WGSL, so change both together. See
[Light, sky, wind, global shader parameters and lightning](runtime/light-and-sky/index.md)
for where the render system reads each.

Adding a field: add it to both structs at the same place (16-byte vectors and
mat4s only, so no padding rules come into it), update the `static_assert`,
and fill it in `FramePlan::finish()`.
