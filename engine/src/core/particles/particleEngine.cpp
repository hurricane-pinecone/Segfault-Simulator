#include "engine/core/particles/particleEngine.h"

#include "engine/core/components/particleEmitterComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/particles/decalSplatter.h"
// Pulled in for the Entity::getComponent<T> template definitions in entity.inl.
#include "engine/core/ecs/registry.h" // IWYU pragma: keep
#include "engine/core/util/profiling.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>

namespace sfs
{

namespace
{

constexpr float kTwoPi = 6.28318530718f;

inline uint32_t nextRand(uint32_t& state)
{
  // xorshift32 -- cheap, deterministic per emitter.
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

inline float randf(uint32_t& state)
{
  return static_cast<float>(nextRand(state)) * (1.0f / 4294967296.0f);
}

inline float sampleRange(uint32_t& state, const FloatRange& r)
{
  return r.min + (r.max - r.min) * randf(state);
}

} // namespace

void ParticleEngine::registerEffect(const std::string& name,
                                    const ParticleEffectDesc& desc)
{
  // Resolve each built-in shape to its engine texture, unless the effect set an
  // explicit one -- so an effect names a shape, not "white_dot"/"white_pixel".
  ParticleEffectDesc resolved = desc;
  if (resolved.texture.empty())
    resolved.texture = builtinShapeTexture(resolved.look);
  if (resolved.decal.texture.empty())
    resolved.decal.texture = builtinShapeTexture(resolved.decal.look);

  auto it = m_effectIds.find(name);

  if (it != m_effectIds.end())
  {
    m_effects[it->second] = resolved;
    return;
  }

  const EffectId id = static_cast<EffectId>(m_effects.size());
  m_effects.push_back(resolved);
  m_effectIds.emplace(name, id);
}

bool ParticleEngine::hasEffect(const std::string& name) const
{
  return m_effectIds.find(name) != m_effectIds.end();
}

const ParticleEffectDesc* ParticleEngine::effect(const std::string& name) const
{
  auto it = m_effectIds.find(name);
  if (it == m_effectIds.end())
    return nullptr;
  return &m_effects[it->second];
}

std::vector<std::string> ParticleEngine::effectNames() const
{
  std::vector<std::string> names;
  names.reserve(m_effectIds.size());
  for (const auto& [name, id] : m_effectIds)
    names.push_back(name);
  std::sort(names.begin(), names.end());
  return names;
}

ParticleEngine::EffectId
ParticleEngine::effectIdOf(const std::string& name) const
{
  auto it = m_effectIds.find(name);
  return it != m_effectIds.end() ? it->second : -1;
}

void ParticleEngine::applySpawnParams(EmitterInstance& inst,
                                      const ParticleSpawnParams& spawn)
{
  inst.baseVelocity = spawn.velocity;
  inst.baseVelocityZ = spawn.velocityZ;

  const float vlen2 =
      spawn.velocity.x * spawn.velocity.x + spawn.velocity.y * spawn.velocity.y;

  // Centre the emission spread on the impact direction (else fan around +X).
  inst.aimAngle = (spawn.aimAlongVelocity && vlen2 > 1e-8f)
                      ? glm::atan(spawn.velocity.y, spawn.velocity.x)
                      : 0.0f;
}

bool ParticleEngine::spawnBurst(const std::string& effect,
                                glm::vec2 worldPos,
                                float elevation,
                                const ParticleSpawnParams& spawn)
{
  const EffectId fx = effectIdOf(effect);

  if (fx < 0)
    return false;

  const ParticleEffectDesc& desc = m_effects[fx];

  EmitterInstance inst;
  inst.effect = fx;
  inst.origin = worldPos;
  inst.groundLevel = elevation;
  inst.emitting = false; // one-shot: emit the pending burst, then drain
  inst.pendingBurst = desc.burstCount > 0 ? desc.burstCount : 1;
  inst.rng = (m_burstSeed += 0x9e3779b9u) | 1u;
  applySpawnParams(inst, spawn);

  m_bursts.push_back(std::move(inst));
  return true;
}

int ParticleEngine::liveParticleCount() const
{
  int total = 0;
  for (const auto& [id, inst] : m_entityEmitters)
    total += static_cast<int>(inst.particles.size());
  for (const auto& inst : m_bursts)
    total += static_cast<int>(inst.particles.size());
  return total;
}

void ParticleEngine::emitParticles(EmitterInstance& inst,
                                   const ParticleEffectDesc& desc,
                                   int count)
{
  // Per-effect capacity and the global live-particle ceiling both clamp the
  // emission; over-budget particles are dropped (graceful, never grows
  // unbounded).
  int room = desc.maxParticles - static_cast<int>(inst.particles.size());
  const int globalRoom = m_maxParticles - liveParticleCount();
  room = glm::min(room, globalRoom);

  if (room <= 0)
    return;

  if (count > room)
    count = room;

  for (int i = 0; i < count; ++i)
  {
    Particle p;

    // Spawn position offset by the emission shape.
    glm::vec2 offset{0.0f, 0.0f};

    switch (desc.shape)
    {
    case EmissionShape::Point:
    case EmissionShape::Cone:
      break;
    case EmissionShape::Circle:
    {
      const float ang = randf(inst.rng) * kTwoPi;
      const float rad = glm::sqrt(randf(inst.rng)) * desc.shapeRadius;
      offset = {glm::cos(ang) * rad, glm::sin(ang) * rad};
      break;
    }
    case EmissionShape::Ring:
    {
      const float ang = randf(inst.rng) * kTwoPi;
      offset = {
          glm::cos(ang) * desc.shapeRadius, glm::sin(ang) * desc.shapeRadius};
      break;
    }
    case EmissionShape::Box:
      offset = {(randf(inst.rng) * 2.0f - 1.0f) * desc.boxExtents.x,
                (randf(inst.rng) * 2.0f - 1.0f) * desc.boxExtents.y};
      break;
    }

    p.pos = inst.origin + offset;

    // Planar launch direction: a random offset within the spread (Cone narrows
    // it), centred on the spawn's aim direction (0 = +X for undirected
    // effects).
    float dirAngle = inst.aimAngle;
    if (desc.shape == EmissionShape::Cone)
    {
      const float half = desc.coneAngleDegrees * (kTwoPi / 360.0f);
      dirAngle += (randf(inst.rng) * 2.0f - 1.0f) * half;
    }
    else
    {
      const float half = desc.directionSpread * 0.5f;
      dirAngle += (randf(inst.rng) * 2.0f - 1.0f) * half;
    }

    const float speed = sampleRange(inst.rng, desc.speed);
    p.vel = {glm::cos(dirAngle) * speed, glm::sin(dirAngle) * speed};
    p.vel += inst.baseVelocity; // inherited impact momentum
    p.velZ = sampleRange(inst.rng, desc.launchHeightSpeed) + inst.baseVelocityZ;

    p.height = sampleRange(inst.rng, desc.startHeight) + inst.spawnHeightBias;
    p.lifetime = glm::max(0.0001f, sampleRange(inst.rng, desc.lifetime));
    p.age = 0.0f;
    p.baseSize = sampleRange(inst.rng, desc.size);
    p.rotation = sampleRange(inst.rng, desc.rotation);
    p.angularVel = sampleRange(inst.rng, desc.angularVelocity);
    p.grounded = false;

    inst.particles.push_back(p);
  }
}

void ParticleEngine::stepInstance(EmitterInstance& inst,
                                  const ParticleEffectDesc& desc,
                                  float dt)
{
  if (inst.emitting)
  {
    if (desc.emitRate > 0.0f)
    {
      inst.emitAccumulator += desc.emitRate * dt;
      const int n = static_cast<int>(inst.emitAccumulator);
      if (n > 0)
      {
        inst.emitAccumulator -= static_cast<float>(n);
        inst.pendingBurst += n;
      }
    }

    if (desc.burstInterval > 0.0f && desc.burstCount > 0)
    {
      inst.burstTimer += dt;
      while (inst.burstTimer >= desc.burstInterval)
      {
        inst.burstTimer -= desc.burstInterval;
        inst.pendingBurst += desc.burstCount;
      }
    }
  }

  if (inst.pendingBurst > 0)
  {
    emitParticles(inst, desc, inst.pendingBurst);
    inst.pendingBurst = 0;
  }

  const float damp =
      desc.drag > 0.0f ? glm::max(0.0f, 1.0f - desc.drag * dt) : 1.0f;

  auto& ps = inst.particles;

  for (std::size_t i = 0; i < ps.size();)
  {
    Particle& p = ps[i];

    p.age += dt;

    if (p.age >= p.lifetime)
    {
      ps[i] = ps.back();
      ps.pop_back();
      continue;
    }

    if (p.grounded)
    {
      p.rotation += p.angularVel * dt;
      ++i;
      continue;
    }

    const glm::vec2 prevPos = p.pos;
    const float prevHeight = p.height;

    p.vel += desc.gravity * dt;
    p.velZ += desc.gravityZ * dt;
    p.vel *= damp;
    p.velZ *= damp;
    p.pos += p.vel * dt;
    p.height += p.velZ * dt;
    p.rotation += p.angularVel * dt;

    // Effects without a GroundBehavior never collide.
    if (desc.ground == GroundBehavior::None)
    {
      ++i;
      continue;
    }

    // Collide against the surface -- terrain on the iso path, colliders on the
    // flat path -- and splat where the droplet sticks.
    if (m_collisionSource)
    {
      const ParticleHit hit =
          m_collisionSource->sweep({prevPos,
                                    p.pos,
                                    inst.groundLevel + prevHeight,
                                    inst.groundLevel + p.height});
      if (hit.hit)
      {
        emitDecal(inst, desc, p, hit);

        // Walls splatter and die; ground/water honour the GroundBehavior.
        if (hit.surface == DecalSurface::Wall ||
            desc.ground == GroundBehavior::Die)
        {
          ps[i] = ps.back();
          ps.pop_back();
          continue;
        }

        p.pos = hit.pos;
        p.grounded = true;
        p.vel = {0.0f, 0.0f};
        p.velZ = 0.0f;
        p.height = hit.elevation - inst.groundLevel;
        p.lifetime =
            glm::min(p.lifetime, p.age + glm::max(0.0f, desc.stickDuration));
      }
      ++i;
      continue;
    }

    // No collision source: fall back to the spawn-plane ground test.
    if (p.height <= 0.0f)
    {
      p.height = 0.0f;

      if (desc.ground == GroundBehavior::Die)
      {
        ps[i] = ps.back();
        ps.pop_back();
        continue;
      }

      p.grounded = true;
      p.vel = {0.0f, 0.0f};
      p.velZ = 0.0f;
      p.lifetime =
          glm::min(p.lifetime, p.age + glm::max(0.0f, desc.stickDuration));
    }

    ++i;
  }
}

void ParticleEngine::emitDecal(EmitterInstance& inst,
                               const ParticleEffectDesc& desc,
                               const Particle& p,
                               const ParticleHit& hit)
{
  if (!m_decalSink || !desc.leavesDecal)
    return;

  const float n =
      p.lifetime > 0.0f ? glm::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0.0f;
  const glm::vec3 rgb = desc.decal.useParticleColor
                            ? desc.colorOverLife.sample(n)
                            : desc.decal.color;
  const glm::vec4 color{rgb, desc.decal.pool.alpha};
  const float baseSize = sampleRange(inst.rng, desc.decal.pool.size);

  // Topology comes from the effect's shaper (default = soft pool + crisp fan).
  SplatParams splat;
  splat.refSpeed = desc.decal.impactRef;
  splat.streakMaxCount = desc.decal.streaks.maxCount;
  splat.streakWidth = desc.decal.streaks.width;
  splat.streakLengthScale = desc.decal.streaks.lengthScale;
  splat.poolSoft = desc.decal.pool.soft;
  splat.streakCrisp = desc.decal.streaks.crisp;
  const ISplatterShaper& shaper =
      desc.decal.shaper ? *desc.decal.shaper : defaultSplatterShaper();

  if (hit.surface == DecalSurface::Wall)
  {
    // Anchor the splatter to the actual wall-face edge of `hit.tile` so it
    // stays within the wall's width. East (2) face is the plane x = tile.x+1
    // running along Y; south (3) is y = tile.y+1 running along X.
    const bool east = hit.wallSide == 2;
    const float faceX = static_cast<float>(hit.tile.x) + 1.0f;
    const float faceY = static_cast<float>(hit.tile.y) + 1.0f;
    const float lo =
        east ? static_cast<float>(hit.tile.y) : static_cast<float>(hit.tile.x);
    const float hi = lo + 1.0f;
    const float hitAlong = glm::clamp(east ? hit.pos.y : hit.pos.x, lo, hi);
    const glm::vec2 facePos =
        east ? glm::vec2{faceX, hitAlong} : glm::vec2{hitAlong, faceY};

    // Splash: a single hit point makes a whole burst paint a flat horizontal
    // line, since the droplets strike at nearly the same height. Scatter each
    // mark across the face instead -- spread growing with the in-plane impact
    // speed and stretched along the direction the blood was travelling, so a
    // fast hit throws an elongated splash and a slow drip stays tight.
    const float alongVel = east ? p.vel.y : p.vel.x;
    const float speed = glm::length(glm::vec2{alongVel, p.velZ});
    const float splash =
        glm::clamp(speed / glm::max(desc.decal.impactRef, 0.001f), 0.0f, 1.0f);
    const float spread = baseSize * (0.3f + 3.0f * splash);
    const glm::vec2 travel = speed > 0.001f
                                 ? glm::vec2{alongVel, p.velZ} / speed
                                 : glm::vec2{0.0f, 1.0f};
    const glm::vec2 across{-travel.y, travel.x};

    const auto stampWall = [&](glm::vec2 size,
                               float rotation,
                               bool crisp,
                               float dripSpeed,
                               glm::vec2 offset)
    {
      DecalSpawn spawn;
      glm::vec2 pos = facePos;
      const float along = glm::clamp((east ? pos.y : pos.x) + offset.x, lo, hi);
      if (east)
        pos.y = along;
      else
        pos.x = along;
      spawn.worldPos = pos;
      // AT the hit elevation (+ splash) -- a low hit marks low on the wall
      // instead of every streak packing the top and running the full height.
      spawn.elevation =
          glm::clamp(hit.elevation + offset.y, hit.wallBottom, hit.wallTop);
      spawn.surface = DecalSurface::Wall;
      spawn.wallSide = hit.wallSide;
      spawn.wallBottom = hit.wallBottom;
      spawn.wallTop = hit.wallTop;
      spawn.size = size;
      spawn.rotation = rotation;
      spawn.color = color;
      spawn.textureId = &desc.decal.texture;
      spawn.crisp = crisp;
      spawn.dripSpeed = dripSpeed;
      m_decalSink->addDecal(spawn);
    };

    // Per-mark scatter in the face plane: wide along the travel axis, tighter
    // across it.
    const auto splashOffset = [&]
    {
      const float a = (randf(inst.rng) * 2.0f - 1.0f) * spread;
      const float b = (randf(inst.rng) * 2.0f - 1.0f) * spread * 0.5f;
      return travel * a + across * b;
    };

    // Directional splatter ON the face: the face shows only the in-plane
    // velocity (along-edge + a damped vertical, so it leans the way the blood
    // travelled rather than straight down). Soft pool + crisp fan, at the hit.
    splat.fan = true;
    SplatImpact impact;
    impact.velocity = glm::vec2{alongVel, p.velZ * 0.5f};
    impact.baseSize = baseSize;
    const SplatPattern pattern = shaper.shape(impact, splat, inst.rng);
    for (int s = 0; s < pattern.count; ++s)
      stampWall(pattern.shapes[s].size,
                pattern.shapes[s].rotation,
                pattern.shapes[s].crisp,
                0.0f,
                splashOffset());

    // One drip running down FROM the hit (short for a low hit, longer high up).
    stampWall(glm::vec2{baseSize * 0.45f, baseSize * 0.45f},
              0.0f,
              desc.decal.streaks.crisp,
              2.0f + randf(inst.rng) * 3.0f,
              glm::vec2{0.0f, 0.0f});
    return;
  }

  // Ground / water (and any flat surface). A flat SIDE hit (mostly-horizontal
  // entry normal) runs down instead of fanning; an iso ground hit reads top
  // (normal 0 -> not a side). Water marks stay single.
  const bool side = glm::abs(hit.normal.x) > glm::abs(hit.normal.y);
  splat.fan = hit.surface != DecalSurface::Water && !side;

  SplatImpact impact;
  impact.velocity = p.vel;
  impact.velZ = p.velZ;
  impact.baseSize = baseSize;
  const SplatPattern pattern = shaper.shape(impact, splat, inst.rng);
  for (int s = 0; s < pattern.count; ++s)
  {
    DecalSpawn spawn;
    spawn.worldPos = hit.pos;
    spawn.elevation = hit.elevation;
    spawn.surface = hit.surface;
    spawn.size = pattern.shapes[s].size;
    spawn.rotation = pattern.shapes[s].rotation;
    spawn.color = color;
    spawn.textureId = &desc.decal.texture;
    spawn.fadeRate = hit.fadeRate;
    spawn.crisp = pattern.shapes[s].crisp;
    spawn.clipMin = hit.boundsMin; // flat clips to the collider; iso ignores
    spawn.clipMax = hit.boundsMax;
    m_decalSink->addDecal(spawn);
  }

  // Flat side hit: a streak that runs DOWN the face over time (the flat sink
  // maps dripSpeed -> a downward grow). size = (width, height); height grows.
  if (side)
  {
    DecalSpawn drip;
    drip.worldPos = hit.pos;
    drip.surface = DecalSurface::Ground;
    drip.size = glm::vec2{desc.decal.streaks.width, baseSize * 0.5f};
    drip.color = color;
    drip.textureId = &desc.decal.texture;
    drip.crisp = desc.decal.streaks.crisp;
    drip.dripSpeed = baseSize * 6.0f; // grow units/sec; flat-sink interpreted
    drip.clipMin = hit.boundsMin;
    drip.clipMax = hit.boundsMax;
    m_decalSink->addDecal(drip);
  }
}

void ParticleEngine::syncComponentEmitters()
{
  // No registry means no entity-component emitters to sync (e.g. a headless
  // burst-only host). The spawnBurst path in simulate() still runs.
  if (!registry)
    return;

  // Assume nothing emits this frame; re-enable emitters whose entity still has
  // an enabled component. Entries left disabled (entity gone or disabled) keep
  // their particles and drain.
  for (auto& [id, inst] : m_entityEmitters)
    inst.emitting = false;

  for (const auto& entity :
       registry->view<TransformComponent, ParticleEmitterComponent>())
  {
    const auto& emitter = entity.getComponent<ParticleEmitterComponent>();

    const EffectId fx = effectIdOf(emitter.effect);
    if (fx < 0)
      continue; // unknown effect name -- skip

    // Where it is in the world (a child emitter follows its parent).
    const TransformComponent transform = hierarchy::worldTransform(entity);

    const float ground = transform.position.z; // its height above the plane

    EmitterInstance& inst = m_entityEmitters[entity.getId()];

    if (inst.effect != fx)
    {
      // First sighting (or the component's effect changed): bind it.
      inst.effect = fx;
      inst.particles.clear();
      inst.emitAccumulator = 0.0f;
      inst.burstTimer = 0.0f;
      inst.pendingBurst = 0;
      inst.rng = (entity.getId() * 747796405u + 2891336453u) | 1u;
    }

    inst.origin = transform.position2D() + glm::vec2(emitter.offset);
    inst.groundLevel = ground;
    inst.spawnHeightBias = emitter.offset.z;
    inst.emitting = emitter.enabled;
  }
}

void ParticleEngine::simulate(double deltaTime)
{
  ZoneScopedN("ParticleEngine::simulate");

  const float dt = static_cast<float>(deltaTime);
  if (dt <= 0.0f)
    return;

  syncComponentEmitters();

  for (auto it = m_entityEmitters.begin(); it != m_entityEmitters.end();)
  {
    EmitterInstance& inst = it->second;
    stepInstance(inst, m_effects[inst.effect], dt);

    if (!inst.emitting && inst.particles.empty())
      it = m_entityEmitters.erase(it);
    else
      ++it;
  }

  for (std::size_t i = 0; i < m_bursts.size();)
  {
    EmitterInstance& inst = m_bursts[i];
    stepInstance(inst, m_effects[inst.effect], dt);

    if (inst.pendingBurst == 0 && inst.particles.empty())
    {
      m_bursts[i] = std::move(m_bursts.back());
      m_bursts.pop_back();
    }
    else
    {
      ++i;
    }
  }
}

void ParticleEngine::sprites(std::vector<ParticleSpriteBatch>& out) const
{
  const auto append = [&](const EmitterInstance& inst)
  {
    if (inst.particles.empty())
      return;
    const ParticleEffectDesc& desc = m_effects[inst.effect];
    ParticleSpriteBatch batch;
    batch.textureId = &desc.texture;
    batch.blend = desc.blend;
    const int cols = glm::max(1, desc.frameCols);
    const int rows = glm::max(1, desc.frameRows);
    const int frames = cols * rows;
    batch.sprites.reserve(inst.particles.size());
    for (const Particle& p : inst.particles)
    {
      const float n =
          p.lifetime > 0.0f ? glm::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0.0f;
      ParticleSprite sprite;
      sprite.color =
          glm::vec4(desc.colorOverLife.sample(n), desc.alphaOverLife.sample(n));
      sprite.size = p.baseSize * desc.sizeOverLife.sample(n);
      if (sprite.color.a <= 0.0f || sprite.size <= 0.0f)
        continue;
      sprite.position = p.pos;
      sprite.height = inst.groundLevel + p.height;
      sprite.rotation = p.rotation;
      if (frames > 1)
      {
        int frame = 0;
        if (desc.frameOverLife)
          frame = glm::min(frames - 1, static_cast<int>(n * frames));
        else if (desc.frameFps > 0.0f)
          frame = static_cast<int>(p.age * desc.frameFps) % frames;
        sprite.uvRect = glm::vec4(static_cast<float>(frame % cols) / cols,
                                  static_cast<float>(frame / cols) / rows,
                                  1.0f / cols,
                                  1.0f / rows);
      }
      batch.sprites.push_back(sprite);
    }
    if (!batch.sprites.empty())
      out.push_back(std::move(batch));
  };
  for (const auto& [id, inst] : m_entityEmitters)
    append(inst);
  for (const auto& inst : m_bursts)
    append(inst);
}

} // namespace sfs
