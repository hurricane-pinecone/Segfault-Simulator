
#include "sampleGame.h"
#include "config.h"
#include "effects/particleEffects.h"
#include "gameObjects/player.h"
#include "scenes/gameScene.h"
#include "systems/TerrainGeneratorSystem.h"
#include <cmath>
#include <engine/components/transformComponent.h>
#include <engine/components/worldCollider.h>
#include <engine/input/keyboardInput.h>
#include <engine/rendering/isometricGeometryRenderer.h>
#include <engine/rendering/modules/particles.h>
#include <engine/rendering/util/isometric/camera.h>
#include <engine/sceneManager/scene.h>
#include <engine/scripting/luaScripting.h>
#include <engine/systems/cameraSystem.h>
#include <engine/systems/isometric/isometricRenderSystem.h>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <lua.hpp>
#include <memory>
#include <string>

namespace
{
using IsometricParticles = sfs::Particles<sfs::IsometricRenderContext>;

IsometricParticles* particlesOf(sfs::Scene* scene)
{
  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return nullptr;
  return scene->getSystem<sfs::IsometricRenderSystem>()
      .module<IsometricParticles>();
}

float terrainElevationAt(sfs::Scene* scene, glm::vec2 pos)
{
  if (!scene || !scene->hasSystem<TerrainGeneratorSystem>())
    return 0.0f;
  return static_cast<float>(
      scene->getSystem<TerrainGeneratorSystem>().terrainHeightAt(
          static_cast<int>(std::floor(pos.x)),
          static_cast<int>(std::floor(pos.y))));
}

// Spray a gore blast at a world position via the active scene's particle
// module.
void spawnGoreAt(sfs::Scene& scene, glm::vec2 pos)
{
  IsometricParticles* particles = particlesOf(&scene);
  if (!particles)
    return;
  spawnGore(*particles,
            pos,
            terrainElevationAt(&scene, pos),
            glm::vec2{1.0f, 0.0f},
            12.0f);
}

// --- Lua `particles` table -------------------------------------------------
// Raw Lua C functions (they read tables/colours straight off the stack); the
// SampleGame* rides as the closure upvalue so they resolve the active scene at
// call time.

bool optNumberField(lua_State* L, int table, const char* key, double& out)
{
  lua_getfield(L, table, key);
  const bool ok = lua_isnumber(L, -1) != 0;
  if (ok)
    out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return ok;
}

// Read an sfs.colors-style table ({r,g,b,a} 0-255, or {255,0,0}) into a 0..1
// vec3.
glm::vec3 readColor(lua_State* L, int idx)
{
  const auto channel = [&](const char* key, int arrayIndex) -> float
  {
    lua_getfield(L, idx, key);
    if (!lua_isnumber(L, -1))
    {
      lua_pop(L, 1);
      lua_rawgeti(L, idx, arrayIndex);
    }
    const float v = static_cast<float>(luaL_optnumber(L, -1, 0.0));
    lua_pop(L, 1);
    return v / 255.0f;
  };
  return glm::vec3{channel("r", 1), channel("g", 2), channel("b", 3)};
}

int particlesSpawn(lua_State* L)
{
  auto* game = static_cast<SampleGame*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* name = luaL_checkstring(L, 1);
  const glm::vec2 pos{static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                      static_cast<float>(luaL_optnumber(L, 3, 0.0))};

  sfs::Scene* scene = game->currentScene();
  if (IsometricParticles* particles = particlesOf(scene))
    particles->spawnBurst(name, pos, terrainElevationAt(scene, pos));
  return 0;
}

// particles.configure(name, { color = sfs.colors.Green, burst = 30, sizeMin =,
//   sizeMax =, speedMin =, speedMax =, lifetimeMin =, lifetimeMax =, gravityZ
//   =, drag =, spread = }) -- tweak a registered effect and re-apply it live.
int particlesConfigure(lua_State* L)
{
  auto* game = static_cast<SampleGame*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  IsometricParticles* particles = particlesOf(game->currentScene());
  if (!particles)
    return 0;

  const sfs::ParticleEffectDesc* current = particles->effect(name);
  if (!current)
    return luaL_error(L, "unknown particle effect '%s'", name);

  sfs::ParticleEffectDesc desc = *current;

  lua_getfield(L, 2, "color");
  if (lua_istable(L, -1))
  {
    const glm::vec3 c = readColor(L, lua_gettop(L));
    desc.colorOverLife = sfs::Gradient::twoStop(c, c * 0.3f);
  }
  lua_pop(L, 1);

  double v = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  if (optNumberField(L, 2, "burst", v))
    desc.burstCount = static_cast<int>(v);
  if (optNumberField(L, 2, "spread", v))
    desc.directionSpread = static_cast<float>(v);
  if (optNumberField(L, 2, "gravityZ", v))
    desc.gravityZ = static_cast<float>(v);
  if (optNumberField(L, 2, "drag", v))
    desc.drag = static_cast<float>(v);
  if (optNumberField(L, 2, "sizeMin", lo) &&
      optNumberField(L, 2, "sizeMax", hi))
    desc.size =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));
  if (optNumberField(L, 2, "speedMin", lo) &&
      optNumberField(L, 2, "speedMax", hi))
    desc.speed =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));
  if (optNumberField(L, 2, "lifetimeMin", lo) &&
      optNumberField(L, 2, "lifetimeMax", hi))
    desc.lifetime =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));

  particles->registerEffect(name, desc);
  return 0;
}
} // namespace

void SampleGame::onSetup()
{
  m_isoConfig = sfs::IsometricProjectionConfig{
      32,
      16,
      8,
      WORLD_SCALE,
      glm::vec2{WINDOW_WIDTH / 2.0f, WINDOW_HEIGHT / 2.0f},
  };

  // WorldCollider offset/size are authored in pixels; one world tile is
  // tileWidth px (render-scale independent -- worldScale magnifies the box on
  // screen anyway).
  sfs::WorldCollider::pixelsPerUnit = static_cast<float>(m_isoConfig.tileWidth);

  // TODO: Create actual title screen and refactor
  // sfs::Scene* titleScene = sceneManager.createScene("Title Scene");
  auto gameScene = sceneManager.createScene<GameScene>("Game");

  setupLua();

  isRunning = true;
}

void SampleGame::setupLua()
{
  m_lua = std::make_unique<sfs::LuaScripting>();
  m_lua->init();
  sfs::setActiveLua(m_lua.get()); // route the web editor's eval entry here

  // Particle controls bound into the live VM. They resolve the active scene's
  // particle module at call time, so editing + running a chunk pokes the
  // running game (the same surface the web JS editor drives).
  m_lua->bind("splat",
              [this]
              {
                sfs::Scene* scene = sceneManager.current();
                if (!scene)
                  return;
                if (Player* player = scene->tryFindObject<Player>())
                  spawnGoreAt(*scene,
                              player->entity()
                                  .getComponent<sfs::TransformComponent>()
                                  .position);
              });

  m_lua->bind(
      "spawnGore",
      [this](double x, double y)
      {
        if (sfs::Scene* scene = sceneManager.current())
          spawnGoreAt(
              *scene, glm::vec2{static_cast<float>(x), static_cast<float>(y)});
      });

  // The `particles` table needs to read Lua tables (colours, config opts),
  // which the std::function binds can't, so push it with the raw Lua C API.
  // particles.spawn(name, x, y) / particles.configure(name, opts).
  if (lua_State* L = m_lua->state())
  {
    lua_newtable(L);

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, &particlesSpawn, 1);
    lua_setfield(L, -2, "spawn");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, &particlesConfigure, 1);
    lua_setfield(L, -2, "configure");

    lua_setglobal(L, "particles");
  }
}

void SampleGame::onUpdate(double deltaTime)
{
  sfs::Scene* scene = sceneManager.current();

  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return;

  const sfs::ActiveCamera camera =
      scene->hasSystem<sfs::CameraSystem>()
          ? scene->getSystem<sfs::CameraSystem>().activeCamera()
          : sfs::ActiveCamera{};

  m_isoProjection = sfs::makeProjection(m_isoConfig, camera);

  scene->getSystem<sfs::IsometricRenderSystem>().setProjection(
      &m_isoProjection);
}

void SampleGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifdef ENGINE_WEB
    return;
#endif
    isRunning = false;
  }

  if (sceneManager.current()->name() == "Title Scene" &&
      input.mouse().mousePressed(sfs::MouseButton::Left))
    sceneManager.load("Game");
}

void SampleGame::onDestroy()
{
  sfs::setActiveLua(nullptr); // m_lua is destroyed after this
}

std::unique_ptr<sfs::IQuadRenderer>
SampleGame::createQuadRenderer(int windowWidth, int windowHeight)
{
  return std::make_unique<sfs::IsometricGeometryRenderer>(
      windowWidth, windowHeight);
}
