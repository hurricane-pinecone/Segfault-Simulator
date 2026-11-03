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
#include "engine/core/jobs/workerPool.h"
#include "engine/core/rendering/terrain.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
            ? m_meshes->posedBounds(
                  entity.getComponent<MeshRendererComponent>().mesh,
                  entity.getId())
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
  {
    SFS_CPU_SCOPE("grass/benders");
    gatherBenders(seconds);
  }
  for (auto& [key, patch] : m_patches)
  {
    if (!patch.gpu.valid())
      continue;
    bool changed = false;
    {
      SFS_CPU_SCOPE("grass/trample");
      changed = patch.trample.update(seconds, m_benders, patch.recovery);
    }
    if (!changed && patch.trample.size() == patch.uploadedTrample)
      continue;
    SFS_CPU_SCOPE("grass/trample upload");
    uploadTrample(patch);
  }
}

void GrassModule::uploadTrample(Patch& patch)
{
  if (!patch.gpu.valid())
    return;
  // All of it for a new map; else just what changed.
  if (patch.trample.size() != patch.uploadedTrample)
    patch.trample.markAllChanged();
  const auto [from, to] = patch.trample.takeChanged();
  if (to.x <= from.x || to.y <= from.y)
    return;
  std::vector<uint8_t> pixels;
  patch.trample.pixels(from, to, pixels);
  m_renderer.updateGrassTrample(patch.gpu,
                                GrassTrampleImage{patch.trample.min(),
                                                  patch.trample.max(),
                                                  patch.trample.size(),
                                                  pixels,
                                                  from,
                                                  to});
  patch.uploadedTrample = patch.trample.size();
}

float GrassModule::windowReach(float drawDistance)
{
  return 1.2f * std::max(drawDistance, 1.0f) + windowSlack(drawDistance);
}

float GrassModule::windowSlack(float drawDistance)
{
  return std::max(0.15f * drawDistance, 8.0f);
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

  // A mesh much wider than its grass reaches either side of the camera grows
  // it in a window around the camera.
  const Aabb* bounds = m_meshes->bounds(mesh);
  const float reach = windowReach(grass.drawDistance) / sidewaysScale(model);
  const bool windowed = bounds && !bounds->empty() &&
                        std::max(bounds->max.x - bounds->min.x,
                                 bounds->max.y - bounds->min.y) > 2.5f * reach;

  // The ground again only when the mesh, or how it's turned or scaled,
  // changes (moving it doesn't).
  const glm::mat3 turn(model);
  const bool rebuild =
      patch.mesh != mesh.id || patch.turn != turn || patch.windowed != windowed;
  if (rebuild)
  {
    m_renderer.destroyGrass(patch.gpu);
    patch.gpu = {};
    patch.windowed = windowed;
    patch.nextWindow = {};
    patch.windowHalf = 0.0f;
    patch.source.reset();
    if (windowed)
    {
      // Its triangles kept, for its windows (the first is built below).
      auto source = std::make_shared<Source>();
      const auto positions = m_meshes->positions(mesh);
      const auto indices = m_meshes->indices(mesh);
      const auto uvs = m_meshes->uvs(mesh);
      source->positions.assign(positions.begin(), positions.end());
      source->indices.assign(indices.begin(), indices.end());
      source->uvs.assign(uvs.begin(), uvs.end());
      source->min = glm::vec2(bounds->min);
      source->max = glm::vec2(bounds->max);
      patch.source = std::move(source);
      patch.ground = GrassGround();
    }
    else
    {
      patch.ground = GrassGround::build(m_meshes->positions(mesh),
                                        m_meshes->indices(mesh),
                                        model,
                                        m_meshes->uvs(mesh));
      patch.gpu = m_renderer.createGrass(patch.ground);
      patch.uploadedTrample = glm::ivec2(0);
    }
    patch.coverGrid = {};
    patch.mesh = mesh.id;
    patch.turn = turn;
    patch.sendMaps = true;
  }
  if (patch.windowed)
  {
    // Its trample map comes with its window.
    if (patch.model != model || patch.trampleHeight != tallest(grass))
      patch.windowStale = true;
    patch.model = model;
  }
  else if (rebuild || patch.model != model ||
           patch.trampleHeight != tallest(grass))
  {
    patch.model = model;
    patch.trampleHeight = tallest(grass);
    patch.trample = GrassTrample(patch.ground, model, patch.trampleHeight);
  }

  // Painted again (in the editor, or by setting `paint`): its maps start over
  // from it (a window's: from the next one).
  if (patch.paint != grass.paint || (rebuild && patch.windowed))
  {
    patch.paint = grass.paint;
    if (patch.windowed)
    {
      GrassMaps painted = GrassMaps::fromJson(grass.paint);
      patch.painted =
          painted.empty()
              ? nullptr
              : std::make_shared<const GrassMaps>(std::move(painted));
      patch.windowStale = true;
    }
    else
    {
      patch.maps = GrassMaps::fromJson(grass.paint);
      patch.sendMaps = true;
      patch.coverMadeMaps = false;
    }
  }

  // A terrain under it (or none any more): how much it lets grow, again.
  std::shared_ptr<const TerrainMaterial> terrain = terrainUnder(entity);
  // (By what decides the grass, not which object: a terrain standing in for
  // a game's painted material is made again whenever it's painted.)
  const bool sameCover =
      terrain == patch.terrain ||
      (terrain && patch.terrain && terrain->splat == patch.terrain->splat &&
       terrain->layers == patch.terrain->layers);
  if (sameCover)
    patch.terrain = std::move(terrain);
  else if (patch.windowed)
  {
    patch.terrain = std::move(terrain);
    patch.windowStale = true;
  }
  else if (terrain && patch.terrain)
  {
    // The same terrain painted again: what changed of it.
    patch.terrain = std::move(terrain);
    patch.coverStale = true;
  }
  else
  {
    patch.terrain = std::move(terrain);
    patch.coverGrid = {};
    patch.sendMaps = true;
    // Maps made only for the terrain's sake go again with it.
    if (!patch.terrain && patch.coverMadeMaps &&
        patch.maps == GrassMaps(patch.maps.grid()))
      patch.maps = GrassMaps();
    patch.coverMadeMaps = false;
  }

  if (patch.windowed)
    windowAround(patch, grass);
  else if (patch.terrain && patch.maps.empty() && !patch.ground.empty())
  {
    patch.maps = GrassMaps(patch.ground.grid());
    patch.coverMadeMaps = true;
  }
  if (patch.terrain && !patch.maps.empty() &&
      (patch.coverGrid != patch.maps.grid() || patch.coverStale))
  {
    if (patch.coverGrid == patch.maps.grid())
      coverRepainted(patch);
    else
    {
      workOutCover(patch);
      patch.sendMaps = true;
    }
  }
  patch.coverStale = false;
  uploadMaps(patch);
  return patch.gpu.valid() ? &patch : nullptr;
}

void GrassModule::windowAround(Patch& patch, const GrassComponent& grass)
{
  // One built in the background, ready: in place of the last.
  if (patch.nextWindow.valid() &&
      patch.nextWindow.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    useWindow(patch, patch.nextWindow.get());

  const float scale = sidewaysScale(patch.model);
  const float half = windowReach(grass.drawDistance) / scale;
  const float slack = windowSlack(grass.drawDistance) / scale;
  // Around the camera (the mesh's middle until there is one).
  const glm::vec2 camera =
      m_camera
          ? glm::vec2(glm::inverse(patch.model) * glm::vec4(*m_camera, 1.0f))
          : (patch.source->min + patch.source->max) * 0.5f;
  const glm::vec2 off = glm::abs(camera - patch.windowCenter);
  const bool first = patch.windowHalf <= 0.0f;
  const bool moved = first || std::max(off.x, off.y) > slack ||
                     std::abs(half - patch.windowHalf) > 0.05f * half ||
                     patch.windowStale;
  if (!moved || patch.nextWindow.valid())
    return;
  patch.windowStale = false;
  const float trampleHeight = tallest(grass);

  // The first at once (there's nothing to show till then); after it, in the
  // background, the last one showing meanwhile.
  if (first)
  {
    useWindow(patch,
              buildWindow(*patch.source,
                          patch.model,
                          camera,
                          half,
                          trampleHeight,
                          patch.painted.get(),
                          patch.terrain.get()));
    return;
  }
  if (!m_worker)
    m_worker = std::make_unique<WorkerPool>(1);
  auto job = std::make_shared<std::packaged_task<Window()>>(
      [source = patch.source,
       model = patch.model,
       camera,
       half,
       trampleHeight,
       painted = patch.painted,
       terrain = patch.terrain]
      {
        return buildWindow(*source,
                           model,
                           camera,
                           half,
                           trampleHeight,
                           painted.get(),
                           terrain.get());
      });
  patch.nextWindow = job->get_future();
  m_worker->submit([job] { (*job)(); });
}

GrassModule::Window GrassModule::buildWindow(const Source& source,
                                             const glm::mat4& model,
                                             glm::vec2 center,
                                             float half,
                                             float trampleHeight,
                                             const GrassMaps* painted,
                                             const TerrainMaterial* terrain)
{
  Window window;
  window.center = center;
  window.half = half;
  window.model = model;
  window.trampleHeight = trampleHeight;
  const GrassGrid grid = grassWindow(
      source.min, source.max, center, half, kGrassTexel / sidewaysScale(model));
  if (grid.empty())
    return window;
  window.ground = GrassGround::build(
      source.positions, source.indices, model, source.uvs, &grid);
  if (window.ground.empty())
    return window;
  window.trample = GrassTrample(window.ground, model, trampleHeight);
  window.maps = GrassMaps(grid);
  // Its paint, as it is over this part of the mesh.
  if (painted && !painted->empty())
    for (int c = 0; c < GrassMaps::kChannels; ++c)
    {
      const auto channel = static_cast<GrassMaps::Channel>(c);
      for (int y = 0; y < grid.size.y; ++y)
        for (int x = 0; x < grid.size.x; ++x)
          window.maps.set(
              channel, x, y, painted->sample(channel, grid.center(x, y)));
    }
  if (terrain)
    window.cover = coverOf(window.ground, grid, *terrain);
  return window;
}

void GrassModule::useWindow(Patch& patch, Window window)
{
  patch.windowCenter = window.center;
  patch.windowHalf = window.half;
  // Off the mesh altogether: no grass, till the camera's back over it.
  if (window.ground.empty())
  {
    m_renderer.destroyGrass(patch.gpu);
    patch.gpu = {};
    patch.ground = GrassGround();
    return;
  }
  if (patch.gpu.valid())
    m_renderer.updateGrassGround(patch.gpu, window.ground);
  else
    patch.gpu = m_renderer.createGrass(window.ground);
  patch.ground = std::move(window.ground);
  patch.trample = std::move(window.trample);
  patch.trampleHeight = window.trampleHeight;
  patch.maps = std::move(window.maps);
  patch.sendMaps = true;
  patch.cover = std::move(window.cover);
  patch.coverGrid = patch.cover.empty() ? GrassGrid() : patch.maps.grid();
  patch.coverMadeMaps = true;
  patch.trample.markAllChanged();
  uploadTrample(patch);
}

void GrassModule::finishWindows()
{
  for (auto& [key, patch] : m_patches)
    if (patch.nextWindow.valid())
    {
      patch.nextWindow.wait();
      useWindow(patch, patch.nextWindow.get());
    }
}

bool GrassModule::windowed(const Entity& entity) const
{
  const Patch* patch = find(entity);
  return patch && patch->windowed;
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
  auto [from, to] = patch.maps.takeChanged();
  std::vector<glm::ivec4> regrow = patch.maps.takeGrownChanged();
  // And where the terrain under it changed.
  if (patch.coverTo.x > patch.coverFrom.x &&
      patch.coverTo.y > patch.coverFrom.y)
  {
    const bool none = to.x <= from.x || to.y <= from.y;
    from = none ? patch.coverFrom : glm::min(from, patch.coverFrom);
    to = none ? patch.coverTo : glm::max(to, patch.coverTo);
    regrow.insert(
        regrow.end(), patch.coverRegrow.begin(), patch.coverRegrow.end());
  }
  patch.coverFrom = patch.coverTo = glm::ivec2(0);
  patch.coverRegrow.clear();
  if (!patch.sendMaps && (to.x <= from.x || to.y <= from.y))
    return;
  // The band where grass gives out at a fall in coverage moves with it
  // (grassEdgeBand()): sent kGrassEdgeBand texels round what changed, worked
  // out from as far again past that.
  const glm::ivec2 size = patch.maps.grid().size;
  const glm::ivec2 band(kGrassEdgeBand);
  from = glm::max(from - band, glm::ivec2(0));
  to = glm::min(to + band, size);
  const glm::ivec2 readFrom = glm::max(from - band, glm::ivec2(0));
  const glm::ivec2 readTo = glm::min(to + band, size);
  std::vector<uint8_t> shape, data;
  patch.maps.pixels(readFrom, readTo, shape, data);
  // Less where the terrain under it keeps grass off.
  const int readWidth = readTo.x - readFrom.x;
  if (patch.terrain && patch.coverGrid == patch.maps.grid())
    for (int y = readFrom.y; y < readTo.y; ++y)
      for (int x = readFrom.x; x < readTo.x; ++x)
      {
        uint8_t& coverage =
            shape[(std::size_t(y - readFrom.y) * readWidth + (x - readFrom.x)) *
                  4];
        coverage = static_cast<uint8_t>(
            (coverage *
                 patch.cover[std::size_t(y) * patch.coverGrid.size.x + x] +
             127) /
            255);
      }
  grassEdgeBand(shape, readTo - readFrom);
  // Just the texels to send, if it read more round them.
  if (readFrom != from || readTo != to)
  {
    const int width = to.x - from.x;
    std::size_t at = 0;
    for (int y = from.y; y < to.y; ++y)
    {
      const std::size_t row =
          (std::size_t(y - readFrom.y) * readWidth + (from.x - readFrom.x)) * 4;
      std::copy_n(shape.begin() + row, width * 4, shape.begin() + at);
      std::copy_n(data.begin() + row, width * 4, data.begin() + at);
      at += std::size_t(width) * 4;
    }
    shape.resize(at);
    data.resize(at);
  }
  m_renderer.updateGrassMaps(
      patch.gpu,
      GrassMapsImage{patch.maps.grid(),
                     from,
                     to,
                     shape,
                     data,
                     std::span<const glm::ivec4>(regrow)});
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
  if (context.camera)
    m_camera = context.camera->position();

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
    const auto& meshRenderer = entity.getComponent<MeshRendererComponent>();
    m_commands.push_back(GrassDrawCommand{
        patch->gpu,
        material,
        MeshHandle{patch->mesh},
        patch->model,
        {grass.density, grass.height, grass.heightVariation, grass.width},
        grass.stiffness,
        grass.drawDistance,
        grass.castsShadows,
        grass.selfShadow,
        meshRenderer.materials.empty()
            ? MaterialHandle{}
            : m_meshes->material(meshRenderer.materials[0])});
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

std::shared_ptr<const TerrainMaterial>
GrassModule::terrainUnder(const Entity& entity)
{
  const auto& meshRenderer = entity.getComponent<MeshRendererComponent>();
  if (!m_meshes || meshRenderer.materials.empty())
    return nullptr;
  const MeshMaterial* drawn =
      m_meshes->materialData(m_meshes->material(meshRenderer.materials[0]));

  return drawn && drawn->terrain && terrainLimitsGrass(*drawn->terrain)
             ? drawn->terrain
             : nullptr;
}

void GrassModule::workOutCover(Patch& patch)
{
  patch.coverGrid = patch.maps.grid();
  patch.cover = coverOf(patch.ground, patch.coverGrid, *patch.terrain);
}

void GrassModule::coverRepainted(Patch& patch)
{
  std::vector<uint8_t> cover =
      coverOf(patch.ground, patch.coverGrid, *patch.terrain);
  // The blocks of texels (as GrassMaps::takeGrownChanged()'s) where it
  // changed, run together along their rows, and all of them as one.
  constexpr int kBlock = GrassMaps::kGrownBlock;
  const glm::ivec2 size = patch.coverGrid.size;
  const glm::ivec2 blocks = (size + kBlock - 1) / kBlock;
  for (int by = 0; by < blocks.y; ++by)
  {
    int run = -1; // the first block of a run of changed ones
    for (int bx = 0; bx <= blocks.x; ++bx)
    {
      bool changed = false;
      for (int y = by * kBlock;
           bx < blocks.x && !changed && y < std::min((by + 1) * kBlock, size.y);
           ++y)
        for (int x = bx * kBlock; x < std::min((bx + 1) * kBlock, size.x); ++x)
        {
          const std::size_t i = std::size_t(y) * size.x + x;
          if (i < patch.cover.size() && cover[i] != patch.cover[i])
          {
            changed = true;
            break;
          }
        }
      if (changed && run < 0)
        run = bx;
      if (changed || run < 0)
        continue;
      const glm::ivec4 rect(run * kBlock,
                            by * kBlock,
                            std::min(bx * kBlock, size.x),
                            std::min((by + 1) * kBlock, size.y));
      run = -1;
      const bool none = patch.coverTo.x <= patch.coverFrom.x ||
                        patch.coverTo.y <= patch.coverFrom.y;
      patch.coverFrom = none ? glm::ivec2(rect.x, rect.y)
                             : glm::min(patch.coverFrom, glm::ivec2(rect));
      patch.coverTo = none
                          ? glm::ivec2(rect.z, rect.w)
                          : glm::max(patch.coverTo, glm::ivec2(rect.z, rect.w));
      patch.coverRegrow.push_back(rect);
    }
  }
  patch.cover = std::move(cover);
}

std::vector<uint8_t> GrassModule::coverOf(const GrassGround& ground,
                                          const GrassGrid& grid,
                                          const TerrainMaterial& terrain)
{
  // Each of the maps' texels at the ground's texel under it, by the
  // terrain's layers there.
  const GrassGrid& groundGrid = ground.grid();
  std::vector<uint8_t> cover(grid.count(), 255);
  if (!ground.hasUvs() || groundGrid.empty())
    return cover;
  for (int y = 0; y < grid.size.y; ++y)
    for (int x = 0; x < grid.size.x; ++x)
    {
      const glm::ivec2 at = glm::clamp(
          glm::ivec2(glm::floor(groundGrid.toTexels(grid.center(x, y)))),
          glm::ivec2(0),
          groundGrid.size - 1);
      // Where there's no ground (a hole in the mesh) nothing grows anyway:
      // left full, so the grass round a hole doesn't give out toward it as
      // at an edge (see grassShape() in meshGrassGrow.wgsl).
      if (!ground.has(at.x, at.y))
        continue;
      const float grows = terrainGrass(terrain, ground.uv(at.x, at.y));
      cover[std::size_t(y) * grid.size.x + x] = static_cast<uint8_t>(
          std::lround(std::clamp(grows, 0.0f, 1.0f) * 255.0f));
    }
  return cover;
}

void GrassModule::terrainPainted(const Entity& entity)
{
  const auto it = m_patches.find(keyOf(entity));
  if (it == m_patches.end() || !it->second.terrain)
    return;
  it->second.coverStale = true; // worked out again next frame
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
