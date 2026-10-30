#include "engine/runtime/rendering/modules/grassModule.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/grassBenderComponent.h"
#include "engine/core/components/grassComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/rigidBody3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <string>

namespace sfs
{

namespace
{

// The largest of a transform's scales (what a sphere's radius scales by).
float largestScale(const glm::mat4& model)
{
  return std::max({glm::length(glm::vec3(model[0])),
                   glm::length(glm::vec3(model[1])),
                   glm::length(glm::vec3(model[2]))});
}

// How far a world unit across the ground is in its mesh's own XY.
float sidewaysScale(const glm::mat4& model)
{
  return std::max(0.5f * (glm::length(glm::vec3(model[0])) +
                          glm::length(glm::vec3(model[1]))),
                  1e-6f);
}

// The tallest a field's blades grow (its trampling reaches that high).
float tallest(const GrassComponent& grass)
{
  return grass.height * (1.0f + grass.heightVariation * 0.5f);
}

} // namespace

GrassModule::GrassModule(IRenderer& renderer) : m_renderer(renderer)
{
  // A plain grass green, for grass without a material of its own.
  MeshMaterial green;
  green.baseColor = {0.26f, 0.48f, 0.14f, 1.0f};
  green.roughness = 0.85f;
  m_defaultMaterial = m_renderer.createMaterial(green);
}

GrassModule::~GrassModule()
{
  for (auto& [key, patch] : m_patches)
    m_renderer.destroyGrass(patch.gpu);
  m_renderer.destroyMaterial(m_defaultMaterial);
}

uint64_t GrassModule::keyOf(const Entity& entity)
{
  // Ids are reused once an entity's gone: the generation tells them apart.
  return (uint64_t(entity.getGeneration()) << 32) | entity.getId();
}

void GrassModule::gatherBenders(float seconds)
{
  m_benders.clear();
  if (!m_registry)
    return;
  std::unordered_map<uint64_t, glm::vec3> positions;
  // `part`: which of an entity's shapes (it may have several), so each keeps
  // its own last position.
  const auto add =
      [&](const Entity& entity, int part, glm::vec3 center, float radius)
  {
    const uint64_t key = keyOf(entity) ^ (uint64_t(part) << 56);
    glm::vec3 velocity(0.0f);
    if (const auto last = m_lastPositions.find(key);
        last != m_lastPositions.end() && seconds > 0.0f)
      velocity = (center - last->second) / seconds;
    positions[key] = center;
    m_benders.push_back({center, radius, velocity});
  };

  // Rigid bodies that bend grass, as their colliders: a sphere as itself; a
  // box or capsule as a sphere as wide as its footprint, its bottom where
  // theirs is.
  for (const Entity& entity :
       m_registry->view<TransformComponent, RigidBody3DComponent>())
  {
    if (!entity.getComponent<RigidBody3DComponent>().bendsGrass)
      continue;
    const glm::mat4 model = hierarchy::worldMatrix(entity);
    const glm::vec3 scale(glm::length(glm::vec3(model[0])),
                          glm::length(glm::vec3(model[1])),
                          glm::length(glm::vec3(model[2])));
    if (entity.hasComponent<SphereCollider3DComponent>())
    {
      const auto& sphere = entity.getComponent<SphereCollider3DComponent>();
      if (!sphere.trigger)
        add(entity,
            0,
            glm::vec3(model * glm::vec4(sphere.offset, 1.0f)),
            sphere.radius * largestScale(model));
    }
    if (entity.hasComponent<BoxCollider3DComponent>())
    {
      const auto& box = entity.getComponent<BoxCollider3DComponent>();
      if (!box.trigger)
      {
        const glm::vec3 half = box.size * scale * 0.5f;
        const float radius = std::max(half.x, half.y);
        const glm::vec3 center(model * glm::vec4(box.offset, 1.0f));
        add(entity, 1, center - glm::vec3(0.0f, 0.0f, half.z - radius), radius);
      }
    }
    if (entity.hasComponent<CapsuleCollider3DComponent>())
    {
      const auto& capsule = entity.getComponent<CapsuleCollider3DComponent>();
      if (!capsule.trigger)
      {
        const float radius = capsule.radius * std::max(scale.x, scale.y);
        const float half = capsule.height * scale.z * 0.5f;
        const glm::vec3 center(model * glm::vec4(capsule.offset, 1.0f));
        add(entity,
            2,
            center - glm::vec3(0.0f, 0.0f, std::max(half - radius, 0.0f)),
            radius);
      }
    }
  }

  for (const Entity& entity :
       m_registry->view<TransformComponent, GrassBenderComponent>())
  {
    const auto& bender = entity.getComponent<GrassBenderComponent>();
    const glm::mat4 model = hierarchy::worldMatrix(entity);
    // Around its mesh, as placed: its middle, and its largest half-extent.
    const Aabb* bounds =
        bender.fitMesh && m_meshes &&
                entity.hasComponent<MeshRendererComponent>()
            ? m_meshes->bounds(
                  entity.getComponent<MeshRendererComponent>().mesh)
            : nullptr;
    if (bounds && !bounds->empty())
    {
      const glm::vec3 half = (bounds->max - bounds->min) * 0.5f *
                             glm::vec3(glm::length(glm::vec3(model[0])),
                                       glm::length(glm::vec3(model[1])),
                                       glm::length(glm::vec3(model[2])));
      const glm::vec3 middle = (bounds->min + bounds->max) * 0.5f;
      add(entity,
          3,
          glm::vec3(model * glm::vec4(middle + bender.offset, 1.0f)),
          std::max({half.x, half.y, half.z}));
    }
    else
      add(entity,
          3,
          glm::vec3(model * glm::vec4(bender.offset, 1.0f)),
          bender.radius * largestScale(model));
  }
  m_lastPositions = std::move(positions);
}

void GrassModule::update(double deltaTime)
{
  const float seconds = static_cast<float>(deltaTime);
  gatherBenders(seconds);
  for (auto& [key, patch] : m_patches)
    if (patch.gpu.valid() &&
        patch.trample.update(seconds, m_benders, patch.recovery))
    {
      const std::vector<uint8_t> pixels = patch.trample.image();
      m_renderer.updateGrassTrample(patch.gpu,
                                    GrassTrampleImage{patch.trample.min(),
                                                      patch.trample.max(),
                                                      patch.trample.size(),
                                                      pixels});
    }
}

GrassModule::Patch* GrassModule::refresh(const Entity& entity)
{
  if (!m_meshes || !entity.isValid() ||
      !entity.hasComponent<TransformComponent>() ||
      !entity.hasComponent<MeshRendererComponent>() ||
      !entity.hasComponent<GrassComponent>())
    return nullptr;
  const auto& grass = entity.getComponent<GrassComponent>();
  const MeshHandle mesh = entity.getComponent<MeshRendererComponent>().mesh;
  if (m_meshes->isMissing(mesh))
    return nullptr; // not loaded (yet): grass once it is

  Patch& patch = m_patches[keyOf(entity)];
  patch.seen = true;
  patch.recovery = grass.recovery;
  const glm::mat4 model = hierarchy::worldMatrix(entity);

  // The ground again only when the mesh, or how it's turned or scaled,
  // changes (moving it doesn't).
  const glm::mat3 turn(model);
  const bool rebuild = patch.mesh != mesh.id || patch.turn != turn;
  if (rebuild)
  {
    m_renderer.destroyGrass(patch.gpu);
    patch.ground = GrassGround::build(
        m_meshes->positions(mesh), m_meshes->indices(mesh), model);
    patch.gpu = m_renderer.createGrass(patch.ground);
    patch.mesh = mesh.id;
    patch.turn = turn;
    patch.sendMaps = true;
  }
  if (rebuild || patch.model != model || patch.trampleHeight != tallest(grass))
  {
    patch.model = model;
    patch.trampleHeight = tallest(grass);
    patch.trample = GrassTrample(patch.ground, model, patch.trampleHeight);
  }

  // Painted again (in the editor, or by setting `paint`): its maps start over
  // from it.
  if (patch.paint != grass.paint)
  {
    patch.paint = grass.paint;
    patch.maps = GrassMaps::fromJson(grass.paint);
    patch.sendMaps = true;
  }
  uploadMaps(patch);
  return patch.gpu.valid() ? &patch : nullptr;
}

void GrassModule::uploadMaps(Patch& patch)
{
  if (!patch.gpu.valid())
    return;
  // All of them for a new grid (or new maps); else just what changed.
  if (patch.maps.grid() != patch.uploadedGrid)
    patch.sendMaps = true;
  if (patch.sendMaps)
    patch.maps.markAllChanged();
  const auto [from, to] = patch.maps.takeChanged();
  if (!patch.sendMaps && (to.x <= from.x || to.y <= from.y))
    return;
  std::vector<uint8_t> shape, data;
  patch.maps.pixels(from, to, shape, data);
  m_renderer.updateGrassMaps(
      patch.gpu, GrassMapsImage{patch.maps.grid(), from, to, shape, data});
  patch.uploadedGrid = patch.maps.grid();
  patch.sendMaps = false;
}

void GrassModule::computeCommands(const RenderContext& context)
{
  flush();
  if (!m_registry || !context.meshes)
    return;
  m_meshes = context.meshes;

  for (auto& [key, patch] : m_patches)
    patch.seen = false;

  for (const Entity& entity :
       m_registry
           ->view<TransformComponent, MeshRendererComponent, GrassComponent>())
  {
    const Patch* patch = refresh(entity);
    if (!patch)
      continue;
    const auto& grass = entity.getComponent<GrassComponent>();
    MaterialHandle material = grass.material.empty()
                                  ? MaterialHandle{}
                                  : m_meshes->material(grass.material);
    if (!material.valid())
      material = m_defaultMaterial;
    m_commands.push_back(GrassDrawCommand{
        patch->gpu,
        material,
        MeshHandle{patch->mesh},
        patch->model,
        {grass.density, grass.height, grass.heightVariation, grass.width},
        grass.stiffness,
        grass.drawDistance});
  }

  // Grass whose entity (or component) is gone.
  for (auto it = m_patches.begin(); it != m_patches.end();)
    if (!it->second.seen)
    {
      m_renderer.destroyGrass(it->second.gpu);
      it = m_patches.erase(it);
    }
    else
      ++it;
}

const GrassModule::Patch* GrassModule::find(const Entity& entity) const
{
  const auto it = m_patches.find(keyOf(entity));
  return it != m_patches.end() && it->second.gpu.valid() ? &it->second
                                                         : nullptr;
}

GrassMaps* GrassModule::maps(const Entity& entity)
{
  // As of the last frame, if it was drawn then (a game may stamp many times
  // a frame); else made now.
  const auto it = m_patches.find(keyOf(entity));
  Patch* patch = it != m_patches.end() && it->second.gpu.valid() &&
                         entity.hasComponent<GrassComponent>()
                     ? &it->second
                     : refresh(entity);
  if (!patch)
    return nullptr;
  if (patch->maps.empty())
    patch->maps = GrassMaps(patch->ground.grid());
  return &patch->maps;
}

float GrassModule::sample(const Entity& entity,
                          GrassMaps::Channel channel,
                          glm::vec3 point) const
{
  const Patch* patch = find(entity);
  if (!patch)
    return GrassMaps::defaultValue(channel);
  const glm::vec3 local(glm::inverse(patch->model) * glm::vec4(point, 1.0f));
  return patch->maps.sample(channel, glm::vec2(local));
}

void GrassModule::stamp(const Entity& entity,
                        GrassMaps::Channel channel,
                        glm::vec3 center,
                        float radius,
                        float value,
                        GrassMaps::Blend blend,
                        float strength,
                        float softness)
{
  GrassMaps* painted = maps(entity);
  if (!painted)
    return;
  const glm::mat4& model = m_patches.at(keyOf(entity)).model;
  const glm::vec3 local(glm::inverse(model) * glm::vec4(center, 1.0f));
  painted->stamp(channel,
                 glm::vec2(local),
                 radius / sidewaysScale(model),
                 value,
                 blend,
                 strength,
                 softness);
}

void GrassModule::fill(const Entity& entity,
                       GrassMaps::Channel channel,
                       float value)
{
  if (GrassMaps* painted = maps(entity))
    painted->fill(channel, value);
}

const GrassGround* GrassModule::ground(const Entity& entity) const
{
  const Patch* patch = find(entity);
  return patch ? &patch->ground : nullptr;
}

float GrassModule::flattened(const Entity& entity, glm::vec2 point) const
{
  const Patch* patch = find(entity);
  return patch ? patch->trample.flattened(point) : 0.0f;
}

} // namespace sfs
