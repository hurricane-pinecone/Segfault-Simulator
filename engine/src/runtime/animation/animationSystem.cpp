#include "engine/runtime/animation/animationSystem.h"

#include "engine/core/components/animatorComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/rendering/meshLibrary.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
} // namespace

AnimationSystem::AnimationSystem(MeshLibrary& meshes, AssetStore& assets)
    : m_meshes(meshes), m_assets(assets)
{
  registerComponent<AnimatorComponent>();
}

// Nothing to free: the mesh library lets go of posed copies no one keeps
// (and frees the rest with itself, whichever goes first).
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::setPreview(const Entity& animator, bool preview)
{
  if (preview)
    m_previews.insert(animator.getId());
  else
    m_previews.erase(animator.getId());
}

bool AnimationSystem::previewing(const Entity& animator) const
{
  return m_previews.contains(animator.getId());
}

void AnimationSystem::rebind(const Entity& animator)
{
  for (auto& [id, binding] : m_bindings)
    if (!animator.isValid() || binding.animator == animator)
      binding.stale = true;
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

void AnimationSystem::bind(Binding& binding, AnimatorComponent& animator)
{
  // Rest poses carry over (a live joint's transform is posed by now).
  std::unordered_map<Entity::EntityId, std::pair<uint32_t, JointPose>> rest;
  for (std::size_t i = 0; i < binding.nodes.size() && i < binding.rest.size();
       ++i)
    rest[binding.nodes[i].getId()] = {
        binding.nodes[i].getGeneration(), binding.rest[i]};

  binding.nodes.clear();
  binding.parents.clear();
  binding.byName.clear();
  binding.skins.clear();
  binding.rest.clear();
  binding.animated.clear();
  binding.targets.clear();
  binding.targetsOf = nullptr;
  binding.complete = true;
  binding.stale = false;
  binding.framesSinceBind = 0;

  // Everything below it, down to another animator (whose entities are its).
  const hierarchy::EntityTree tree(registry->entities());
  const std::function<void(const Entity&, int)> add =
      [&](const Entity& entity, int parent)
  {
    const int index = static_cast<int>(binding.nodes.size());
    binding.nodes.push_back(entity);
    binding.parents.push_back(parent);
    if (entity.hasComponent<NameComponent>())
      binding.byName.try_emplace(
          entity.getComponent<NameComponent>().name, index);
    for (const Entity& child : tree.children(entity))
      if (!child.hasComponent<AnimatorComponent>())
        add(child, index);
  };
  add(binding.animator, -1);

  for (std::size_t i = 0; i < binding.nodes.size(); ++i)
  {
    const Entity& node = binding.nodes[i];
    // Kept only while playing: while editing, nothing poses the joints, so
    // they're at rest as they are (and may have been moved by hand).
    const auto kept = m_live ? rest.find(node.getId()) : rest.end();
    binding.rest.push_back(kept != rest.end() &&
                                   kept->second.first == node.getGeneration()
                               ? kept->second.second
                               : poseOf(node));
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
  const AnimationSet* set = animator.m_clips.get();
  const auto durationOf = [set](const std::string& name)
  {
    const AnimationClip* clip = set ? set->find(name) : nullptr;
    return clip ? clip->duration : 0.0f;
  };
  AnimationPlayback& playback = animator.m_playback;

  // A clip changed (by play(), or in the Inspector), or played again.
  const AnimationPlayback::Layer* current = playback.current();
  const bool changed = !current || current->clip != animator.clip;
  if (animator.clip.empty())
    playback.clear();
  else if (changed || animator.m_restart)
  {
    const float duration = durationOf(animator.clip);
    // From its start when played; as `time` says when it's set in the
    // fields (or the scene starts).
    const float start = animator.m_restart && animator.m_started
                            ? 0.0f
                            : std::clamp(animator.time, 0.0f, 1.0f) * duration;
    const float fade = !m_live || !animator.m_started   ? 0.0f
                       : animator.m_fadeRequest >= 0.0f ? animator.m_fadeRequest
                                                        : animator.fade;
    if (!changed)
      playback.clear(); // the same clip from its start
    playback.play(animator.clip, fade, start);
    animator.m_timeWritten = -1.0f;
  }
  animator.m_restart = false;
  animator.m_fadeRequest = -1.0f;

  // `time` set from outside (the Inspector, a script): seek to it.
  if (AnimationPlayback::Layer* layer = playback.current();
      layer && animator.m_timeWritten >= 0.0f &&
      animator.time != animator.m_timeWritten)
    layer->time =
        std::clamp(animator.time, 0.0f, 1.0f) * durationOf(layer->clip);

  if (running && animator.playing)
    playback.advance(seconds * animator.speed, animator.loop, durationOf);

  animator.m_finished = false;
  if (const AnimationPlayback::Layer* layer = playback.current())
  {
    const float duration = durationOf(layer->clip);
    animator.time = duration > 0.0f ? layer->time / duration : 0.0f;
    animator.m_finished = !animator.loop && duration > 0.0f &&
                          (animator.speed >= 0.0f ? layer->time >= duration
                                                  : layer->time <= 0.0f);
  }
  animator.m_timeWritten = animator.time;
  animator.m_started = true;
  (void)binding;
}

void AnimationSystem::pose(Binding& binding, AnimatorComponent& animator)
{
  const AnimationSet* set = animator.m_clips.get();
  const std::size_t count = binding.nodes.size();
  binding.pose.resize(count);
  // What its clips move starts from rest (so a channel one clip has and
  // another doesn't goes back); the rest as they are now.
  if (set)
    useSet(binding, *set);
  for (std::size_t i = 0; i < count; ++i)
    binding.pose[i] =
        binding.animated[i] ? binding.rest[i] : poseOf(binding.nodes[i]);

  const auto& layers = animator.m_playback.layers();
  if (set && !layers.empty())
  {
    if (layers.size() == 1)
    {
      if (const AnimationClip* clip = set->find(layers.front().clip))
        applyClip(*clip,
                  layers.front().time,
                  targetsFor(binding, *set, *clip),
                  binding.pose);
    }
    else
    {
      // Each layer posed from rest, then mixed by weight: only the nodes
      // they move.
      std::vector<std::vector<JointPose>> posed;
      std::vector<float> weights;
      for (const AnimationPlayback::Layer& layer : layers)
      {
        binding.layerPose = binding.pose;
        if (const AnimationClip* clip = set->find(layer.clip))
          applyClip(*clip,
                    layer.time,
                    targetsFor(binding, *set, *clip),
                    binding.layerPose);
        posed.push_back(binding.layerPose);
        weights.push_back(layer.weight);
      }
      std::vector<JointPose> mix(layers.size());
      for (std::size_t i = 0; i < count; ++i)
      {
        if (!binding.animated[i])
          continue;
        for (std::size_t l = 0; l < posed.size(); ++l)
          mix[l] = posed[l][i];
        binding.pose[i] = blendPoses(mix, weights);
      }
    }
  }

  // Each node relative to the animator, down the tree.
  binding.models.resize(count);
  for (std::size_t i = 0; i < count; ++i)
  {
    const int parent = binding.parents[i];
    binding.models[i] =
        parent < 0 ? glm::mat4(1.0f)
                   : binding.models[static_cast<std::size_t>(parent)] *
                         (binding.nodes[i].hasComponent<TransformComponent>()
                              ? binding.pose[i].matrix()
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
        transform.position = binding.pose[i].translation;
        transform.rotation = binding.pose[i].rotation;
        transform.scale = binding.pose[i].scale;
      }

  // Each skinned mesh's palette and bounds.
  for (const SkinBinding& skin : binding.skins)
  {
    const MeshSkin* meshSkin = m_meshes.skin(skin.base);
    if (!meshSkin || meshSkin->joints.size() != skin.joints.size())
      continue;
    binding.jointModels.resize(skin.joints.size());
    for (std::size_t j = 0; j < skin.joints.size(); ++j)
      binding.jointModels[j] =
          skin.joints[j] >= 0
              ? binding.models[static_cast<std::size_t>(skin.joints[j])]
              : binding.models[skin.node];
    skinPalette(*meshSkin,
                binding.jointModels,
                binding.models[skin.node],
                binding.palette);
    m_meshes.setSkinPose(skin.base,
                         skin.mesh.getId(),
                         binding.palette,
                         skinnedBounds(*meshSkin, binding.palette));
    ++m_stats.skins;
  }
}

void AnimationSystem::update(double deltaTime)
{
  SFS_CPU_SCOPE("update/animation");
  ++m_frame;
  m_stats = {};
  const float seconds = static_cast<float>(deltaTime) * m_timeScale;

  // Animators gone (or no longer animators) are forgotten.
  for (auto it = m_bindings.begin(); it != m_bindings.end();)
    if (!it->second.animator.isAlive() ||
        !it->second.animator.hasComponent<AnimatorComponent>())
    {
      m_previews.erase(it->first);
      it = m_bindings.erase(it);
    }
    else
      ++it;

  for (const Entity& entity : getEntities())
  {
    if (!entity.isAlive())
      continue;
    AnimatorComponent& animator = entity.getComponent<AnimatorComponent>();
    Binding& binding = m_bindings[entity.getId()];
    if (binding.animator != entity)
    {
      binding = Binding{};
      binding.animator = entity;
    }

    // What's below it, found again when it's changed.
    ++binding.framesSinceBind;
    bool stale = binding.stale || animator.source != animator.m_sourceSeen;
    for (std::size_t i = 0; !stale && i < binding.nodes.size(); ++i)
      stale = !binding.nodes[i].isAlive();
    if (!stale && binding.framesSinceBind >= kRebindFrames &&
        (!m_live || !binding.complete))
      stale = true;
    if (stale)
    {
      bind(binding, animator);
      bool waiting = false;
      animator.m_clips = clipsFor(binding, animator, waiting);
      animator.m_sourceSeen = waiting ? std::string() : animator.source;
      if (waiting)
        binding.complete = false; // look again soon
    }
    if (!animator.m_clips)
    {
      // Nothing to play (yet): its meshes as bound.
      for (const SkinBinding& skin : binding.skins)
        m_meshes.keepSkin(skin.base, skin.mesh.getId());
      continue;
    }
    ++m_stats.animators;

    const bool running = m_live || m_previews.contains(entity.getId());
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
