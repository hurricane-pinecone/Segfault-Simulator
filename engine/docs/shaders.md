# Shaders

Contributor notes on how the engine's GLSL is structured. For game-developer
documentation (using the engine), see the docs site under `docs/`.

## Lit shader architecture

The lit quad shader is split into a shared base and a renderer-specific variant,
composed at shader-build time so each renderer's program contains only the GLSL
it actually needs.

`engine/src/runtime/rendering/gl/shaders/quad.frag` is the shared core: the
common uniforms, `main()`, and three hook prototypes (`surfaceEffect`,
`sunShadow`, `pointLighting`). `OpenGLQuadRenderer::createShaderProgram` builds
the fragment source as `glslVersion + quad.frag + litShaderImpl()`, where
`litShaderImpl()` is a protected virtual returning the variant that defines those
hooks:

- `quadBase.frag` (flat/base): `surfaceEffect` and `sunShadow` are no-ops;
  `pointLighting` uses planar distance only. Returned by `OpenGLQuadRenderer`.
- `isometric/quadIso.frag` (isometric): terrain sun shadow via a heightfield
  horizon-angle march, point-light terrain occlusion, and procedural tile surface
  effects (grass, sand), plus the heightmap uniforms. Returned by
  `IsometricGeometryRenderer`.

So a flat 2D program contains no heightfield GLSL, and an isometric program
contains no pure-2D simplification; `quad.frag` is byte-identical for both. GLSL
function prototypes let `main()` call the hooks that the appended variant defines.

The shaders are real files embedded into the binary at build time (see
`engine/cmake/embedShaders.cmake`), so there is no runtime shader file loading.

**Extension point:** a renderer subclass can override `litShaderImpl()` to supply
its own variant GLSL. This is the only shader-level extension point today, and it
is not yet a documented public feature.
