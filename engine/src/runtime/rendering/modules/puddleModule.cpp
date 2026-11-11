#include "engine/runtime/rendering/modules/puddleModule.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/core/rendering/deformation.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/terrain.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/meshLibrary.h"

#include <algorithm>
#include <chrono>

namespace sfs
{

PuddleModule::PuddleModule(IRenderer& renderer) : m_renderer(renderer) {}

PuddleModule::~PuddleModule()
{
  // Let any ground being laid finish before its fields go (the pool waits
  // for it).
  m_worker.reset();
}

uint64_t PuddleModule::keyOf(const Entity& entity)
{
  // Ids are reused once an entity's gone: the generation tells them apart.
  return (uint64_t(entity.getGeneration()) << 32) | entity.getId();
}

bool PuddleModule::samePuddles(const TerrainMaterial& a,
                               const TerrainMaterial& b)
{
  if (a.puddles != b.puddles || a.puddleDepth != b.puddleDepth ||
      a.microRelief != b.microRelief ||
      a.microReliefSize != b.microReliefSize || a.splat != b.splat ||
      a.layers.size() != b.layers.size())
    return false;
  for (std::size_t i = 0; i < a.layers.size(); ++i)
    if (a.layers[i].puddles != b.layers[i].puddles)
      return false;
  return true;
}

void PuddleModule::emit(const RenderContext& context,
                        std::vector<AnyRenderCommand>& /*out*/)
{
  if (!m_registry || !context.meshes)
    return;
  m_meshes = context.meshes;
  if (context.camera)
  {
    m_eye = context.camera->position();
    m_view = context.camera->viewDirection();
  }

  for (auto& [key, field] : m_fields)
    field.seen = false;
  for (const Entity& entity :
       m_registry->view<TransformComponent, MeshRendererComponent>())
  {
    const auto& renderer = entity.getComponent<MeshRendererComponent>();
    if (renderer.materials.empty())
      continue;
    const MaterialHandle material = m_meshes->material(renderer.materials[0]);
    const MeshMaterial* drawn = m_meshes->materialData(material);
    if (!drawn || !drawn->terrain || !drawn->terrain->puddles)
      continue;
    const uint64_t key = keyOf(entity);
    Field& field = m_fields[key];
    field.seen = true;
    // A fill rate set before its puddles were first found goes with them.
    if (const auto rate = m_fillRates.find(key);
        rate != m_fillRates.end() && field.sent.empty())
      field.sent.fillRate = rate->second.rate;
    // Drawn with another material now (a style's variant): its puddles go
    // over to it.
    if (field.material != material)
    {
      if (field.material.valid())
        m_renderer.setTerrainPuddles(field.material, {});
      field.material = material;
      if (!field.sent.empty())
        m_renderer.setTerrainPuddles(material, field.sent);
    }
    if (!field.terrain || !samePuddles(*field.terrain, *drawn->terrain))
      field.stale = true;
    field.terrain = drawn->terrain;
    refresh(entity, field);
  }

  std::erase_if(m_fillRates,
                [&](const auto& entry)
                { return !m_registry->isAlive(entry.second.ground); });

  // Terrains gone, or without puddles now.
  for (auto it = m_fields.begin(); it != m_fields.end();)
    if (!it->second.seen)
    {
      if (it->second.material.valid())
        m_renderer.setTerrainPuddles(it->second.material, {});
      if (it->second.next.valid())
        it->second.next.wait();
      it = m_fields.erase(it);
    }
    else
      ++it;
}

void PuddleModule::refresh(const Entity& entity, Field& field)
{
  // Its ground laid in the background, ready: its puddles found with it.
  if (field.next.valid() &&
      field.next.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    use(field, field.next.get());

  const MeshHandle mesh = entity.getComponent<MeshRendererComponent>().mesh;
  const Aabb* bounds = m_meshes->bounds(mesh);
  if (m_meshes->isMissing(mesh) || !bounds || bounds->empty())
    return; // not loaded (yet)
  const glm::mat4 model = hierarchy::worldMatrix(entity);
  if (field.mesh != mesh.id || field.model != model || !field.surface)
  {
    field.surface =
        std::make_shared<const GroundSurface>(m_meshes->positions(mesh),
                                              m_meshes->indices(mesh),
                                              m_meshes->uvs(mesh),
                                              model);
    field.mesh = mesh.id;
    field.model = model;
    field.stale = true;
  }
  if (field.surface->empty() || field.next.valid())
    return;

  // Its window: ahead of the camera, moved once the camera's gone a good way.
  const glm::vec3 eye = m_eye ? *m_eye : glm::vec3(model[3]);
  const std::optional<GroundSurface::Point> under =
      field.surface->at(glm::vec2(eye));
  const glm::vec2 center =
      puddleWindowCenter(eye, m_view, under ? under->height : model[3].z);
  if (!field.stale && !field.sent.empty() &&
      !puddleWindowMoves(field.origin, center))
    return;
  field.stale = false;
  field.origin = puddleWindowOrigin(center);

  // Its terrain as it is now, the splat map copied (the editor paints it in
  // place as it's read).
  auto terrain = std::make_shared<TerrainMaterial>(*field.terrain);
  terrain->tint.reset();
  terrain->paint.reset(); // (changed in place by the game as it's read)
  if (terrain->splat)
    terrain->splat = std::make_shared<const TextureData>(*terrain->splat);
  if (!m_worker)
    m_worker = std::make_unique<WorkerPool>(1);
  using Ground = std::shared_ptr<const std::vector<glm::vec4>>;
  auto job = std::make_shared<std::packaged_task<Ground()>>(
      [surface = field.surface,
       terrain = std::shared_ptr<const TerrainMaterial>(std::move(terrain)),
       origin = field.origin]
      {
        return std::make_shared<const std::vector<glm::vec4>>(
            puddleGround(*surface, *terrain, origin));
      });
  field.next = job->get_future();
  m_worker->submit([job] { (*job)(); });
}

void PuddleModule::use(Field& field,
                       std::shared_ptr<const std::vector<glm::vec4>> ground)
{
  const TerrainMaterial& terrain = *field.terrain;
  field.sent.origin = field.origin;
  field.sent.texel = kPuddleTexel;
  field.sent.size = kPuddleTexels;
  field.sent.ground = std::move(ground);
  field.sent.groundTexel = kPuddleGroundTexel;
  field.sent.groundSize = kPuddleGroundTexels;
  field.sent.depth = std::clamp(terrain.puddleDepth, 0.0f, kPuddleDepthRange);
  field.sent.microRelief = std::max(terrain.microRelief, 0.0f);
  // Dips no smaller than a few texels, so they're more than one.
  field.sent.microReliefSize =
      std::max(terrain.microReliefSize, 4.0f * kPuddleTexel);
  field.sent.radius = kPuddleRadius;
  send(field);
}

void PuddleModule::setFillRate(const Entity& ground,
                               std::shared_ptr<const TextureData> fillRate)
{
  const uint64_t key = keyOf(ground);
  m_fillRates[key] = {ground, fillRate};
  if (const auto it = m_fields.find(key); it != m_fields.end() &&
                                          !it->second.sent.empty() &&
                                          it->second.sent.fillRate != fillRate)
  {
    it->second.sent.fillRate = std::move(fillRate);
    send(it->second);
  }
}

void PuddleModule::send(Field& field)
{
  ++field.sent.version;
  if (field.material.valid())
    m_renderer.setTerrainPuddles(field.material, field.sent);
}

void PuddleModule::reshaped(const Entity& ground)
{
  if (const auto it = m_fields.find(keyOf(ground));
      it != m_fields.end() && !it->second.sent.empty())
    send(it->second);
}

const TerrainPuddles* PuddleModule::puddles(const Entity& ground) const
{
  const auto it = m_fields.find(keyOf(ground));
  return it != m_fields.end() && !it->second.sent.empty() ? &it->second.sent
                                                          : nullptr;
}

void PuddleModule::finishBakes()
{
  for (auto& [key, field] : m_fields)
    if (field.next.valid())
      use(field, field.next.get());
}

} // namespace sfs
