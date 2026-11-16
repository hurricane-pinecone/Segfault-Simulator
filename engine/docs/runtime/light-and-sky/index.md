# Light, sky, wind, global shader parameters and lightning: internals

How the render system and the particles read the scene's presentation state.
What it's for and how a game uses it:
[Light, sky, wind and global shader parameters](../../../../docs/runtime/rendering/light-and-sky/index.md).

## The types

- `DirectionalLighting`, `AmbientLighting`, `CelestialBodies`, `Clouds`,
  `Rain`, `Wind`, `ShaderGlobals` (`engine/core/sceneData/`): scene data
  (`[[sfs::scene_data]]`), reflected through engine-core's reflected header
  list (`ENGINE_SCENE_DATA_DIR` in `engine/CMakeLists.txt`), each optional.
  Each is plain data on the registry's scene data slot: one heap object, read
  where it lives (`RenderSystem::sceneDataOf<T>()`, null when the scene hasn't
  got it). The frame's `SceneLighting` is built from them each frame by
  `sceneLighting()`: the directional light as the "sun" (none: below the
  horizon, strength 0), the ambient light (none: 0, a black sky), the
  clouds' cover, and under a physical sky the celestial bodies' sun and moon
  (none: the sun along the directional light, no moon). The moon is not its
  own light: a game writes it into the DirectionalLighting by night
  (`moonlight(bodies)` gives its clear-sky share; the renderer applies the
  clouds' `clearSky()` once). A physical sky keeps drawing the moon's disc
  and glow and works its light out itself from the CelestialBodies, as before
  (its moon substitution in `buildContext()` is unchanged; under a flat sky
  there's no moon in the frame's lighting, so it never substitutes).
- `ShaderParameter` lists are their own reflected field kind
  (`FieldKind::ShaderParameters`, `std::vector<sfs::ShaderParameter>`):
  codegen's parser and generator know the spelling, the serializer saves
  `[{"name", "value"}]`, and the Inspector edits it with list rows
  (`shaderParameterList()`, `inspectorPanel.cpp`).
- `LightningComponent` (`engine/core/components/`): a strike's settings and,
  hidden, the render system's state for it (age, duration, top, width, its
  channel's two `MeshHandle`s, `shaped`, `finished`). `LightningStrike`
  (`lightning.h`) stays the lightning functions' input; `strikeOf()` makes one
  from the component and its entity's world position.
- `WindZoneComponent`: a local zone (`radius` across the plane round its
  entity's position).

## Where each is read

- `RenderSystem::update()`: `windVelocity(sceneWind())` drifts the clouds and
  the rain (while animating); `updateLightning(seconds)` shapes new strikes
  (meshes facing the camera as last drawn: a strike's over before it can
  move far), ages
  them unless held, and finishes the dark ones and, past
  `kMostLightningStrikes`, the oldest (freeing their meshes). A 2D scene
  finishes strikes at once, and so does one without a TransformComponent
  (logged). `observe<LightningComponent>` frees a strike's meshes when its
  entity is destroyed before it finished. Known limit: when only the
  component is removed, the observer runs after it's gone (a system can't
  read a component its entity lost), so its meshes stay in the MeshLibrary
  until the RenderSystem goes; the docs tell games to destroy the entity.
- `buildContext()`: `sceneLighting()` of the light scene data (the celestial
  bodies only under a physical sky) into `m_worldLighting` (then the 2D sun,
  the style's sky tint, cloud dimming and, under a physical sky, the moon);
  `updateSky()` takes the Clouds' darkness;
  `placeLightning()` the brightest strike.
- `render()`: `FrameParams::wind` (the Wind, or still air: speed 0) and
  `sway` (`windSway()`; 0 without a Wind), `rainfall` (the Rain's, 0..1),
  `globals`
  (`gatherShaderGlobals()`, lookups only; the engine's two clamped 0..1 by
  `unitInterval()`, `engine/core/util/clamp.h`); `emitLightning()`
  the channels.
- `FramePlan`: `FrameUniforms::rainClouds` (x rain, w cloud cover),
  `windDrift` (xy the wind's velocity, zw the clouds' drift), `globals`
  (16 vec4, at the end of the uniform); rain's counts, veil and drips take
  `rainfall` and the `wetness` slot; `rainMapOn()` the rain and both slots.
- WGSL (`meshCommon.wgsl`): `frame.rainClouds`, `frame.windDrift`,
  `frame.globals[GLOBAL_WETNESS]` (`surfaceWetness()`, meshLit) and
  `frame.globals[GLOBAL_PUDDLES]` (`terrainPuddles()`, meshTerrainSurface).
- `ParticleEngine::gatherWind()`: the Wind's velocity once per update, the
  enabled zones into `m_windZones` (a reused scratch list: no allocation once
  it has room); `windAt()` per emitter instance, only for effects with a
  `windInfluence`.
- The Global Parameter graph node (`materialNodes.cpp`, `Property::Global`,
  `Node::global`) compiles to `frame.globals[slot]` (the slot from
  `shaderGlobalSlot()` at compile time), `vec4f(0.0)` without one; on the CPU
  it reads 0.

## The slot table

`shaderGlobalSlot()` (`shaderGlobals.cpp`) is process-wide: a fixed array of
16 names, `wetness` and `puddles` first, then names in the order a graph that
reads them (a Global Parameter node) is first compiled. Only the compiler adds
names, under a mutex, published by an atomic count; `findShaderGlobalSlot()`
scans the published names without a lock and adds nothing, so the per-frame
gather never takes a slot (a scene's parameter no shader reads isn't
gathered), and a half-typed name in the editor never reaches either: the
Shader Globals and Global Parameter name fields commit on Enter or focus loss
(`widgets::inputTextOnCommit()`). Past 16, a name gets -1 (logged once) and
reads 0.

## Per-frame cost

Three scene data index lookups; the cached `LightningComponent` queries (empty
outside storms); the scene's parameters' slot lookups (each a lock-free scan
of at most 16 short names: a few compares for a few parameters); the frame uniform 256
bytes longer. Nothing allocates per frame: `FrameParams::globals` is a
`std::array`, the zones' list keeps its room.

## Scene files and the editor

`renderToJson()` writes none of them; `convertRenderSceneData()` (`sceneSerializer.cpp`) turns an older file's into
the scene data the scene hasn't got, after its `"sceneData"` has loaded, and
logs it once. In the editor, Scene Data edits them (Scene Settings has no
widgets of its own for them); `EditorScene::
resetToDefault()` adds a DirectionalLighting and an AmbientLighting (and in
3D a Wind) to a new scene; a prefab's tab has its own AmbientLighting (the
grey backdrop), which its undo
snapshots keep (`EditorScene::clear(keepSceneData)` from
`SceneDocument::restore()` for prefab content).

## Tests

`renderDataTests` (core: Wind, `sceneLighting()`, the parameters and their
slots), `particleEngineTests` (the scene's wind and local zones),
`renderSystemTests` (the light, sky, rain, wind and parameters reaching the
frame, each absent,
lightning entities), `sceneSerializerTests` (round trips, old files),
`materialGraphTests` (the node), `webGpuRendererTests` (the layout and every
node's shader, the Global Parameter's included, on the GPU; llvmpipe in WSL).
