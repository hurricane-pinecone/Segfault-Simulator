# Game lifecycle

A SFS game has three nested layers, each a class you subclass and whose hooks the
engine calls for you:

```
Game     your application — window, the main loop, which scenes exist
  └─ Scene     a level or screen — owns the entities, systems, and game objects
       └─ System / GameObject     your gameplay and rendering logic
```

You never write the loop yourself; you override hooks and the engine drives them.

![Ownership: the Game owns its scenes; each Scene owns its entities, systems, and game objects](../../images/high-level-ownership.png)

## Game — the application

Subclass `Game` and override the hooks you need. The engine runs `init()` →
`setup()` → `run()` (the loop) → teardown; your overrides hang off those:

| Hook | When | Typical use |
| --- | --- | --- |
| `onSetup()` | once, at startup | create your scenes; stand up Lua |
| `onInit()` | once, after init | app-wide setup |
| `onProcessInput(input)` | each frame | app-level input |
| `onUpdate(dt)` | each frame | app-level update |
| `onRender()` | each frame | app-level draw (HUD, overlays) |
| `onDebugUI()` | each frame (debug builds) | an ImGui debug panel |
| `onDestroy()` | at shutdown | teardown (the one required override) |
| `makeRenderBackend()` | once | return the backend for the graphics API; defaults to OpenGL |
| `createQuadRenderer(w, h)` | once (OpenGL path) | pick the 2D renderer within the OpenGL backend (flat or isometric) |

```cpp
class MyGame : public sfs::Game
{
protected:
  void onSetup() override
  {
    // createScene<T>() builds the scene, makes it current, and runs its onInit.
    sceneManager.createScene<MyScene>("Game");
  }

  void onDestroy() override { /* teardown */ }
};
```

The default `makeRenderBackend()` returns an OpenGL backend. Within that path,
`createQuadRenderer()` selects the 2D renderer: flat-2D by default, or an
isometric renderer for heightfield games. A game using WebGPU overrides
`makeRenderBackend()` to return a `WebGpuRenderBackend` instead, and
`createQuadRenderer()` is not called in that path.

`renderBackend()` returns a non-owning pointer to the active backend; cast it to
the concrete type when a scene needs the device directly (for example, to
construct a `VoxelGpuSystem`).

## Scene — a level or screen

A `Scene` owns one world: its entities, systems, and game objects. Subclass it and
build the level in `onInit()`:

| Hook | When |
| --- | --- |
| `onInit()` | once, when the scene is created |
| `onEnter()` / `onExit()` | when it becomes / stops being the current scene |
| `onProcessInput(input)` | each frame |
| `onUpdate(dt)` | each frame |
| `onRender()` | each frame |
| `onDebugUI()` | each frame (debug builds) |

```cpp
class MyScene : public sfs::Scene
{
  using sfs::Scene::Scene; // inherit the (id, services[, name]) constructors

protected:
  void onInit() override
  {
    addSystem<MovementSystem>();                       // register systems
    addSystem<sfs::FlatRenderSystem>(assetStore(), quadRenderer());

    sfs::Entity e = createEntity();                    // spawn entities
    e.addComponent<sfs::TransformComponent>();

    createObject<Player>();                             // or a GameObject actor
  }

  void onUpdate(double dt) override { /* scene-wide per-frame logic */ }
};
```

Inside a scene you have `createEntity()`, `createObject<T>()`, `addSystem<T>()` /
`getSystem<T>()` / `hasSystem<T>()`, plus the engine services `assetStore()`,
`quadRenderer()`, and `textRenderer()`.

Manage scenes through the scene manager: `sceneManager.createScene<T>(name)` builds
one (and makes it current), `sceneManager.load(name)` switches to an existing one,
and `sceneManager.current()` returns the active scene.

## The frame loop

Each tick the engine runs three phases and fans each out through the current scene
to its systems and game objects:

```
processInput → update(dt) → render
```

- **processInput** — `Game::onProcessInput`, then the scene's, then each
  GameObject's `onProcessInput`.
- **update** — `Game::onUpdate`, the scene's `onUpdate`, every enabled `System`'s
  `update(dt)`, and each GameObject's `onUpdate`.
- **render** — every `System`'s `render()` (this is where the render systems draw),
  then `onRender`.

## Putting it together

```cpp
// main.cpp
int main()
{
  MyGame game;
  if (!game.init(1280, 720))   // window, GL context, engine services
    return 1;
  game.setup();                // runs your onSetup -> create scenes
  game.run();                  // the main loop until quit; tears down on exit
  return 0;
}
```

That's the whole shape: `Game` owns the loop and your scenes; a `Scene` owns the
entities, systems, and objects; systems and game objects carry the behaviour.
