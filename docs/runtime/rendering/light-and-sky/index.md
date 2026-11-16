# Light, sky, wind and global shader parameters

What a scene shows of its world (its light, its sky, the wind, how wet things
look) is [scene data](../../../core/ecs/index.md#scene-data) the render system
reads live, each frame, where it lives. Each is optional: a scene adds the ones
it uses.

| Scene data | Like | What it is |
| --- | --- | --- |
| `sfs::DirectionalLighting` | a directional light in Unity, Unreal or Godot | The sun by day, the moon by night: what lights the scene from one way and casts shadows |
| `sfs::AmbientLighting` | an ambient or sky light | The light from all around, and the sky's colour |
| `sfs::CelestialBodies` | Unity HDRP's celestial bodies | Where a physical sky's sun and moon are, and how full the moon is |
| `sfs::Clouds` | | How much of the sky the clouds cover, and how heavy they are |
| `sfs::Rain` | | How hard it's raining |
| `sfs::Wind` | Unity's directional WindZone, Unreal's WindDirectionalSource | Which way and how fast the wind blows, and how gusty it is |
| `sfs::ShaderGlobals` | Unreal's Material Parameter Collection, Godot's global shader uniforms | Named values every material and shader can read, which a game sets |

A game never tells the render system any of this: one of its systems (or a
script) writes the scene data, and the next frame shows it. Each is authored in
**Scene Settings > Scene Data** like any scene data, and saved with the scene.
A new scene in the editor starts with a DirectionalLighting and an
AmbientLighting (and, in 3D, a Wind), so it's lit as it always was.

Lightning strikes are entities with a `LightningComponent` (see
[Lightning](../meshes-and-lighting/index.md#lightning)), and local gusts are
entities with a `WindZoneComponent` (see
[Wind](../../particles/index.md#wind)).

## Light

The fields are in [Lighting](../meshes-and-lighting/index.md#lighting).

- **DirectionalLighting** (`direction`, `color`, `intensity`) is one light:
  the sun by day, and by night the moon, less intense (a full moon's is about
  0.12). A game's day/night cycle writes whichever is shining, as games on
  other engines turn and dim one directional light. Without it, the scene has
  no directional light and no shadows from one.
- **AmbientLighting** (`intensity`, `skyColor`) is the base brightness
  everything receives and the sky's colour (what the background, haze and
  shiny things' reflections show under a flat sky). Without it, there's none,
  and the sky is black.
- **Clouds** (`cover`, `darkness`) dim the directional light (as overcast as
  they are) and soften shadows; a physical sky draws them.
- **CelestialBodies** (`sunDirection`, `moonDirection`, `moonPhase`) are a
  physical sky's: the sun lights its air (and glows through twilight below the
  horizon), both are drawn as discs (the moon's lit to its phase), the night
  sky glows brighter under the moon, and the sky works the directional light
  out from them itself, physically (the sun's colour through the air; once it
  has set, the moon's light, fading in as the sun sinks). Without them, a
  physical sky's sun is along the DirectionalLighting and it has no moon. A
  flat sky draws neither, and its light is the DirectionalLighting as it is.

A day/night cycle is a system that writes them from the time of day. By night
it puts the moon in the directional light's place, as bright as
`moonlight(bodies)` says (its phase, how high it is, how far the sun has
sunk; the renderer dims it behind clouds itself):

```cpp
struct [[sfs::scene_data]] DayClock
{
  [[sfs::range(0, 1)]] float timeOfDay = 0.3f; // 0 midnight, 0.5 noon
  float dayLength = 600.0f;                    // seconds
};

class DayNightSystem : public sfs::System
{
protected:
  void create() override { setTimeStep(TimeStep::Frame); }

  void update(double deltaTime) override
  {
    DayClock* clock = registry->sceneData<DayClock>();
    if (!clock)
      return;
    clock->timeOfDay = std::fmod(
        clock->timeOfDay + float(deltaTime) / clock->dayLength, 1.0f);
    const float angle = (clock->timeOfDay - 0.25f) * glm::two_pi<float>();
    const float height = std::sin(angle);
    const float day = glm::smoothstep(-0.1f, 0.2f, height);

    sfs::CelestialBodies bodies;
    bodies.sunDirection = {std::cos(angle), -0.4f, height};
    bodies.moonDirection = -bodies.sunDirection; // full, opposite the sun
    if (auto* sky = registry->sceneData<sfs::CelestialBodies>())
      *sky = bodies;
    if (auto* ambient = registry->sceneData<sfs::AmbientLighting>())
    {
      ambient->intensity = glm::mix(0.08f, 0.5f, day);
      ambient->skyColor = glm::mix(
          glm::vec3(0.02f, 0.03f, 0.08f), glm::vec3(0.5f, 0.7f, 0.9f), day);
    }
    if (auto* light = registry->sceneData<sfs::DirectionalLighting>())
    {
      const bool sunUp = height > 0.0f;
      light->direction = sunUp ? bodies.sunDirection : bodies.moonDirection;
      light->color = sunUp ? glm::vec3(1.0f, 0.96f, 0.88f)
                           : glm::vec3(0.62f, 0.72f, 1.0f);
      light->intensity =
          sunUp ? 0.85f * day : 0.12f * sfs::moonlight(bodies);
    }
  }
};
```

The system that writes each is its one writer; everything else that wants to
know how light it is (a lamp that switches on at dusk) reads it with
`registry->sceneData<const sfs::CelestialBodies>()` and the like.

## Wind

It holds the wind's physical properties only:

| Field | Default | Meaning |
| --- | --- | --- |
| `direction` | 30 | Which way it blows, degrees from +X toward +Y |
| `speed` | 4 | How fast the air moves, m/s along `direction` |
| `gustiness` | 0.5 | How much it comes and goes in gusts, rolling across the land the way it blows (0: steady) |

How hard things sway follows from the speed (`windSway()`): `kSwayAtBreeze`
(0.35) in a breeze of `kBreezeSpeed` (4 m/s), in proportion to the speed,
from `kLeastSway` (0.4) of that (a light stir, even in calm air) to
`kMostSway` (2.5) of it (a gale). A scene without a Wind has still air: no
sway at all. What sways, and how stiffly, is the material's: a
[foliage material](../meshes-and-lighting/index.md#foliage-materials) (by its
vertex colours), [grass](../meshes-and-lighting/index.md#grass), and the
ground's sheen. Particles whose effect has a `windInfluence` drift on
`windVelocity(wind)` (the speed along the direction), plus the
`WindZoneComponent`s their emitter is in. Clouds drift on it and rain slants
in it.

## Global shader parameters

`ShaderGlobals::parameters` is a list of named values (a `glm::vec4` each; a
number is its x). Set one with `setShaderGlobal(globals, "name", value)`
(which adds it if it isn't there) and read one with `shaderGlobal(globals,
"name")`. A material reads one with a **Global Parameter** node (see
[Material graphs](../../../core/rendering/material-graphs/index.md)), by
name; a parameter the scene hasn't got reads 0.

The engine's own looks read two (each its x, 0..1):

| Name | What it does |
| --- | --- |
| `wetness` | Wets every lit surface the rain reaches: darker and glossier, water standing on flat ground (see [Wet surfaces](../meshes-and-lighting/index.md#wet-surfaces)) |
| `puddles` | Fills a terrain's low spots with water (see [Puddles](../meshes-and-lighting/index.md#puddles)) |

They're a game's to decide, as it wants its ground to look: they usually lag
the rain, as things wet and dry.

Every frame carries 16 parameter slots. A name gets one when a material that
reads it is first built (`wetness` and `puddles` always have the first two),
the same for the whole run; a scene's parameter no material reads takes none
(and costs nothing). Past 16 names read by materials, one more reads 0 and is
logged once.

## A storm

A storm is a system writing the clouds, the rain, the wind and how wet things
look, and setting off the odd lightning strike.

```cpp
struct [[sfs::scene_data]] Storm
{
  [[sfs::range(0, 1)]] float strength = 0.0f; // 0 fair .. 1 a downpour
  [[sfs::hide]] float wet = 0.0f;             // how wet the ground is
  [[sfs::hide]] uint32_t strikes = 0;         // strikes so far (their seeds)
};

class StormSystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<sfs::LightningComponent>();
    setTimeStep(TimeStep::Frame);
  }

  void update(double deltaTime) override
  {
    Storm* storm = registry->sceneData<Storm>();
    sfs::Clouds* clouds = registry->sceneData<sfs::Clouds>();
    sfs::Rain* rain = registry->sceneData<sfs::Rain>();
    sfs::Wind* wind = registry->sceneData<sfs::Wind>();
    sfs::ShaderGlobals* globals = registry->sceneData<sfs::ShaderGlobals>();
    if (!storm || !clouds || !rain || !wind || !globals)
      return;
    const float s = storm->strength;
    const float dt = float(deltaTime);

    clouds->cover = glm::mix(0.2f, 1.0f, s);
    clouds->darkness = s;
    rain->intensity = glm::smoothstep(0.4f, 1.0f, s);
    wind->speed = glm::mix(3.0f, 14.0f, s); // things sway harder with it
    wind->gustiness = glm::mix(0.4f, 0.8f, s);

    // Things wet quickly in the rain and dry slowly after it.
    storm->wet = rain->intensity > 0.0f
                     ? std::min(storm->wet + rain->intensity * dt / 60.0f, 1.0f)
                     : std::max(storm->wet - dt / 600.0f, 0.0f);
    sfs::setShaderGlobal(*globals, "wetness", storm->wet);
    sfs::setShaderGlobal(*globals, "puddles", storm->wet * storm->wet);

    // Now and then a strike at the storm's height; clear away finished ones.
    for (auto [bolt, strike] : getEntities<sfs::LightningComponent>())
      if (strike.finished)
        registry->destroyEntity(bolt);
    if (s > 0.9f && random01() < dt * 0.2f)
    {
      sfs::Entity bolt = registry->createEntity();
      bolt.addComponent<sfs::TransformComponent>(
          sfs::TransformComponent{randomPointAround(800.0f)});
      sfs::LightningComponent strike;
      strike.seed = ++storm->strikes;
      bolt.addComponent<sfs::LightningComponent>(strike);
    }
  }
};
```

(`random01()` and `randomPointAround()` stand for the game's own random
numbers.)

## Older scene files

Scenes saved before these were scene data kept them in their render settings
(`"render"`: `"lighting"`, `"wind"`, `"weather"`). Loading one converts them,
each only if the scene hasn't got that scene data already, and logs it once:

- the lighting's sun becomes the DirectionalLighting, its ambient level and
  sky colour the AmbientLighting, and a moon above the horizon, with the sun,
  the CelestialBodies (a flat sky's moonlight is the game's to put in the
  directional light now: the old moon colour and strength aren't kept);
- its cloud cover and the weather's cloud darkness become the Clouds, the
  weather's rain the Rain;
- the wind becomes the Wind, its sway kept: the speed that sways things as
  hard, by `windSway()`'s curve backwards (without one, the weather wind's
  speed);
- the weather's wetness and puddles become the `wetness` and `puddles`
  parameters.

Saving writes them as scene data. A render block that isn't an object is
logged and skipped, the others converted as ever.

A `WindZoneComponent` saved before zones were local blew everywhere, as the
scene's `Wind` does now: loading one logs a note (it now blows within its
`radius` of its entity, and nowhere without a `TransformComponent`). Move its
wind into the Wind.
