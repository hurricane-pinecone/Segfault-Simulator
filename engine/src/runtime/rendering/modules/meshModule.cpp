#include "engine/runtime/rendering/modules/meshModule.h"

#include "engine/core/components/meshLodComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/worldTransformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/ext/scalar_constants.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace sfs
{

namespace
{
// Seconds without being drawn with levels after which an entity's levels
// start afresh (no cross-fade from where it was long ago).
constexpr double kLevelsForgotten = 4.0;
} // namespace

void MeshModule::computeCommands(const RenderContext& context)
{
  flush();
  m_commandIndex.clear();
  ++m_frame;
  m_now = std::chrono::duration<double>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();

  if (!m_registry || !context.meshes)
    return;

  const bool check = debugFeatures::checkTransforms();
  for (auto [entity, worldTransform, meshRenderer] :
       m_registry->query<WorldTransformComponent, MeshRendererComponent>())
    addEntity(context, entity, meshRenderer, worldTransform, check);
}

void MeshModule::addEntity(const RenderContext& context,
                           const Entity& entity,
                           const MeshRendererComponent& renderer,
                           const WorldTransformComponent& world,
                           bool check)
{
  const Aabb* localBounds = context.meshes->bounds(renderer.mesh);
  if (!localBounds)
    return; // invalid handle, or a mesh from another library
  // A skinned mesh an animator poses: its own deformed copy, culled as posed.
  MeshHandle drawn = renderer.mesh;
  const MeshLibrary::SkinPose* skin =
      context.meshes->hasSkinPoses()
          ? context.meshes->skinPose(renderer.mesh, entity.getId())
          : nullptr;
  if (skin)
  {
    if (skin->gpu.valid())
      drawn = skin->gpu;
    if (!skin->bounds.empty())
      localBounds = &skin->bounds;
  }

  const glm::mat4 model = hierarchy::cachedWorldMatrix(entity, world, check);
  const Aabb worldBounds = localBounds->transformed(model);

  const bool visible = context.frustum.intersects(worldBounds);
  const bool castsShadow = renderer.castsShadow && context.shadowsEnabled &&
                           context.shadowFrustum.intersects(worldBounds);
  bool castsLightShadow = false;
  if (renderer.castsShadow && context.shadowedLights)
    for (const PointLight& light : *context.shadowedLights)
    {
      const glm::vec4 reach = lightBounds(light); // a spot's: round its cone
      if (worldBounds.intersectsSphere(glm::vec3(reach), reach.w))
      {
        castsLightShadow = true;
        break;
      }
    }
  // (The frame's camera feed's view, while one's filmed.)
  const bool feedSees =
      context.feedOn && context.feedFrustum.intersects(worldBounds);
  if (!visible && !feedSees && !castsShadow && !castsLightShadow)
    return;
  const auto seen =
      static_cast<uint8_t>((visible ? Lit : 0) | (feedSees ? Feed : 0));
  if (skin)
    context.meshes->markSkinDrawn(renderer.mesh, entity.getId());

  // One command per mesh and set of materials: those draw as one instanced
  // batch.
  m_materials.clear();
  for (const std::string& name : renderer.materials)
    m_materials.push_back(context.meshes->material(name));
  while (!m_materials.empty() && !m_materials.back().valid())
    m_materials.pop_back(); // trailing "own material" slots change nothing

  MeshInstance instance{model, renderer.tint};
  instance.emission =
      glm::vec4(renderer.emissionColor * renderer.emission, 0.0f);
  MeshLodComponent* lod = !skin && context.camera
                              ? entity.getComponent<MeshLodComponent>()
                              : nullptr;
  if (lod)
  {
    addLevels(context,
              *lod,
              drawn,
              model,
              instance,
              seen,
              castsShadow,
              castsLightShadow);
    return;
  }
  addInstance(drawn,
              instance,
              seen | (castsShadow ? SunShadow : 0) |
                  (castsLightShadow ? LightShadow : 0));
}

void MeshModule::addInstance(MeshHandle mesh,
                             const MeshInstance& instance,
                             uint8_t passes)
{
  if (!mesh.valid() || passes == 0)
    return;
  std::size_t key = std::hash<uint32_t>()(mesh.id);
  for (const MaterialHandle material : m_materials)
    key = key * 1000003u ^ material.id;
  const auto matches = [&](const MeshDrawCommand& command)
  { return command.mesh == mesh && command.materials == m_materials; };

  auto [first, last] = m_commandIndex.equal_range(key);
  while (first != last && !matches(m_commands[first->second]))
    ++first;
  const std::size_t index = first != last ? first->second : m_commands.size();
  if (first == last)
  {
    m_commandIndex.emplace(key, index);
    m_commands.push_back(
        MeshDrawCommand{.mesh = mesh, .materials = m_materials});
  }

  MeshDrawCommand& command = m_commands[index];
  if (passes & Lit)
    command.instances.push_back(instance);
  // Shadows don't fade: whichever level casts, casts whole.
  MeshInstance solid = instance;
  solid.emission.w = 0.0f;
  if (passes & SunShadow)
    command.shadowInstances.push_back(solid);
  if (passes & LightShadow)
    command.lightShadowInstances.push_back(solid);
  if (passes & Feed)
    command.feedInstances.push_back(instance);
}

void MeshModule::addLevels(const RenderContext& context,
                           MeshLodComponent& lod,
                           MeshHandle mesh,
                           const glm::mat4& model,
                           const MeshInstance& instance,
                           uint8_t seen,
                           bool castsShadow,
                           bool castsLightShadow)
{
  // The levels it has, in order (the mesh's own first), each with where it
  // takes over; the impostor, if it has one, is the level after them.
  struct Level
  {
    MeshHandle mesh;
    float from = 0.0f;
  };
  std::array<Level, 5> levels{};
  std::size_t count = 0;
  levels[count++] = {mesh, 0.0f};
  for (const auto& [handle, from] : {std::pair{lod.lod1, lod.lod1Distance},
                                     std::pair{lod.lod2, lod.lod2Distance},
                                     std::pair{lod.lod3, lod.lod3Distance}})
    if (handle.valid() && from > levels[count - 1].from)
      levels[count++] = {handle, from};
  const std::size_t meshLevels = count;
  if (lod.impostor.valid() && lod.impostorDistance > levels[count - 1].from)
    levels[count++] = {lod.impostor, lod.impostorDistance};

  // How far, in the model's own size.
  const glm::vec3 origin(model[3]);
  const glm::vec3 eye = context.camera->position();
  const float scale = std::max(largestScale(model), 1e-4f);
  const float distance = glm::distance(eye, origin) / scale;
  if (lod.cullDistance > 0.0f && distance > lod.cullDistance)
    return;

  // Its level: the one for its distance, but the one it's at stays until
  // the distance is `hysteresis` past either of its ends (so it doesn't flick
  // back and forth on a boundary). A change cross-fades over `fadeSeconds`.
  std::size_t wanted = 0;
  while (wanted + 1 < count && distance >= levels[wanted + 1].from)
    ++wanted;
  if (m_now - lod.levelSeen > kLevelsForgotten)
    lod.level = lod.previousLevel = static_cast<uint8_t>(wanted);
  lod.levelSeen = m_now;
  const std::size_t at = std::min<std::size_t>(lod.level, count - 1);
  const float margin = std::max(lod.hysteresis, 0.0f);
  const bool beyondNear = at > 0 && distance < levels[at].from - margin;
  const bool beyondFar =
      at + 1 < count && distance >= levels[at + 1].from + margin;
  if (wanted != at && (beyondNear || beyondFar || lod.level >= count))
  {
    lod.previousLevel = static_cast<uint8_t>(at);
    lod.level = static_cast<uint8_t>(wanted);
    lod.levelSince = m_now;
  }
  const std::size_t level = std::min<std::size_t>(lod.level, count - 1);
  const std::size_t previous =
      std::min<std::size_t>(lod.previousLevel, count - 1);
  float fade = 1.0f; // how far in it's faded (1: done)
  if (previous != level && lod.fadeSeconds > 0.0f)
    fade =
        std::clamp(static_cast<float>(m_now - lod.levelSince) / lod.fadeSeconds,
                   0.0f,
                   1.0f);
  if (fade >= 1.0f)
    lod.previousLevel = lod.level;

  // Shadows from the coarser of its level and the shadow level (none from
  // impostors, unless asked for: then the last mesh level's).
  const bool impostor = level >= meshLevels;
  const uint8_t shadowPasses = static_cast<uint8_t>(
      (castsShadow ? SunShadow : 0) | (castsLightShadow ? LightShadow : 0));
  if (shadowPasses != 0 && (!impostor || lod.impostorShadows))
  {
    const std::size_t caster = std::min(
        std::max(level, static_cast<std::size_t>(std::max(lod.shadowLevel, 0))),
        meshLevels - 1);
    addInstance(levels[caster].mesh, instance, shadowPasses);
  }
  if (seen == 0)
    return;

  // A level drawn, dithered by `dither` (MeshInstance::emission.w) while it
  // crosses over. The impostor is turned about the model's up axis to face
  // the camera, showing the view round it nearest the camera's direction (in
  // the model's own frame).
  const auto draw = [&](std::size_t which, float dither)
  {
    MeshInstance shown = instance;
    shown.emission.w = dither;
    if (which < meshLevels)
    {
      addInstance(levels[which].mesh, shown, seen);
      return;
    }
    const glm::vec3 up = glm::normalize(glm::vec3(model[2]));
    glm::vec3 toEye = eye - origin;
    toEye -= up * glm::dot(toEye, up);
    if (glm::dot(toEye, toEye) < 1e-8f)
      toEye = glm::vec3(model[0]);
    const glm::vec3 facing = glm::normalize(toEye);
    const glm::vec3 side = glm::cross(up, facing);
    shown.model[0] = glm::vec4(facing * scale, 0.0f);
    shown.model[1] = glm::vec4(side * scale, 0.0f);
    shown.model[2] = glm::vec4(up * scale, 0.0f);
    const glm::vec3 forward = glm::normalize(glm::vec3(model[0]));
    const glm::vec3 left = glm::cross(up, forward);
    const float frames = static_cast<float>(std::max(lod.impostorFrames, 1));
    float view = std::atan2(glm::dot(facing, left), glm::dot(facing, forward)) /
                     glm::two_pi<float>() * frames +
                 0.5f;
    view -= std::floor(view / frames) * frames; // 0 .. frames
    shown.uvRect =
        glm::vec4(std::floor(view) / frames, 0.0f, 1.0f / frames, 1.0f);
    addInstance(lod.impostor, shown, seen);
  };
  if (fade >= 1.0f)
  {
    draw(level, 0.0f);
    return;
  }
  // Crossing over: the old level giving way to the new, pixel for pixel.
  draw(previous, fade);
  draw(level, -fade);
}

} // namespace sfs
