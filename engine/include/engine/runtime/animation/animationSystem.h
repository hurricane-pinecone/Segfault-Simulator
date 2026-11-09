#pragma once

#include "engine/core/animation/animation.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfs
{

class AssetStore;
class MeshLibrary;
struct AnimatorComponent;

/**
 * Plays every AnimatorComponent's clips (see animatorComponent.h), each
 * update: moves its time on (by the update's seconds, so a game that stops
 * updating, or pauses its time, pauses its animations), samples its clips,
 * cross-fading between them, and poses what's below it:
 *
 * - its joints and other animated entities, by name: their
 *   TransformComponents are set while the game plays (so what's parented to
 *   a joint, a held tool, moves with it). While editing (setLive(false))
 *   they're left as they are, the pose only shown;
 * - its skinned meshes: each one's joint palette (inverse(mesh) * joint *
 *   inverse bind, all relative to the animator) and posed bounds go to the
 *   scene's MeshLibrary (MeshLibrary::setSkinPose), which has the renderer
 *   deform that entity's copy of the mesh on the GPU, and which culling,
 *   picking and deformers read the pose from.
 *
 * An animator none of whose skinned meshes was drawn since its last pose
 * (off screen, out of every shadow) is posed only every few updates; its
 * time still runs.
 *
 * StandardScene adds one. What's below an animator is found again when an
 * entity it moved is destroyed, while its meshes are still loading, and on
 * rebind().
 */
class AnimationSystem : public System
{
public:
  AnimationSystem(MeshLibrary& meshes, AssetStore& assets);
  ~AnimationSystem() override;

  /**
   * Whether the scene is playing (the default): animators' times run and
   * their joints' transforms are set. Off (the editor, editing): each shows
   * its pose at its `time` without moving its joints, its time running only
   * while it's previewed (setPreview()).
   */
  void setLive(bool live) { m_live = live; }
  bool live() const { return m_live; }

  /** While editing: play an animator's clip (its time runs), or stop. */
  void setPreview(const Entity& animator, bool preview);
  bool previewing(const Entity& animator) const;

  /** Every animator's speed is multiplied by this (1: as they are). */
  void setTimeScale(float scale) { m_timeScale = scale; }
  float timeScale() const { return m_timeScale; }

  /**
   * Find what's below an animator again on the next update (none: every
   * animator), e.g. after parenting a new skinned mesh under it.
   */
  void rebind(const Entity& animator = {});

  /** Last update's work, for a profiler. */
  struct Stats
  {
    std::size_t animators = 0; // with clips to play
    std::size_t posed = 0;     // posed this update
    std::size_t skins = 0;     // skinned meshes posed this update
  };
  const Stats& stats() const { return m_stats; }

protected:
  void update(double deltaTime) override;

private:
  // An animator's entities: those below it (down to another animator),
  // parents first, the animator itself first of all.
  struct SkinBinding
  {
    Entity mesh;
    std::size_t node = 0;    // its place in nodes
    MeshHandle base;         // its MeshRendererComponent's mesh
    std::vector<int> joints; // each skin joint's node (-1: none found)
  };
  struct Binding
  {
    Entity animator;
    std::vector<Entity> nodes;
    std::vector<int> parents; // -1 for the animator
    std::unordered_map<std::string, int> byName;
    std::vector<SkinBinding> skins;
    bool complete = true; // false while a mesh below it is still loading
    // Each node's rest pose (as it was first found) and whether its clips
    // move it at all; each clip's channels' nodes (-1: none).
    std::vector<JointPose> rest;
    std::vector<char> animated;
    const AnimationSet* targetsOf = nullptr;
    std::unordered_map<const AnimationClip*, std::vector<int>> targets;
    uint32_t framesSinceBind = 0;
    bool stale = true;
    // Scratch, kept to reuse.
    std::vector<JointPose> pose;
    std::vector<JointPose> layerPose;
    std::vector<glm::mat4> models;
    std::vector<glm::mat4> jointModels;
    std::vector<glm::mat4> palette;
  };

  void bind(Binding& binding, AnimatorComponent& animator);
  std::shared_ptr<const AnimationSet>
  clipsFor(const Binding& binding,
           const AnimatorComponent& animator,
           bool& waiting) const;
  // Note which nodes a set's clips move (once per set).
  void useSet(Binding& binding, const AnimationSet& set);
  const std::vector<int>& targetsFor(Binding& binding,
                                     const AnimationSet& set,
                                     const AnimationClip& clip);
  void advance(Binding& binding,
               AnimatorComponent& animator,
               float seconds,
               bool running);
  void pose(Binding& binding, AnimatorComponent& animator);

  MeshLibrary& m_meshes;
  AssetStore& m_assets;
  bool m_live = true;
  float m_timeScale = 1.0f;
  uint64_t m_frame = 0;
  std::unordered_map<Entity::EntityId, Binding> m_bindings;
  std::unordered_set<Entity::EntityId> m_previews;
  Stats m_stats;
};

} // namespace sfs
