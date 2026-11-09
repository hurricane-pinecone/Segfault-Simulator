#include "engine/runtime/rendering/modules/deformationModule.h"

#include "engine/core/components/deformerComponent.h"
#include "engine/core/components/grassComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/mesh.h"
#include "engine/core/rendering/terrain.h"
#include "engine/core/util/cpuTiming.h"
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
// enough for a ridge and the lugs above the tyre's lowest point (a wheel
// sunk below the ground, above its lowest point).
constexpr float kAboveGround = 0.12f;
// How far a tyre's lugs stand proud (metres): a rut's floor that deep is
// left as its lugs marked it when its walls slump (PressSettings::tread).
constexpr float kTread = 0.03f;
// Most presses a frame for one of a deformer's meshes along the way it went
// since it last pressed (sliding, and rolling: each of more places, their
// lowest, if there are more), and the farthest it's pressed along (further
// in a frame, it was put there, not driven): kFarthestSweep, or as far as
// kFastest goes in the frame's game time (the game going fast).
constexpr int kMostSweeps = 16;
constexpr int kMostRolls = 4;
constexpr float kFarthestSweep = 1.0f;
constexpr float kFastest = 25.0f;
// How far a deformer's mesh goes before it presses again (metres), rather
// than each frame, so what it costs goes with the distance, not the frame
// rate. One rolling (a wheel) is pressed where it is, often enough that
// the ground under each lug is pressed within kRollError of as deep as
// pressing it each frame would (a wheel of radius r, pressed each s, is at
// most s * s / 8r above its lowest: 8 cm on one of 80 cm, 5 cm on one of
// 30); one sliding (a tine) is swept along the way it went, each
// kSlideStride, or sooner once it's sunk or risen (lowered) kRiseStride or
// turned enough to move any of it a texel.
constexpr float kRollError = 0.001f;
constexpr float kRollStrideMost = 0.12f;
constexpr float kSlideStride = 0.06f;
constexpr float kRiseStride = 0.003f;

// How far a mesh moved from `was` to `now`: its origin over the ground and
// up or down, and the most any point `radius` from its origin moved by its
// turning (a wheel's rim as it rolls).
struct Moved
{
  float along = 0.0f;
  float rise = 0.0f;
  float turned = 0.0f;
};
Moved movedFrom(const glm::mat4& was, const glm::mat4& now, float radius)
{
  Moved moved;
  moved.along = glm::length(glm::vec2(now[3] - was[3]));
  moved.rise = std::abs(now[3].z - was[3].z);
  for (int axis = 0; axis < 3; ++axis)
    moved.turned = std::max(
        moved.turned, glm::length(glm::vec3(now[axis] - was[axis])) * radius);
  return moved;
}

// Rolling rather than sliding: gone a way, and turned about as far as it
// went (a wheel, a disc), so what's under it stays where it is as it goes.
// (A wheel steered where it stands slides.)
bool rolling(const Moved& moved, float texel)
{
  return moved.along >= 0.5f * texel && moved.turned >= texel &&
         moved.turned > 0.5f * moved.along;
}

// Copies of `here` moved back by each of `shifts` (texels), as one: the
// lowest of them over each texel.
void lowestOf(const Underside& here,
              std::span<const glm::ivec2> shifts,
              Underside& out)
{
  glm::ivec2 low(std::numeric_limits<int>::max());
  glm::ivec2 high(std::numeric_limits<int>::lowest());
  for (const glm::ivec2 shift : shifts)
  {
    low = glm::min(low, here.from - shift);
    high = glm::max(high, here.from + here.size - shift);
  }
  out.from = low;
  out.size = high - low;
  out.bottom.assign(std::size_t(out.size.x) * out.size.y,
                    std::numeric_limits<float>::quiet_NaN());
  out.coverage.assign(out.bottom.size(), 0.0f);
  for (const glm::ivec2 shift : shifts)
  {
    const glm::ivec2 at = here.from - shift - out.from;
    for (int y = 0; y < here.size.y; ++y)
      for (int x = 0; x < here.size.x; ++x)
      {
        const std::size_t from = std::size_t(y) * here.size.x + x;
        const float z = here.bottom[from];
        if (std::isnan(z))
          continue;
        const std::size_t to = std::size_t(at.y + y) * out.size.x + at.x + x;
        if (std::isnan(out.bottom[to]) || z < out.bottom[to])
          out.bottom[to] = z;
        out.coverage[to] = std::max(out.coverage[to], here.coverage[from]);
      }
  }
}

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
  m_frameSeconds = static_cast<float>(deltaTime);
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

  {
    SFS_CPU_SCOPE("deformation/press");
    deform();
  }

  PuddleModule* puddles = m_host ? m_host->module<PuddleModule>() : nullptr;
  for (const Seen& terrain : seen)
  {
    {
      SFS_CPU_SCOPE("deformation/draw");
      draw(context, terrain.material, *terrain.drawn, *terrain.field);
    }
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
  if (context.feedOn)
    command.feedInstances = {instance};
  m_commands.push_back(std::move(command));
}

void DeformationModule::deform()
{
  if (m_fields.empty() || m_registry->view<DeformerComponent>().empty())
  {
    m_placed.clear();
    return;
  }
  m_stillPlaced.clear();
  const float farthest = std::max(
      kFarthestSweep,
      kFastest * m_frameSeconds * static_cast<float>(m_registry->timeScale()));
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
    // Only once it's gone far enough since it last pressed (pressing again
    // where it stands changes nothing).
    const glm::mat4 model = hierarchy::worldMatrix(mesh);
    const uint64_t key = keyOf(mesh);
    const auto last = m_placed.find(key);
    glm::vec2 motion(0.0f);
    bool rolled = false;
    float stride = kSlideStride;
    if (last != m_placed.end())
    {
      const MeshHandle handle = mesh.getComponent<MeshRendererComponent>().mesh;
      const Aabb* bounds = m_meshes->posedBounds(handle, mesh.getId());
      const float radius =
          bounds && !bounds->empty()
              ? std::max(glm::length(bounds->min), glm::length(bounds->max))
              : 0.0f;
      const Moved moved = movedFrom(last->second.model, model, radius);
      const float texel = kDeformationTexel;
      // Gone further than its turning can tell (a wheel turns round in
      // under 6.3 radii): as it went last time.
      rolled =
          moved.along > radius ? last->second.rolled : rolling(moved, texel);
      // Rolling: its radius is how far it reaches below its middle (its
      // axle).
      stride =
          !rolled ? kSlideStride
                  : std::clamp(
                        std::sqrt(8.0f * kRollError *
                                  std::max(model[3].z -
                                               bounds->transformed(model).min.z,
                                           0.0f)),
                        texel,
                        kRollStrideMost);
      if (moved.along < stride &&
          (rolled || (moved.rise < kRiseStride && moved.turned < texel)))
      {
        m_stillPlaced[key] = last->second;
        continue;
      }
      motion = glm::vec2(model[3] - last->second.model[3]);
    }
    m_stillPlaced[key] = Placed{model, rolled};
    pressMesh(mesh, model, at, *deformer, motion, rolled, stride, farthest);
  }
  std::swap(m_placed, m_stillPlaced);
}

void DeformationModule::pressMesh(const Entity& mesh,
                                  const glm::mat4& model,
                                  const Entity& owner,
                                  const DeformerComponent& deformer,
                                  glm::vec2 motion,
                                  bool rolled,
                                  float stride,
                                  float farthest)
{
  const MeshHandle handle = mesh.getComponent<MeshRendererComponent>().mesh;
  // A skinned mesh as an animator poses it (a foot down where it steps).
  const Aabb* bounds = m_meshes->posedBounds(handle, mesh.getId());
  if (!bounds || bounds->empty())
    return;
  const Aabb world = bounds->transformed(model);
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
    // (A wheel sat down into the ground, in mud: from its lowest, so it
    // costs no more than on firm ground. Its lowest point, not its bounds':
    // turned, their corners reach as much as 1.4 x its radius below its
    // axle, and from there nothing of it would be under `below`: it'd press
    // only at each quarter turn, a rut of separate scoops.)
    const float below =
        (rolled ? std::min(ground->height,
                           lowestPointOf(mesh, handle, model, world.min.z))
                : ground->height) +
        kAboveGround;
    if (world.min.z > below)
      continue;
    Underside& bottom = m_bottom;
    {
      SFS_CPU_SCOPE("deformation/underside");
      bottom = undersideOf(mesh, handle, model, below, field.map.texel());
    }
    if (bottom.empty())
      continue;
    // Pressed all along the way it went since it last pressed, its
    // underside moved back along it: sliding (a tine's point), at each texel,
    // so each tine cuts a furrow, not a row of dimples, and the soil it
    // pushes aside rises evenly along it; rolling, at each stride, where it
    // went further than two in a frame (the game going fast), so its track's
    // unbroken. At most kMostSweeps (kMostRolls) presses, each of an even
    // share of those places (their lowest) if there are more.
    const float texel = field.map.texel();
    const float moved = glm::length(motion);
    int places = 1;
    if (moved > 1e-6f && moved < farthest)
    {
      const float each = rolled ? stride : texel;
      if (!rolled || moved > 2.0f * stride)
        places = static_cast<int>(std::ceil(moved / std::max(each, texel)));
    }
    m_shifts.clear();
    for (int k = 1; k <= places; ++k)
    {
      const glm::ivec2 shift =
          k >= places
              ? glm::ivec2(0)
              : glm::ivec2(glm::round(
                    motion * (1.0f - static_cast<float>(k) / places) / texel));
      if (m_shifts.empty() || shift != m_shifts.back())
        m_shifts.push_back(shift);
    }
    const int count = static_cast<int>(m_shifts.size());
    const int presses = std::min(count, rolled ? kMostRolls : kMostSweeps);
    // All it goes over, and the ground under that.
    const Underside* over = &bottom;
    if (count > 1)
    {
      lowestOf(bottom, m_shifts, m_swept);
      over = &m_swept;
    }
    const std::shared_ptr<const TextureData> tread =
        !deformer.tread.empty() && m_assets
            ? m_assets->imageTexture(deformer.tread)
            : nullptr;

    {
      SFS_CPU_SCOPE("deformation/ground under");
      groundUnder(field, *over);
    }
    PressSettings settings;
    settings.ridge = deformer.ridge;
    settings.ridgeWidth = std::max(0.15f, deformer.sinkDepth * 2.5f);
    const float ridgeMost = std::max(0.01f, deformer.sinkDepth * 0.4f);
    settings.motion = motion;
    settings.tread = rolled ? kTread : 0.0f;
    float pressed = 0.0f;
    float lowest = 0.0f;   // its deepest target, all told
    float softness = 0.0f; // summed over the texels under it
    int texels = 0;
    for (int k = 0; k < presses; ++k)
    {
      // This press's share of the places: one, or the lowest of several.
      const int first = k * count / presses;
      const int end = (k + 1) * count / presses;
      const Underside* piece = &bottom;
      glm::ivec2 from = bottom.from - m_shifts[first];
      if (end - first > 1)
      {
        lowestOf(
            bottom,
            std::span<const glm::ivec2>(m_shifts).subspan(first, end - first),
            m_piece);
        piece = &m_piece;
        from = m_piece.from;
      }
      // Each texel pressed down to its sink below the underside, deeper on
      // soft soil and where the tread presses.
      Footprint& footprint = m_footprint;
      footprint.from = from;
      footprint.size = piece->size;
      footprint.target.assign(
          piece->bottom.size(), std::numeric_limits<float>::quiet_NaN());
      footprint.coverage.assign(piece->coverage.begin(), piece->coverage.end());
      float deepest = 0.0f;
      for (int y = 0; y < piece->size.y; ++y)
        for (int x = 0; x < piece->size.x; ++x)
        {
          const std::size_t i = std::size_t(y) * piece->size.x + x;
          const glm::ivec2 at = from + glm::ivec2(x, y) - over->from;
          const std::size_t under = std::size_t(at.y) * over->size.x + at.x;
          if (std::isnan(piece->bottom[i]) || std::isnan(m_groundHeight[under]))
            continue;
          const float soft = m_groundSoftness[under];
          const float sink =
              deformer.sinkDepth *
              glm::mix(1.0f,
                       soft,
                       std::clamp(deformer.softnessResponse, 0.0f, 1.0f));
          float pattern = 0.0f;
          if (tread)
          {
            const glm::vec2 c = field.map.centerOf(from + glm::ivec2(x, y));
            const float size = std::max(deformer.treadSize, 0.01f);
            pattern = redAt(*tread,
                            glm::fract(glm::vec2(glm::dot(c, forward),
                                                 glm::dot(c, side)) /
                                       size)) *
                      deformer.treadDepth * soft;
          }
          footprint.target[i] =
              piece->bottom[i] - m_groundHeight[under] - sink - pattern;
          deepest = std::min(deepest, footprint.target[i]);
          softness += soft;
          ++texels;
        }
      lowest = std::min(lowest, deepest);
      // A wheel sunk deep (in mud) pushes its ridge up higher.
      settings.ridgeMost =
          rolled ? std::max(ridgeMost, -0.4f * deepest) : ridgeMost;
      SFS_CPU_SCOPE("deformation/map press");
      pressed += field.map.press(footprint, settings);
    }
    const glm::vec2 low = field.map.centerOf(over->from);
    const glm::vec2 high = field.map.centerOf(over->from + over->size - 1);
    if (field.contacts)
      field.contacts(
          Contact{.deformer = owner,
                  .mesh = mesh,
                  .low = low,
                  .high = high,
                  .motion = motion,
                  .sinkDepth = deformer.sinkDepth,
                  .pressed = pressed,
                  .depth = -lowest,
                  .softness = texels > 0 ? softness / texels : 0.0f});

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

void DeformationModule::groundUnder(const Field& field, const Underside& bottom)
{
  // Looked up at the corners of blocks of kBlock x kBlock texels (each
  // once, and only around texels under the deformer). Where a block's
  // corners are all on one of the surface's triangles, the texels between
  // are on its plane (else each is looked up); the softness is the corners'
  // between them (the same, where it's the same at all four: a game's
  // softness is asked for once a block, not for each texel).
  constexpr int kBlock = 4;
  const glm::ivec2 size = bottom.size;
  const glm::ivec2 blocks = (size + kBlock - 1) / kBlock;
  const int across = blocks.x + 1;
  m_corners.assign(std::size_t(across) * (blocks.y + 1), Corner{});
  const std::size_t count = std::size_t(size.x) * size.y;
  m_groundHeight.assign(count, std::numeric_limits<float>::quiet_NaN());
  m_groundSoftness.assign(count, 0.0f);
  const GroundSurface& surface = *field.surface;
  const auto softnessAt = [&](glm::vec2 point)
  {
    return std::clamp(
        field.softnessAt ? field.softnessAt(point) : field.softness,
        0.0f,
        1.0f);
  };
  const auto cornerAt = [&](int x, int y) -> const Corner&
  {
    Corner& corner = m_corners[std::size_t(y) * across + x];
    if (!corner.asked)
    {
      corner.asked = true;
      const glm::vec2 point =
          field.map.centerOf(bottom.from + glm::ivec2(x, y) * kBlock);
      corner.on = surface.at(point, corner.triangle).has_value();
      corner.softness = softnessAt(point);
    }
    return corner;
  };
  for (int by = 0; by < blocks.y; ++by)
    for (int bx = 0; bx < blocks.x; ++bx)
    {
      const glm::ivec2 first = glm::ivec2(bx, by) * kBlock;
      const glm::ivec2 last = glm::min(first + kBlock, size);
      bool any = false;
      for (int y = first.y; y < last.y && !any; ++y)
        for (int x = first.x; x < last.x && !any; ++x)
          any = !std::isnan(bottom.bottom[std::size_t(y) * size.x + x]);
      if (!any)
        continue;
      const Corner* corners[4] = {&cornerAt(bx, by),
                                  &cornerAt(bx + 1, by),
                                  &cornerAt(bx, by + 1),
                                  &cornerAt(bx + 1, by + 1)};
      bool oneTriangle = true;
      for (const Corner* corner : corners)
        oneTriangle = oneTriangle && corner->on &&
                      corner->triangle == corners[0]->triangle;
      for (int y = first.y; y < last.y; ++y)
        for (int x = first.x; x < last.x; ++x)
        {
          const std::size_t i = std::size_t(y) * size.x + x;
          if (std::isnan(bottom.bottom[i]))
            continue;
          const glm::vec2 point =
              field.map.centerOf(bottom.from + glm::ivec2(x, y));
          if (oneTriangle)
            m_groundHeight[i] = surface.heightOn(corners[0]->triangle, point);
          else if (const std::optional<GroundSurface::Point> here =
                       surface.at(point))
            m_groundHeight[i] = here->height;
          const glm::vec2 f =
              glm::vec2(glm::ivec2(x, y) - first) / static_cast<float>(kBlock);
          m_groundSoftness[i] = glm::mix(
              glm::mix(corners[0]->softness, corners[1]->softness, f.x),
              glm::mix(corners[2]->softness, corners[3]->softness, f.x),
              f.y);
        }
    }
}

Underside DeformationModule::undersideOf(const Entity& mesh,
                                         MeshHandle handle,
                                         const glm::mat4& model,
                                         float below,
                                         float texel)
{
  // Posed by an animator: as it's posed now.
  if (m_meshes->skinPose(handle, mesh.getId()))
    return underside(m_meshes->posedPositions(handle, mesh.getId()),
                     m_meshes->indices(handle),
                     model,
                     below,
                     texel);
  return underside(readyMesh(handle), model, below, texel);
}

const UndersideMesh& DeformationModule::readyMesh(MeshHandle handle)
{
  // Made ready once (again if the mesh is changed).
  const std::span<const glm::vec3> positions = m_meshes->positions(handle);
  const std::span<const uint32_t> indices = m_meshes->indices(handle);
  Ready& ready = m_ready[handle.id];
  if (!ready.mesh || ready.positions != positions.data() ||
      ready.vertices != positions.size() || ready.indices != indices.data() ||
      ready.corners != indices.size())
  {
    ready.mesh = std::make_unique<UndersideMesh>(positions, indices);
    ready.positions = positions.data();
    ready.vertices = positions.size();
    ready.indices = indices.data();
    ready.corners = indices.size();
  }
  return *ready.mesh;
}

float DeformationModule::lowestPointOf(const Entity& mesh,
                                       MeshHandle handle,
                                       const glm::mat4& model,
                                       float bound)
{
  if (m_meshes->skinPose(handle, mesh.getId()))
    return bound;
  const UndersideMesh& ready = readyMesh(handle);
  if (ready.empty())
    return bound;
  return std::max(lowestPoint(ready, model), bound);
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

void DeformationModule::relax(const Entity& ground,
                              std::span<const glm::ivec2> tiles,
                              std::span<const glm::vec4> corners)
{
  if (Field* field = fieldOf(ground))
    field->map.relax(tiles, corners);
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
