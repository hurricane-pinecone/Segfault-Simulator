#include "engine/runtime/rendering/modules/deformationModule.h"

#include "engine/core/components/deformerComponent.h"
#include "engine/core/components/grassComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/terrain.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/iRenderer.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/grassModule.h"
#include "engine/runtime/rendering/modules/puddleModule.h"
#include "engine/runtime/rendering/modules/renderModuleHost.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sfs
{

namespace
{

constexpr int kTile = DeformationMap::kTile;
// The patch moves in steps of its coarsest ring's spacing, so every ring's
// vertices stay on their own grid over the world (nothing swims).
constexpr float kPatchStep = DeformationModule::kPatchSpacing *
                             (1 << (DeformationModule::kPatchRings - 1));
// How far above the ground a deformer's underside is looked at (metres):
// enough for a ridge and the lugs above the tyre's lowest point.
constexpr float kAboveGround = 0.12f;

bool sameDeformation(const TerrainDeformation& a, const TerrainDeformation& b)
{
  return a.patch == b.patch && a.texel == b.texel && a.size == b.size &&
         a.from == b.from && a.ground == b.ground &&
         a.groundOrigin == b.groundOrigin && a.groundTexel == b.groundTexel &&
         a.groundSize == b.groundSize && a.patchCenter == b.patchCenter &&
         a.patchHalf == b.patchHalf && a.patchBand == b.patchBand &&
         a.deepest == b.deepest;
}

} // namespace

DeformationModule::DeformationModule(IRenderer& renderer) : m_renderer(renderer)
{
}

DeformationModule::~DeformationModule() = default;

void DeformationModule::init(const ModuleInit& m)
{
  m_registry = m.registry;
  m_assets = m.assetStore;
}

uint64_t DeformationModule::keyOf(const Entity& entity)
{
  // Ids are reused once an entity's gone: the generation tells them apart.
  return (uint64_t(entity.getGeneration()) << 32) | entity.getId();
}

DeformationModule::Field* DeformationModule::fieldOf(const Entity& entity)
{
  const auto it = m_fields.find(keyOf(entity));
  return it != m_fields.end() ? &it->second : nullptr;
}

const DeformationModule::Field*
DeformationModule::fieldOf(const Entity& entity) const
{
  const auto it = m_fields.find(keyOf(entity));
  return it != m_fields.end() ? &it->second : nullptr;
}

void DeformationModule::update(double deltaTime)
{
  for (auto& [key, field] : m_fields)
    field.sincePuddles += deltaTime;
}

void DeformationModule::computeCommands(const RenderContext& context)
{
  flush();
  if (!m_registry || !context.meshes)
    return;
  m_meshes = context.meshes;

  // The deformable terrains, as placed.
  struct Seen
  {
    Field* field;
    MaterialHandle material;
    const MeshMaterial* drawn;
  };
  std::vector<Seen> seen;
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
    if (!drawn || !drawn->terrain || !drawn->terrain->deformable)
      continue;
    Field& field = m_fields[keyOf(entity)];
    field.entity = entity;
    field.seen = true;
    prepare(entity, field);
    seen.push_back({&field, material, drawn});
  }
  // Not deformable now (or gone): drawn plain again. A map asked for before
  // its terrain's drawn is kept while the entity lives.
  for (auto it = m_fields.begin(); it != m_fields.end();)
  {
    Field& field = it->second;
    if (!field.seen && field.material.valid())
    {
      m_renderer.setTerrainDeformation(field.material, {});
      field.material = {};
      field.windowed = false;
      field.sent = {};
    }
    if (!field.seen && !field.entity.isAlive())
      it = m_fields.erase(it);
    else
      ++it;
  }

  deform();

  PuddleModule* puddles = m_host ? m_host->module<PuddleModule>() : nullptr;
  for (const Seen& terrain : seen)
  {
    draw(context, terrain.material, *terrain.drawn, *terrain.field);
    Field& field = *terrain.field;
    if (field.reshaped && puddles && field.sincePuddles >= kPuddleInterval)
    {
      puddles->reshaped(field.entity);
      field.reshaped = false;
      field.sincePuddles = 0.0;
    }
  }
}

void DeformationModule::prepare(const Entity& entity, Field& field)
{
  const MeshHandle mesh = entity.getComponent<MeshRendererComponent>().mesh;
  const Aabb* bounds = m_meshes->bounds(mesh);
  if (m_meshes->isMissing(mesh) || !bounds || bounds->empty())
    return; // not loaded (yet)
  const glm::mat4 model = hierarchy::worldMatrix(entity);
  if (field.surface && field.mesh == mesh.id && field.model == model)
    return;
  field.surface =
      std::make_shared<const GroundSurface>(m_meshes->positions(mesh),
                                            m_meshes->indices(mesh),
                                            m_meshes->uvs(mesh),
                                            model);
  field.mesh = mesh.id;
  field.model = model;
  field.ground.reset(); // laid again under the patch
}

glm::vec2 DeformationModule::patchCenterFor(const Camera& camera, float ground)
{
  // Where the view meets the ground, at most kReach out: what's looked at,
  // in detail, and the ground between it and the camera.
  const glm::vec3 eye = camera.position();
  const glm::vec3 view = camera.viewDirection();
  const glm::vec2 flat(view);
  const float across = glm::length(flat);
  if (across < 1e-4f)
    return glm::vec2(eye);
  float out = kReach;
  if (view.z < -1e-4f)
    out = std::min(out, std::max(eye.z - ground, 0.0f) / -view.z * across);
  return glm::vec2(eye) + flat / across * out;
}

void DeformationModule::upload(Field& field, glm::ivec2 tile)
{
  m_halves.resize(std::size_t(kTile) * kTile);
  field.map.halves(tile, m_halves);
  m_renderer.writeTerrainDeformation(field.material, tile * kTile, m_halves);
}

void DeformationModule::draw(const RenderContext& context,
                             MaterialHandle material,
                             const MeshMaterial& drawn,
                             Field& field)
{
  if (field.material != material)
  {
    // Drawn with another material now (a style's variant): its deformation
    // goes over to it.
    if (field.material.valid())
      m_renderer.setTerrainDeformation(field.material, {});
    field.material = material;
    field.windowed = false;
    field.sent = {};
  }
  // What's changed since last frame (to upload, and for the puddles). Not
  // drawn, the window's sent afresh once it is.
  const std::vector<glm::ivec2> changed = field.map.takeChanged();
  if (!changed.empty())
    field.reshaped = true;
  if (!field.surface || field.surface->empty() || !context.camera)
  {
    field.windowed = false;
    return;
  }

  // Where the patch goes, and the window around it.
  const glm::vec3 eye = context.camera->position();
  const std::optional<GroundSurface::Point> under =
      field.surface->at(glm::vec2(eye));
  const glm::vec2 center =
      patchCenterFor(*context.camera, under ? under->height : 0.0f);
  const glm::vec2 patchCenter = glm::round(center / kPatchStep) * kPatchStep;
  const int windowTiles = kWindowTexels / kTile;
  const glm::ivec2 from =
      (DeformationMap::tileOf(field.map.texelOf(patchCenter)) -
       windowTiles / 2) *
      kTile;
  const glm::ivec2 firstTile = from / kTile;
  const auto inWindow = [&](glm::ivec2 tile)
  {
    const glm::ivec2 at = tile - firstTile;
    return at.x >= 0 && at.y >= 0 && at.x < windowTiles && at.y < windowTiles;
  };

  // Nothing pressed in near it: the ground's drawn as it is (and costs
  // nothing more).
  bool any = false;
  for (const glm::ivec2 tile : field.map.tiles())
    if (inWindow(tile))
    {
      any = true;
      break;
    }
  if (!any)
  {
    if (!field.sent.empty())
      m_renderer.setTerrainDeformation(material, {});
    field.sent = {};
    field.windowed = false;
    return;
  }

  // Its patch's material: a copy of the terrain's, as drawn.
  if (!field.patchMadeFrom || !(*field.patchMadeFrom == drawn))
  {
    MeshMaterial copy = drawn;
    copy.variants.clear();
    field.patch = m_meshes->setMaterial(
        "deformation:patch:" + std::to_string(keyOf(field.entity)), copy);
    field.patchMadeFrom = std::make_shared<const MeshMaterial>(drawn);
  }
  if (!field.patch.valid())
  {
    field.windowed = false;
    return;
  }

  // The ground under the patch, laid again once it's moved a good way.
  const float groundHalf = 0.5f * kGroundTexels * kGroundTexel;
  if (!field.ground ||
      glm::length(patchCenter - (field.groundOrigin + groundHalf)) >
          0.25f * groundHalf)
  {
    field.groundOrigin =
        glm::floor((patchCenter - groundHalf) / kGroundTexel) * kGroundTexel;
    field.ground =
        std::make_shared<const std::vector<glm::vec4>>(field.surface->raster(
            field.groundOrigin, kGroundTexel, glm::ivec2(kGroundTexels)));
  }
  field.patchCenter = patchCenter;

  TerrainDeformation deformation;
  deformation.patch = field.patch;
  deformation.texel = field.map.texel();
  deformation.size = kWindowTexels;
  deformation.from = from;
  deformation.ground = field.ground;
  deformation.groundOrigin = field.groundOrigin;
  deformation.groundTexel = kGroundTexel;
  deformation.groundSize = glm::ivec2(kGroundTexels);
  deformation.patchCenter = patchCenter;
  deformation.patchHalf = kPatchHalf;
  deformation.patchBand = kPatchBand;
  deformation.deepest = field.map.deepest();
  if (!sameDeformation(deformation, field.sent))
    m_renderer.setTerrainDeformation(material, deformation);
  field.sent = deformation;

  // The window's tiles: those come into it (all of them, the first time),
  // and those changed in it.
  const glm::ivec2 oldFirst = field.windowFrom / kTile;
  const auto wasIn = [&](glm::ivec2 tile)
  {
    const glm::ivec2 at = tile - oldFirst;
    return field.windowed && at.x >= 0 && at.y >= 0 && at.x < windowTiles &&
           at.y < windowTiles;
  };
  if (!field.windowed || from != field.windowFrom)
    for (int y = 0; y < windowTiles; ++y)
      for (int x = 0; x < windowTiles; ++x)
        if (!wasIn(firstTile + glm::ivec2(x, y)))
          upload(field, firstTile + glm::ivec2(x, y));
  for (const glm::ivec2 tile : changed)
    if (inWindow(tile) && wasIn(tile))
      upload(field, tile);
  field.windowed = true;
  field.windowFrom = from;

  // The patch, displaced on the GPU (its z here only for culling).
  if (!m_patchMesh.valid())
    buildPatchMesh();
  if (!m_patchMesh.valid())
    return;
  const std::optional<GroundSurface::Point> middle =
      field.surface->at(patchCenter);
  MeshInstance instance;
  instance.model = glm::translate(
      glm::mat4(1.0f),
      glm::vec3(patchCenter, middle ? middle->height : field.model[3].z));
  MeshDrawCommand command;
  command.mesh = m_patchMesh;
  command.materials = {field.patch};
  command.instances = {instance};
  if (context.shadowsEnabled)
    command.shadowInstances = {instance};
  m_commands.push_back(std::move(command));
}

void DeformationModule::deform()
{
  if (m_fields.empty() || m_registry->view<DeformerComponent>().empty())
  {
    m_placed.clear();
    return;
  }
  std::unordered_map<uint64_t, glm::mat4> placed;
  for (const Entity& mesh :
       m_registry->view<TransformComponent, MeshRendererComponent>())
  {
    // Its deformer: its own, or an ancestor's that covers its children.
    const DeformerComponent* deformer = nullptr;
    Entity at = mesh;
    for (bool own = true; at.isValid() && at.isAlive(); own = false)
    {
      if (at.hasComponent<DeformerComponent>())
      {
        const auto& found = at.getComponent<DeformerComponent>();
        if (own || found.children)
          deformer = &found;
        break;
      }
      at = hierarchy::parentOf(at);
    }
    if (!deformer || !deformer->enabled)
      continue;
    // Only when it's moved (pressing again where it stands changes nothing).
    const glm::mat4 model = hierarchy::worldMatrix(mesh);
    const uint64_t key = keyOf(mesh);
    placed[key] = model;
    const auto last = m_placed.find(key);
    if (last != m_placed.end() && last->second == model)
      continue;
    const glm::vec2 motion = last != m_placed.end()
                                 ? glm::vec2(model[3] - last->second[3])
                                 : glm::vec2(0.0f);
    pressMesh(mesh, model, at, *deformer, motion);
  }
  m_placed = std::move(placed);
}

void DeformationModule::pressMesh(const Entity& mesh,
                                  const glm::mat4& model,
                                  const Entity& owner,
                                  const DeformerComponent& deformer,
                                  glm::vec2 motion)
{
  const MeshHandle handle = mesh.getComponent<MeshRendererComponent>().mesh;
  // A skinned mesh as an animator poses it (a foot down where it steps).
  const Aabb* bounds = m_meshes->posedBounds(handle, mesh.getId());
  if (!bounds || bounds->empty())
    return;
  const Aabb world = bounds->transformed(model);
  const std::shared_ptr<const TextureData> tread =
      !deformer.tread.empty() && m_assets
          ? m_assets->imageTexture(deformer.tread)
          : nullptr;
  const glm::vec2 forward = glm::normalize(glm::vec2(model[0]) + 1e-9f);
  const glm::vec2 side(-forward.y, forward.x);
  GrassModule* grass = m_host ? m_host->module<GrassModule>() : nullptr;

  for (auto& [key, field] : m_fields)
  {
    if (!field.seen || !field.surface || field.surface->empty() ||
        field.entity == mesh)
      continue;
    const GroundSurface& surface = *field.surface;
    if (world.max.x < surface.low().x || world.max.y < surface.low().y ||
        world.min.x > surface.high().x || world.min.y > surface.high().y)
      continue;
    // Near the ground under it at all?
    const glm::vec2 middle =
        (glm::vec2(world.min) + glm::vec2(world.max)) * 0.5f;
    const std::optional<GroundSurface::Point> ground = surface.at(middle);
    if (!ground)
      continue;
    const float below = ground->height + kAboveGround;
    if (world.min.z > below)
      continue;
    const Underside bottom =
        underside(m_meshes->posedPositions(handle, mesh.getId()),
                  m_meshes->indices(handle),
                  model,
                  below,
                  field.map.texel());
    if (bottom.empty())
      continue;

    // Each texel pressed down to its sink below the underside, deeper on
    // soft soil and where the tread presses.
    Footprint footprint;
    footprint.from = bottom.from;
    footprint.size = bottom.size;
    footprint.target.assign(
        bottom.bottom.size(), std::numeric_limits<float>::quiet_NaN());
    footprint.coverage = bottom.coverage;
    for (int y = 0; y < bottom.size.y; ++y)
      for (int x = 0; x < bottom.size.x; ++x)
      {
        const std::size_t i = std::size_t(y) * bottom.size.x + x;
        if (std::isnan(bottom.bottom[i]))
          continue;
        const glm::vec2 c = field.map.centerOf(bottom.from + glm::ivec2(x, y));
        const std::optional<GroundSurface::Point> here = surface.at(c);
        if (!here)
          continue;
        const float soft =
            std::clamp(field.softnessAt ? field.softnessAt(c) : field.softness,
                       0.0f,
                       1.0f);
        const float sink =
            deformer.sinkDepth *
            glm::mix(
                1.0f, soft, std::clamp(deformer.softnessResponse, 0.0f, 1.0f));
        float pattern = 0.0f;
        if (tread)
        {
          const float size = std::max(deformer.treadSize, 0.01f);
          pattern = redAt(*tread,
                          glm::fract(glm::vec2(glm::dot(c, forward),
                                               glm::dot(c, side)) /
                                     size)) *
                    deformer.treadDepth * soft;
        }
        footprint.target[i] = bottom.bottom[i] - here->height - sink - pattern;
      }
    PressSettings settings;
    settings.ridge = deformer.ridge;
    settings.ridgeWidth = std::max(0.15f, deformer.sinkDepth * 2.5f);
    settings.ridgeMost = std::max(0.01f, deformer.sinkDepth * 0.4f);
    settings.motion = motion;
    const float pressed = field.map.press(footprint, settings);
    const glm::vec2 low = field.map.centerOf(bottom.from);
    const glm::vec2 high = field.map.centerOf(bottom.from + bottom.size - 1);
    if (field.contacts)
      field.contacts(Contact{.deformer = owner,
                             .mesh = mesh,
                             .low = low,
                             .high = high,
                             .motion = motion,
                             .sinkDepth = deformer.sinkDepth,
                             .pressed = pressed});

    // Grass on it worn away where it's pressed (over at least a texel of
    // its maps, which are coarser than a rut).
    if (pressed > 0.0f && field.wearsGrass && grass &&
        field.entity.hasComponent<GrassComponent>())
      if (GrassMaps* maps = grass->maps(field.entity))
      {
        const GrassGrid& grid = maps->grid();
        const glm::vec2 texel = (grid.max - grid.min) /
                                glm::max(glm::vec2(grid.size), glm::vec2(1.0f));
        grass->stamp(field.entity,
                     GrassMaps::Coverage,
                     glm::vec3((low + high) * 0.5f, ground->height),
                     std::max(0.5f * std::min(high.x - low.x, high.y - low.y),
                              0.75f * std::max(texel.x, texel.y)),
                     0.0f,
                     GrassMaps::Blend::Toward,
                     0.3f);
      }
  }
}

void DeformationModule::buildPatchMesh()
{
  // Rings around its middle, each twice the last's spacing and size; the
  // first a full square. A vertex on a ring's outer edge between two of the
  // next ring's is put halfway between them (its uv says which way), so they
  // meet without cracks; the last ring's edge has a skirt hung under it
  // (a downward normal) against any gap to the ground's own mesh. The
  // normal's length is the spacing the map is smoothed over for the vertex
  // (its ring's, or on its outer edge the next's, as that ring's vertices
  // there are), so finer detail than its triangles doesn't alias into
  // spikes.
  MeshData mesh;
  constexpr int n = kPatchCells;
  const auto vertex =
      [&](glm::vec2 at, glm::vec2 stitch, bool skirt, float smoothing)
  {
    MeshVertex v;
    v.position = glm::vec3(at, 0.0f);
    v.normal = glm::vec3(0.0f, 0.0f, skirt ? -smoothing : smoothing);
    v.uv = stitch;
    mesh.vertices.push_back(v);
    return static_cast<uint32_t>(mesh.vertices.size() - 1);
  };
  for (int ring = 0; ring < kPatchRings; ++ring)
  {
    const float spacing = kPatchSpacing * static_cast<float>(1 << ring);
    const bool last = ring == kPatchRings - 1;
    const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
    for (int j = -n; j <= n; ++j)
      for (int i = -n; i <= n; ++i)
      {
        glm::vec2 stitch(0.0f);
        if (!last && (std::abs(j) == n) && (i & 1))
          stitch = {spacing, 0.0f};
        else if (!last && (std::abs(i) == n) && (j & 1))
          stitch = {0.0f, spacing};
        const bool edge = std::abs(i) == n || std::abs(j) == n;
        vertex(glm::vec2(i, j) * spacing,
               stitch,
               false,
               edge && !last ? 2.0f * spacing : spacing);
      }
    const auto at = [&](int i, int j)
    { return first + uint32_t((j + n) * (2 * n + 1) + (i + n)); };
    for (int j = -n; j < n; ++j)
      for (int i = -n; i < n; ++i)
      {
        // Inside the ring before it: that one's.
        if (ring > 0 && i >= -n / 2 && i < n / 2 && j >= -n / 2 && j < n / 2)
          continue;
        mesh.indices.insert(mesh.indices.end(),
                            {at(i, j),
                             at(i + 1, j),
                             at(i + 1, j + 1),
                             at(i, j),
                             at(i + 1, j + 1),
                             at(i, j + 1)});
      }
    if (!last)
      continue;
    // The skirt, both ways round (seen from either side).
    std::vector<glm::ivec2> edge;
    for (int i = -n; i < n; ++i)
      edge.push_back({i, -n});
    for (int j = -n; j < n; ++j)
      edge.push_back({n, j});
    for (int i = n; i > -n; --i)
      edge.push_back({i, n});
    for (int j = n; j > -n; --j)
      edge.push_back({-n, j});
    std::vector<uint32_t> hung;
    for (const glm::ivec2 e : edge)
      hung.push_back(
          vertex(glm::vec2(e) * spacing, glm::vec2(0.0f), true, spacing));
    for (std::size_t k = 0; k < edge.size(); ++k)
    {
      const std::size_t next = (k + 1) % edge.size();
      const uint32_t a = at(edge[k].x, edge[k].y);
      const uint32_t b = at(edge[next].x, edge[next].y);
      const uint32_t c = hung[next];
      const uint32_t d = hung[k];
      mesh.indices.insert(
          mesh.indices.end(), {a, d, c, a, c, b, a, b, c, a, c, d});
    }
  }
  m_patchMesh = m_meshes->add(mesh);
}

// --- the game's API -----------------------------------------------------

DeformationMap* DeformationModule::map(const Entity& ground)
{
  if (!ground.isValid() || !ground.isAlive())
    return nullptr;
  Field& field = m_fields[keyOf(ground)];
  field.entity = ground;
  return &field.map;
}

float DeformationModule::offsetAt(const Entity& ground, glm::vec2 point) const
{
  const Field* field = fieldOf(ground);
  return field ? field->map.sample(point) : 0.0f;
}

std::optional<float> DeformationModule::heightAt(const Entity& ground,
                                                 glm::vec2 point) const
{
  const Field* field = fieldOf(ground);
  if (!field || !field->surface)
    return std::nullopt;
  const std::optional<GroundSurface::Point> at = field->surface->at(point);
  if (!at)
    return std::nullopt;
  return at->height + field->map.sample(point);
}

void DeformationModule::stamp(const Entity& ground,
                              glm::vec2 center,
                              float heading,
                              glm::vec2 size,
                              float depth,
                              const std::string& shape)
{
  DeformationMap* into = map(ground);
  if (!into)
    return;
  const std::shared_ptr<const TextureData> image =
      !shape.empty() && m_assets ? m_assets->imageTexture(shape) : nullptr;
  PressSettings settings;
  settings.ridge = 0.4f;
  settings.ridgeWidth = 0.06f;
  settings.ridgeMost = std::max(0.005f, depth * 0.5f);
  into->stamp(center, heading, size, depth, image.get(), settings);
}

void DeformationModule::relax(const Entity& ground, float smooth, float fade)
{
  if (Field* field = fieldOf(ground))
    field->map.relax(smooth, fade);
}

void DeformationModule::clearRegion(const Entity& ground,
                                    std::span<const glm::vec2> polygon)
{
  if (Field* field = fieldOf(ground))
    field->map.clear(polygon);
}

void DeformationModule::clearRegion(const Entity& ground,
                                    glm::vec2 low,
                                    glm::vec2 high)
{
  if (Field* field = fieldOf(ground))
    field->map.clear(low, high);
}

std::vector<uint8_t> DeformationModule::save(const Entity& ground) const
{
  const Field* field = fieldOf(ground);
  return field ? field->map.save() : DeformationMap().save();
}

bool DeformationModule::load(const Entity& ground,
                             std::span<const uint8_t> bytes)
{
  DeformationMap* into = map(ground);
  return into && into->load(bytes);
}

void DeformationModule::setSoftness(const Entity& ground, float softness)
{
  if (map(ground))
  {
    Field& field = m_fields[keyOf(ground)];
    field.softness = std::clamp(softness, 0.0f, 1.0f);
    field.softnessAt = nullptr;
  }
}

void DeformationModule::setSoftness(const Entity& ground, Softness softness)
{
  if (map(ground))
    m_fields[keyOf(ground)].softnessAt = std::move(softness);
}

void DeformationModule::setWearsGrass(const Entity& ground, bool wears)
{
  if (map(ground))
    m_fields[keyOf(ground)].wearsGrass = wears;
}

void DeformationModule::setContacts(const Entity& ground, Contacts contacts)
{
  if (map(ground))
    m_fields[keyOf(ground)].contacts = std::move(contacts);
}

} // namespace sfs
