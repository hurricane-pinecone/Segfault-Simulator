#include "engine/rendering/modules/particles.h"

#include "engine/components/elevationComponent.h"
#include "engine/components/particleEmitterComponent.h"
#include "engine/components/transformComponent.h"
// Pulled in for the Entity::getComponent<T> template definitions in entity.inl.
#include "engine/ecs/registry.h" // IWYU pragma: keep
#include "engine/utils/profiling.h"

#include <algorithm>
#include <cmath>

namespace sfs
{

namespace
{

constexpr float kTwoPi = 6.28318530718f;

// Small forward bias so a particle sitting on a tile sorts just in front of
// that tile's surface (matches how sprites sit above the ground they stand on).
constexpr float kParticleBias = 0.02f;

// A step-up of at least this many elevation levels counts as a wall, so blood
// sticks to short ("half block") steps too, not just tall cliffs.
constexpr int kMinWallStep = 1;

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

inline glm::ivec2 floorTile(const glm::vec2& p)
{
  return {static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))};
}

} // namespace

void ParticleEngine::registerEffect(const std::string& name,
                                    const ParticleEffectDesc& desc)
{
  auto it = m_effectIds.find(name);

  if (it != m_effectIds.end())
  {
    m_effects[it->second] = desc;
    return;
  }

  const EffectId id = static_cast<EffectId>(m_effects.size());
  m_effects.push_back(desc);
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
                      ? std::atan2(spawn.velocity.y, spawn.velocity.x)
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
  inst.screenSpace = desc.space == SimulationSpace::Screen;
  inst.origin = worldPos;
  inst.groundLevel = elevation;
  inst.emitting = false; // one-shot: emit the pending burst, then drain
  inst.pendingBurst = desc.burstCount > 0 ? desc.burstCount : 1;
  inst.rng = (m_burstSeed += 0x9e3779b9u) | 1u;
  applySpawnParams(inst, spawn);

  m_bursts.push_back(std::move(inst));
  return true;
}

bool ParticleEngine::spawnScreenBurst(const std::string& effect,
                                      glm::vec2 screenPos,
                                      const ParticleSpawnParams& spawn)
{
  const EffectId fx = effectIdOf(effect);

  if (fx < 0)
    return false;

  const ParticleEffectDesc& desc = m_effects[fx];

  EmitterInstance inst;
  inst.effect = fx;
  inst.screenSpace = true;
  inst.origin = screenPos;
  inst.groundLevel = 0.0f;
  inst.emitting = false;
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
  const int room = desc.maxParticles - static_cast<int>(inst.particles.size());

  if (room <= 0)
    return;

  if (count > room)
    count = room; // over-budget emissions are dropped

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
      const float rad = std::sqrt(randf(inst.rng)) * desc.shapeRadius;
      offset = {std::cos(ang) * rad, std::sin(ang) * rad};
      break;
    }
    case EmissionShape::Ring:
    {
      const float ang = randf(inst.rng) * kTwoPi;
      offset = {
          std::cos(ang) * desc.shapeRadius, std::sin(ang) * desc.shapeRadius};
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
    p.vel = {std::cos(dirAngle) * speed, std::sin(dirAngle) * speed};
    p.vel += inst.baseVelocity; // inherited impact momentum
    p.velZ = sampleRange(inst.rng, desc.launchHeightSpeed) + inst.baseVelocityZ;

    p.height = sampleRange(inst.rng, desc.startHeight) + inst.spawnHeightBias;
    p.lifetime = std::max(0.0001f, sampleRange(inst.rng, desc.lifetime));
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
      desc.drag > 0.0f ? std::max(0.0f, 1.0f - desc.drag * dt) : 1.0f;

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

    p.vel += desc.gravity * dt;
    p.velZ += desc.gravityZ * dt;
    p.vel *= damp;
    p.velZ *= damp;
    p.pos += p.vel * dt;
    p.height += p.velZ * dt;
    p.rotation += p.angularVel * dt;

    // Screen particles and effects without a GroundBehavior never collide.
    if (inst.screenSpace || desc.ground == GroundBehavior::None)
    {
      ++i;
      continue;
    }

    // Terrain-aware collision: splat on the actual surface the droplet hits.
    if (m_terrainSource)
    {
      const float zAbs = inst.groundLevel + p.height;
      const glm::ivec2 oldTile = floorTile(prevPos);
      const glm::ivec2 newTile = floorTile(p.pos);

      bool collided = false;
      DecalSurface surface = DecalSurface::Ground;
      uint8_t wallSide = 0;
      float wallBottom = 0.0f;
      float wallTop = 0.0f;
      glm::vec2 hitPos = p.pos;
      float hitElev = zAbs;

      // Slammed into the vertical face of a tile that genuinely steps UP from
      // the one it came from (a step down or same level is not a wall --
      // otherwise a fast droplet skimming flat ground reads every tile crossing
      // as a wall).
      if (newTile != oldTile)
      {
        const int newTop =
            m_terrainSource->terrainHeightAt(newTile.x, newTile.y);
        const int oldTop =
            m_terrainSource->terrainHeightAt(oldTile.x, oldTile.y);

        if (newTop > oldTop && zAbs < static_cast<float>(newTop))
        {
          const int dx = newTile.x - oldTile.x;
          const int dy = newTile.y - oldTile.y;
          uint8_t side;
          if (dx > 0)
            side = 0; // hit newTile's west face
          else if (dx < 0)
            side = 2; // east
          else if (dy > 0)
            side = 1; // north
          else
            side = 3; // south

          collided = true;

          // A wall stain needs a real cliff on a camera-facing face. Only the
          // south (3) and east (2) faces point toward the camera (the hidden
          // west/north faces would draw over the block), and the step must be
          // tall enough. Everything else pools as ground at the wall's base.
          const bool visibleFace = side == 2 || side == 3;
          const bool tallEnough = (newTop - oldTop) >= kMinWallStep;

          if (visibleFace && tallEnough)
          {
            surface = DecalSurface::Wall;
            wallSide = side;
            wallBottom = static_cast<float>(oldTop);
            wallTop = static_cast<float>(newTop);
            hitPos = prevPos;
            hitElev = zAbs;
          }
          else
          {
            surface = DecalSurface::Ground;
            hitPos = prevPos;
            hitElev = static_cast<float>(oldTop);
          }
        }
      }

      // Otherwise, dropped onto the ground/water beneath it.
      if (!collided)
      {
        const int top = m_terrainSource->terrainHeightAt(newTile.x, newTile.y);
        if (zAbs <= static_cast<float>(top))
        {
          collided = true;
          hitPos = p.pos;
          hitElev = static_cast<float>(top);
          surface = m_terrainSource->isWaterAt(newTile.x, newTile.y)
                        ? DecalSurface::Water
                        : DecalSurface::Ground;
        }
      }

      if (collided)
      {
        emitDecal(inst,
                  desc,
                  p,
                  hitPos,
                  hitElev,
                  surface,
                  wallSide,
                  wallBottom,
                  wallTop,
                  newTile);

        // Walls splatter and die; ground/water honour the GroundBehavior.
        if (surface == DecalSurface::Wall || desc.ground == GroundBehavior::Die)
        {
          ps[i] = ps.back();
          ps.pop_back();
          continue;
        }

        p.grounded = true;
        p.vel = {0.0f, 0.0f};
        p.velZ = 0.0f;
        p.height = hitElev - inst.groundLevel;
        p.lifetime =
            std::min(p.lifetime, p.age + std::max(0.0f, desc.stickDuration));
        ++i;
        continue;
      }

      ++i;
      continue;
    }

    // No terrain source: fall back to the spawn-plane ground test.
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
          std::min(p.lifetime, p.age + std::max(0.0f, desc.stickDuration));
    }

    ++i;
  }
}

void ParticleEngine::emitDecal(EmitterInstance& inst,
                               const ParticleEffectDesc& desc,
                               const Particle& p,
                               glm::vec2 hitPos,
                               float hitElev,
                               DecalSurface surface,
                               uint8_t wallSide,
                               float wallBottom,
                               float wallTop,
                               glm::ivec2 tile)
{
  if (!m_decalSink || !desc.leavesDecal)
    return;

  const float n =
      p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0.0f;

  const glm::vec3 rgb = desc.decal.useParticleColor
                            ? desc.colorOverLife.sample(n)
                            : desc.decal.color;
  const glm::vec4 color{rgb, desc.decal.alpha};
  const float baseSize = sampleRange(inst.rng, desc.decal.size);

  if (surface == DecalSurface::Wall)
  {
    // Anchor drips to the actual wall-face edge of `tile` so they stay within
    // the wall's width instead of floating in the air beside it. East (2) face
    // is the plane x = tile.x+1 running along Y; south (3) is y = tile.y+1
    // running along X.
    const bool east = wallSide == 2;
    const float faceX = static_cast<float>(tile.x) + 1.0f;
    const float faceY = static_cast<float>(tile.y) + 1.0f;
    const float lo =
        east ? static_cast<float>(tile.y) : static_cast<float>(tile.x);
    const float hi = lo + 1.0f;
    const float hitAlong = std::clamp(east ? hitPos.y : hitPos.x, lo, hi);

    int drips = 1 + static_cast<int>(baseSize * 8.0f);
    drips = std::clamp(drips, 1, 3);

    for (int k = 0; k < drips; ++k)
    {
      DecalSpawn spawn;
      // Spread a little along the face, clamped inside the tile edge. Start
      // elevation is biased toward the wall TOP (1 - r^2), so a heavy hit packs
      // the top edge and the streaks run down from there.
      const float along = std::clamp(
          hitAlong + (randf(inst.rng) - 0.5f) * 0.5f, lo + 0.05f, hi - 0.05f);
      const float r = randf(inst.rng);
      const float bias = 1.0f - r * r;
      spawn.worldPos = east ? glm::vec2{faceX, along} : glm::vec2{along, faceY};
      spawn.elevation = hitElev + bias * std::max(0.0f, wallTop - hitElev);
      spawn.surface = DecalSurface::Wall;
      spawn.wallSide = wallSide;
      spawn.wallBottom = wallBottom;
      spawn.size = baseSize * (0.3f + randf(inst.rng) * 0.3f); // thin streak
      spawn.color = color;
      spawn.textureId = &desc.decal.texture;
      spawn.dripSpeed = 3.0f + randf(inst.rng) * 4.0f; // levels/sec
      // fadeRate stays 0: the drip runs down, then persists permanently.
      m_decalSink->addDecal(spawn);
    }
    return;
  }

  // Ground / water: a single mark. Ground is permanent; water fades.
  DecalSpawn spawn;
  spawn.worldPos = hitPos;
  spawn.elevation = hitElev;
  spawn.surface = surface;
  spawn.size = baseSize;
  spawn.rotation = p.rotation;
  spawn.color = color;
  spawn.textureId = &desc.decal.texture;
  spawn.fadeRate = (surface == DecalSurface::Water && m_terrainSource)
                       ? m_terrainSource->waterFadeRateAt(tile.x, tile.y)
                       : 0.0f;

  m_decalSink->addDecal(spawn);
}

void ParticleEngine::syncComponentEmitters()
{
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

    const auto& transform = entity.getComponent<TransformComponent>();

    float ground = 0.0f;
    if (entity.hasComponent<ElevationComponent>())
    {
      const int level = entity.getComponent<ElevationComponent>().level;
      if (level != EmptyElevation)
        ground = static_cast<float>(level);
    }

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

    inst.screenSpace = m_effects[fx].space == SimulationSpace::Screen;
    inst.origin = transform.position + emitter.offset;
    inst.groundLevel = ground;
    inst.spawnHeightBias = emitter.heightOffset;
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

void ParticleEngine::appendBatch(const EmitterInstance& inst,
                                 const ParticleEffectDesc& desc,
                                 const IProjection& proj,
                                 std::vector<ParticleBatchCommand>& out)
{
  ParticleBatchCommand cmd;
  cmd.textureId = &desc.texture;
  cmd.blend = desc.blend;

  const bool screen = inst.screenSpace;
  cmd.order.pass = screen ? RenderPass::UI : RenderPass::Particles;
  cmd.order.subpass = 0;
  cmd.order.depth =
      screen ? 0.0f : (inst.origin.x + inst.origin.y + inst.groundLevel * 0.5f);

  const float pixelPerTile = proj.worldUnitToPixels();

  const int frameCols = std::max(1, desc.frameCols);
  const int frameRows = std::max(1, desc.frameRows);
  const int frameCount = frameCols * frameRows;
  const float invCols = 1.0f / static_cast<float>(frameCols);
  const float invRows = 1.0f / static_cast<float>(frameRows);

  auto& quads = cmd.quad.quads;
  quads.reserve(inst.particles.size());

  for (const auto& p : inst.particles)
  {
    const float n =
        p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0.0f;

    const glm::vec3 rgb = desc.colorOverLife.sample(n);
    const float alpha = desc.alphaOverLife.sample(n);
    const float sizeUnits = p.baseSize * desc.sizeOverLife.sample(n);

    if (alpha <= 0.0f || sizeUnits <= 0.0f)
      continue;

    glm::vec2 center;
    float halfPx;
    float zKey;

    if (screen)
    {
      center = p.pos;
      halfPx = sizeUnits * 0.5f; // screen-space size is authored in pixels
      zKey = 0.0f;
    }
    else
    {
      const float elevation = inst.groundLevel + p.height;
      center = proj.worldToScreen(p.pos, elevation);
      halfPx = sizeUnits * pixelPerTile * 0.5f;
      zKey = p.pos.x + p.pos.y + elevation * 0.5f + kParticleBias;
    }

    // Billboard corners, rotated in screen space.
    const float c = std::cos(p.rotation);
    const float s = std::sin(p.rotation);
    const glm::vec2 ex{c * halfPx, s * halfPx};
    const glm::vec2 ey{-s * halfPx, c * halfPx};

    ParticleQuad q;
    q.points[0] = center - ex - ey;
    q.points[1] = center + ex - ey;
    q.points[2] = center + ex + ey;
    q.points[3] = center - ex + ey;

    // Not premultiplied: the fragment shader does texture * color and the blend
    // mode applies src.a, so colour and alpha stay independent.
    q.color = glm::vec4(rgb, alpha);

    if (frameCount > 1)
    {
      int frame = 0;
      if (desc.frameOverLife)
        frame = std::min(frameCount - 1, static_cast<int>(n * frameCount));
      else if (desc.frameFps > 0.0f)
        frame = static_cast<int>(p.age * desc.frameFps) % frameCount;

      const int fx = frame % frameCols;
      const int fy = frame / frameCols;
      const float u0 = static_cast<float>(fx) * invCols;
      const float v0 = static_cast<float>(fy) * invRows;
      const float u1 = u0 + invCols;
      const float v1 = v0 + invRows;

      q.uvs[0] = {u0, v0};
      q.uvs[1] = {u1, v0};
      q.uvs[2] = {u1, v1};
      q.uvs[3] = {u0, v1};
    }

    q.z = zKey;
    quads.push_back(q);
  }

  if (!quads.empty())
    out.push_back(std::move(cmd));
}

void ParticleEngine::buildCommands(const IProjection& projection,
                                   std::vector<ParticleBatchCommand>& out)
{
  ZoneScopedN("ParticleEngine::buildCommands");

  for (const auto& [id, inst] : m_entityEmitters)
  {
    if (!inst.particles.empty())
      appendBatch(inst, m_effects[inst.effect], projection, out);
  }

  for (const auto& inst : m_bursts)
  {
    if (!inst.particles.empty())
      appendBatch(inst, m_effects[inst.effect], projection, out);
  }
}

} // namespace sfs
