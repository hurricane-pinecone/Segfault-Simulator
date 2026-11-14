#include "engine/runtime/animation/animationSystem.h"

#include "engine/core/components/animatorComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/restPoseComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/rendering/meshLibrary.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace sfs
{

namespace
{
// How often an animator none of whose meshes was drawn is posed (its time
// runs every update).
constexpr uint64_t kUnseenEvery = 8;
// While editing, what's below an animator is found again this often (the
// hierarchy is being changed by hand); and, while a mesh below one is still
// loading, as often in a game.
constexpr uint32_t kRebindFrames = 30;

JointPose poseOf(const Entity& entity)
{
  if (!entity.hasComponent<TransformComponent>())
    return {};
  const TransformComponent& t = entity.getComponent<TransformComponent>();
  return {t.position, t.rotation, t.scale};
}

// Its RestPoseComponent's pose, or its transform's if it has none.
JointPose restOf(const Entity& entity)
{
  if (!entity.hasComponent<RestPoseComponent>())
    return poseOf(entity);
  const RestPoseComponent& rest = entity.getComponent<RestPoseComponent>();
  return {rest.position, rest.rotation, rest.scale};
}
} // namespace

AnimationSystem::AnimationSystem(MeshLibrary& meshes, AssetStore& assets)
    : m_meshes(meshes), m_assets(assets)
{
  registerComponent<AnimatorComponent>();
  // Game time, but once a frame: posing is only seen once a frame.
  setTimeStep(TimeStep::Frame);
}

// Nothing to free: the mesh library lets go of posed copies no one keeps
// (and frees the rest with itself, whichever goes first).
AnimationSystem::~AnimationSystem() = default;

AnimationSystem::Binding*
AnimationSystem::bindingOf(const Entity& animator) const
{
  return animator.isAlive() && animator.hasComponent<AnimatorComponent>()
             ? &animator.getComponent<AnimatorComponent>()
             : nullptr;
}

AnimatorComponent* AnimationSystem::animatorOf(const Entity& animator)
{
  return bindingOf(animator) && animator.isAlive()
             ? &animator.getComponent<AnimatorComponent>()
             : nullptr;
}

void AnimationSystem::play(const Entity& entity,
                           std::string_view clip,
                           float fade)
{
  AnimatorComponent* animator = animatorOf(entity);
  if (!animator)
    return;
  animator->playing = true;
  if (clip == animator->clip && animator->started)
    return;
  animator->clip = std::string(clip);
  animator->fadeRequest = fade;
  animator->restart = true;
}

void AnimationSystem::restart(const Entity& entity)
{
  AnimatorComponent* animator = animatorOf(entity);
  if (!animator)
    return;
  animator->playing = true;
  animator->time = 0.0f;
  animator->restart = true;
  animator->fadeRequest = 0.0f;
}

const AnimationSet* AnimationSystem::clips(const Entity& animator) const
{
  const Binding* binding = bindingOf(animator);
  return binding ? binding->clips.get() : nullptr;
}

float AnimationSystem::length(const Entity& animator,
                              std::string_view clip) const
{
  const AnimationSet* set = clips(animator);
  if (!set)
    return 0.0f;
  const AnimationClip* found = set->find(
      clip.empty()
          ? std::string_view(animator.getComponent<AnimatorComponent>().clip)
          : clip);
  return found ? found->duration : 0.0f;
}

std::vector<std::string>
AnimationSystem::clipNames(const Entity& animator) const
{
  std::vector<std::string> names;
  if (const AnimationSet* set = clips(animator))
    for (const AnimationClip& clip : set->clips)
      names.push_back(clip.name);
  return names;
}

void AnimationSystem::setPreview(const Entity& animator, bool preview)
{
  if (Binding* binding = bindingOf(animator))
    binding->preview = preview;
}

bool AnimationSystem::previewing(const Entity& animator) const
{
  const Binding* binding = bindingOf(animator);
  return binding && binding->preview;
}

void AnimationSystem::rebind(const Entity& animator)
{
  for (const Entity& entity : getEntities())
    if (Binding* binding = bindingOf(entity);
        binding && (!animator.isValid() || entity == animator))
      binding->stale = true;
}

std::shared_ptr<const AnimationSet>
AnimationSystem::clipsFor(const Binding& binding,
                          const AnimatorComponent& animator,
                          bool& waiting) const
{
  waiting = false;
  std::optional<AssetId> model;
  if (!animator.source.empty())
  {
    if (const std::optional<AssetMeshName> named =
            parseAssetMeshName(animator.source))
      model = named->model;
  }
  else
    for (const SkinBinding& skin : binding.skins)
      if (const std::optional<AssetMeshName> named =
              parseAssetMeshName(m_meshes.nameOf(skin.base)))
      {
        model = named->model;
        break;
      }
  if (!model)
  {
    waiting = !binding.complete;
    return nullptr;
  }
  const ModelData* data = nullptr;
  const AssetStore::LoadState state = m_assets.requestModel(*model, &data);
  waiting = state == AssetStore::LoadState::Loading;
  return data ? data->animations : nullptr;
}

void AnimationSystem::bind(const Entity& entity,
                           Binding& binding,
                           AnimatorComponent& animator)
{
  binding.nodes.clear();
  binding.parents.clear();
  binding.byName.clear();
  binding.skins.clear();
  binding.animated.clear();
  binding.targets.clear();
  binding.targetsOf = nullptr;
  binding.complete = true;
  binding.stale = false;
  binding.framesSinceBind = 0;

  // Everything below it, down to another animator (whose entities are its).
  const auto add = [&](const Entity& node, int parent)
  {
    const int index = static_cast<int>(binding.nodes.size());
    binding.nodes.push_back(node);
    binding.parents.push_back(parent);
    if (node.hasComponent<NameComponent>())
      binding.byName.try_emplace(
          node.getComponent<NameComponent>().name, index);
  };
  add(entity, -1);
  const hierarchy::DescendantRange below = hierarchy::descendants(entity);
  for (auto it = below.begin(); it != below.end(); ++it)
  {
    const Entity node = *it;
    if (node.hasComponent<AnimatorComponent>())
    {
      it.skipChildren();
      continue;
    }
    // In preorder, its parent is the last node added or one of its
    // ancestors.
    const Entity parent = hierarchy::parentOf(node);
    int above = static_cast<int>(binding.nodes.size()) - 1;
    while (above >= 0 && binding.nodes[above] != parent)
      above = binding.parents[above];
    add(node, above);
  }

  for (std::size_t i = 0; i < binding.nodes.size(); ++i)
  {
    const Entity& node = binding.nodes[i];
    // A model's nodes recorded their rest pose as they loaded; one made in
    // code rests as it's first found.
    if (m_live && !node.hasComponent<RestPoseComponent>())
      hierarchy::recordRestPose(node);
    if (!node.hasComponent<MeshRendererComponent>())
      continue;
    const MeshHandle mesh = node.getComponent<MeshRendererComponent>().mesh;
    if (m_meshes.isPending(mesh))
      binding.complete = false;
    const MeshSkin* skin = m_meshes.skin(mesh);
    if (!skin)
      continue;
    SkinBinding bound;
    bound.mesh = node;
    bound.node = i;
    bound.base = mesh;
    for (const std::string& joint : skin->joints)
    {
      const auto found = binding.byName.find(joint);
      bound.joints.push_back(found != binding.byName.end() ? found->second
                                                           : -1);
    }
    binding.skins.push_back(std::move(bound));
  }
  binding.animated.assign(binding.nodes.size(), 0);
  (void)animator;
}

void AnimationSystem::useSet(Binding& binding, const AnimationSet& set)
{
  if (binding.targetsOf != &set)
  {
    // A new set: which nodes any of its clips move.
    binding.targets.clear();
    binding.targetsOf = &set;
    std::fill(binding.animated.begin(), binding.animated.end(), 0);
    for (const std::string& node : set.animatedNodes())
      if (const auto found = binding.byName.find(node);
          found != binding.byName.end() && found->second > 0)
        binding.animated[static_cast<std::size_t>(found->second)] = 1;
  }
}

const std::vector<int>& AnimationSystem::targetsFor(Binding& binding,
                                                    const AnimationSet& set,
                                                    const AnimationClip& clip)
{
  useSet(binding, set);
  auto [it, made] = binding.targets.try_emplace(&clip);
  if (made)
    for (const AnimationChannel& channel : clip.channels)
    {
      const auto found = binding.byName.find(channel.node);
      // Never the animator itself: it's where the game places the model.
      it->second.push_back(found != binding.byName.end() && found->second > 0
                               ? found->second
                               : -1);
    }
  return it->second;
}

void AnimationSystem::advance(Binding& binding,
                              AnimatorComponent& animator,
                              float seconds,
                              bool running)
{
  const AnimationSet* set = binding.clips.get();
  const auto durationOf = [set](const std::string& name)
  {
    const AnimationClip* clip = set ? set->find(name) : nullptr;
    return clip ? clip->duration : 0.0f;
  };
  AnimationPlayback& playback = animator.playback;

  // A clip changed (by play(), or in the Inspector), or played again.
  const AnimationPlayback::Layer* current = playback.current();
  const bool changed = !current || current->clip != animator.clip;
  if (animator.clip.empty())
    playback.clear();
  else if (changed || animator.restart)
  {
    const float duration = durationOf(animator.clip);
    // From its start when played; as `time` says when it's set in the
    // fields (or the scene starts).
    const float start = animator.restart && animator.started
                            ? 0.0f
                            : std::clamp(animator.time, 0.0f, 1.0f) * duration;
    const float fade = !m_live || !animator.started   ? 0.0f
                       : animator.fadeRequest >= 0.0f ? animator.fadeRequest
                                                      : animator.fade;
    if (!changed)
      playback.clear(); // the same clip from its start
    playback.play(animator.clip, fade, start);
    binding.timeWritten = -1.0f;
  }
  animator.restart = false;
  animator.fadeRequest = -1.0f;

  // `time` set from outside (the Inspector, a script): seek to it.
  if (AnimationPlayback::Layer* layer = playback.current();
      layer && binding.timeWritten >= 0.0f &&
      animator.time != binding.timeWritten)
    layer->time =
        std::clamp(animator.time, 0.0f, 1.0f) * durationOf(layer->clip);

  if (running && animator.playing)
    playback.advance(seconds * animator.speed, animator.loop, durationOf);

  animator.finished = false;
  if (const AnimationPlayback::Layer* layer = playback.current())
  {
    const float duration = durationOf(layer->clip);
    animator.time = duration > 0.0f ? layer->time / duration : 0.0f;
    animator.finished = !animator.loop && duration > 0.0f &&
                        (animator.speed >= 0.0f ? layer->time >= duration
                                                : layer->time <= 0.0f);
  }
  binding.timeWritten = animator.time;
  animator.started = true;
}

void AnimationSystem::pose(Binding& binding, AnimatorComponent& animator)
{
  const AnimationSet* set = binding.clips.get();
  const std::size_t count = binding.nodes.size();
  m_pose.resize(count);
  // What its clips move starts from rest (so a channel one clip has and
  // another doesn't goes back); the rest as they are now. While editing
  // nothing moves the joints, so they're at rest as they are.
  if (set)
    useSet(binding, *set);
  for (std::size_t i = 0; i < count; ++i)
    m_pose[i] = binding.animated[i] && m_live ? restOf(binding.nodes[i])
                                              : poseOf(binding.nodes[i]);

  const auto& layers = animator.playback.layers();
  if (set && !layers.empty())
  {
    if (layers.size() == 1)
    {
      if (const AnimationClip* clip = set->find(layers.front().clip))
        applyClip(*clip,
                  layers.front().time,
                  targetsFor(binding, *set, *clip),
                  m_pose);
    }
    else
    {
      // Each layer posed from rest, then mixed by weight: only the nodes
      // they move.
      std::vector<std::vector<JointPose>> posed;
      std::vector<float> weights;
      for (const AnimationPlayback::Layer& layer : layers)
      {
        m_layerPose = m_pose;
        if (const AnimationClip* clip = set->find(layer.clip))
          applyClip(
              *clip, layer.time, targetsFor(binding, *set, *clip), m_layerPose);
        posed.push_back(m_layerPose);
        weights.push_back(layer.weight);
      }
      std::vector<JointPose> mix(layers.size());
      for (std::size_t i = 0; i < count; ++i)
      {
        if (!binding.animated[i])
          continue;
        for (std::size_t l = 0; l < posed.size(); ++l)
          mix[l] = posed[l][i];
        m_pose[i] = blendPoses(mix, weights);
      }
    }
  }

  // Each node relative to the animator, down the tree.
  m_models.resize(count);
  for (std::size_t i = 0; i < count; ++i)
  {
    const int parent = binding.parents[i];
    m_models[i] = parent < 0
                      ? glm::mat4(1.0f)
                      : m_models[static_cast<std::size_t>(parent)] *
                            (binding.nodes[i].hasComponent<TransformComponent>()
                                 ? m_pose[i].matrix()
                                 : glm::mat4(1.0f));
  }

  // Playing: the joints are moved (what's attached to them follows).
  if (m_live)
    for (std::size_t i = 1; i < count; ++i)
      if (binding.animated[i] &&
          binding.nodes[i].hasComponent<TransformComponent>())
      {
        TransformComponent& transform =
            binding.nodes[i].getComponent<TransformComponent>();
        transform.position = m_pose[i].translation;
        transform.rotation = m_pose[i].rotation;
        transform.scale = m_pose[i].scale;
      }

  // Each skinned mesh's palette and bounds.
  for (const SkinBinding& skin : binding.skins)
  {
    const MeshSkin* meshSkin = m_meshes.skin(skin.base);
    if (!meshSkin || meshSkin->joints.size() != skin.joints.size())
      continue;
    m_jointModels.resize(skin.joints.size());
    for (std::size_t j = 0; j < skin.joints.size(); ++j)
      m_jointModels[j] =
          skin.joints[j] >= 0
              ? m_models[static_cast<std::size_t>(skin.joints[j])]
              : m_models[skin.node];
    skinPalette(*meshSkin, m_jointModels, m_models[skin.node], m_palette);
    m_meshes.setSkinPose(skin.base,
                         skin.mesh.getId(),
                         m_palette,
                         skinnedBounds(*meshSkin, m_palette));
    ++m_stats.skins;
  }
}

void AnimationSystem::update(double deltaTime)
{
  SFS_CPU_SCOPE("update/animation");
  ++m_frame;
  m_stats = {};
  const float seconds =
      registry->paused() ? 0.0f : static_cast<float>(deltaTime) * m_timeScale;

  for (const Entity& entity : getEntities())
  {
    if (!entity.isAlive())
      continue;
    AnimatorComponent& animator = entity.getComponent<AnimatorComponent>();
    Binding* found = bindingOf(entity);
    if (!found)
      continue;
    Binding& binding = *found;

    // What's below it, found again when it's changed.
    ++binding.framesSinceBind;
    bool stale = binding.stale || animator.source != binding.sourceSeen;
    for (std::size_t i = 0; !stale && i < binding.nodes.size(); ++i)
      stale = !binding.nodes[i].isAlive();
    if (!stale && binding.framesSinceBind >= kRebindFrames &&
        (!m_live || !binding.complete))
      stale = true;
    if (stale)
    {
      bind(entity, binding, animator);
      bool waiting = false;
      binding.clips = clipsFor(binding, animator, waiting);
      binding.sourceSeen = waiting ? std::string() : animator.source;
      if (waiting)
        binding.complete = false; // look again soon
    }
    if (!binding.clips)
    {
      // Nothing to play (yet): its meshes as bound.
      for (const SkinBinding& skin : binding.skins)
        m_meshes.keepSkin(skin.base, skin.mesh.getId());
      continue;
    }
    ++m_stats.animators;

    const bool running = m_live || binding.preview;
    advance(binding, animator, seconds, running);

    // Off screen: posed now and then (its time ran anyway).
    bool seen = binding.skins.empty() || !m_live;
    for (const SkinBinding& skin : binding.skins)
    {
      const MeshLibrary::SkinPose* pose =
          m_meshes.skinPose(skin.base, skin.mesh.getId());
      seen = seen || !pose || pose->drawn;
    }
    if (!seen && (m_frame + entity.getId()) % kUnseenEvery != 0)
    {
      for (const SkinBinding& skin : binding.skins)
        m_meshes.keepSkin(skin.base, skin.mesh.getId());
      continue;
    }
    pose(binding, animator);
    ++m_stats.posed;
  }
  m_meshes.releaseUnusedSkins();
}

} // namespace sfs
