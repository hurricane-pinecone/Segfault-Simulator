# Render modules

A `RenderSystem` composes its features as **render modules** rather than
baking them in. Sprites, meshes, particles, decals, voxel terrain, grass and
UI are all modules, and they work in every [style](../styles/index.md). This
is how a game turns the built-in features on and how it adds its own.

## The model

A module is an `IRenderModule<RenderContext>` (usually a
`CommandModule<RenderContext, Command>`) that emits the frame's render
commands for one feature. `RenderSystem` is a
`RenderModuleHost<RenderContext>`: it constructs, owns and drives the modules
it's given.

```cpp
auto& render = addSystem<sfs::RenderSystem>(renderer());
render.withModule<sfs::MeshModule>();
render.withModule<sfs::SpriteModule>(assetStore());
auto& particles = render.withModule<sfs::ParticleModule>(assetStore());
```

`withModule<T>(args...)` constructs the module with those arguments and
returns it for configuring. `module<T>()` finds a registered one (null if
there's none), `hasModule<T>()` asks, and `withModules<A, B, ...>()` registers
several default-constructed ones at once.

## Registration is the enable

A feature is on **iff** its module type is registered. Adding the module turns
it on; removing it (`removeModule<T>()`) turns it off and drops its state.
There is no separate "enabled" flag to track. A module type registers once;
registering it again logs an error and returns the existing one.

## Modules are independent

A module depends only on:

1. **The render context**: the frame's `RenderContext`, with the camera and
   its frustum, the sun's shadow volume, the lighting and point lights, the
   mesh library, and the game's `Space2D` (null in a 3D world).
2. **What it was given**: the registry and asset store (`ModuleInit`, at
   registration), the host (`setHost`), and its constructor's arguments.

A module never references a sibling. Where two work together, the game
connects them: `particles.enableStains(decals)` hands the particle module the
decal module as its `IDecalSink`. So features compose in any combination, and a
game can add its own module without touching the others.

## Built-in modules

All in `engine/runtime/rendering/modules/`. `StandardScene` (the scene a scene
file describes) registers mesh, grass, sprite, decal, particle and UI modules.

| Module | Draws | Construct with |
| --- | --- | --- |
| `MeshModule` | `MeshRendererComponent` entities, instanced per mesh and culled for the lit and shadow passes (see [Meshes, materials & lighting](../meshes-and-lighting/index.md)) | nothing |
| `SpriteModule` | `SpriteComponent` entities (see [Sprites & sprite sheets](../sprites/index.md)) | the asset store, optionally a `Facing` |
| `ParticleModule` | Particle effects, simulated in the game's own space (see [Particles in your game](../../particles/index.md)) | the asset store |
| `DecalModule` | Marks left where particles land: splashes, scorches, paint | the asset store |
| `VoxelModule` | A `VoxelWorld`'s block faces and water (see [Voxel world](../../voxel/index.md)) | the asset store, the world, its block registry |
| `GrassModule` | `GrassComponent` grass on mesh entities (see [Grass](../meshes-and-lighting/index.md#grass)) | the renderer |
| `UiModule` | The scene's `ui::Layer`s, over the finished frame (see [Game UI](../../ui/index.md)) | the scene |

### DecalModule

Marks lie flat on the game's plane where they landed, placed by its `Space2D`
(in a 3D world, on the ground, a hair above what they landed on), each a
tinted image drawn see-through. A mark can fade away, run down (a
drip), and be cut to the surface it landed on. Particles that stain stamp them
(`ParticleModule::enableStains`), and anything can call `addDecal()`.

| Call | Does |
| --- | --- |
| `setLayer(layer)` | The `RenderLayerComponent` layer marks lie on (1 by default: in front of layer 0) |
| `setMaxDecals(count)` | The most marks kept; the oldest go first (20000 by default) |
| `setCoverage(cellSize, perCell)` | At most `perCell` lasting marks in each `cellSize` square, so a spot hit again and again doesn't pile up marks |
| `count()` | How many marks there are |

### VoxelModule

Draws a `VoxelWorld`: each chunk's visible block faces, cut from its block
types' sprites, meshed when a chunk streams in or changes and kept until then.
In a 2D game they're placed by its `Space2D`; in 3D the grid is the world's, a
block a unit, its levels up +Z. Water draws as see-through
surfaces at each column's level (`setWaterColor` sets its colour and
opacity).

### UiModule

Lays out and draws every `ui::Layer` in the scene, and takes the scene's input
first, so clicks on UI don't reach the game. `setOverlay(layer)` adds one more
layer drawn over the rest; the engine's dev console shows this way.

## Writing your own module

Extend `CommandModule<RenderContext, Command>`, where `Command` is one of the
commands in `commands.h` (`MeshDrawCommand`, `GrassDrawCommand` or
`UiDrawCommand`), and build the frame's commands in `computeCommands()`:

```cpp
class MarkerModule : public sfs::CommandModule<sfs::RenderContext, sfs::MeshDrawCommand>
{
public:
  void init(const sfs::ModuleInit& init) override { m_registry = init.registry; }

  void computeCommands(const sfs::RenderContext& context) override
  {
    flush(); // forget last frame's
    if (!context.meshes)
      return;
    sfs::MeshDrawCommand command;
    command.mesh = m_marker; // a mesh added to context.meshes
    // ... one MeshInstance per thing to draw, placed through
    // context.space2D when the game has one
    m_commands.push_back(std::move(command));
  }

private:
  sfs::Registry* m_registry = nullptr;
  sfs::MeshHandle m_marker;
};

render.withModule<MarkerModule>();
```

Or implement `IRenderModule<RenderContext>` directly and append to the
frame's `AnyRenderCommand`s in `emit()`. Either way the render system sorts
your commands with everyone else's by `RenderPass` and submits them; a module
never talks to the renderer. Override `update(dt)` for per-frame simulation
and `settings(context)` to show knobs in a debug panel.
