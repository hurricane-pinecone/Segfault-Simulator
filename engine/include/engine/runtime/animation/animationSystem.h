#pragma once

#include "engine/core/animation/animation.h"
#include "engine/core/components/animatorComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore;
class MeshLibrary;

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

  /**
   * Play a clip from its start, cross-fading from what's playing over `fade`
   * seconds (0: at once). Playing the clip that's playing just carries on
   * (restart() goes back to its start).
   */
  void play(const Entity& animator, std::string_view clip, float fade = 0.0f);
  /** Back to the start of the clip playing (and playing). */
  void restart(const Entity& animator);
  /**
   * A clip's length in seconds (the one playing if `clip` is empty); 0 if
   * there's no such clip or its model hasn't loaded yet (it's loaded the
   * first time it updates).
   */
  float length(const Entity& animator, std::string_view clip = {}) const;
  /** Every clip it can play, by name (empty until its model has loaded). */
  std::vector<std::string> clipNames(const Entity& animator) const;
  /** Its clips, once loaded (null before, or if its model has none). */
  const AnimationSet* clips(const Entity& animator) const;

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
  void create() override;
  void update(double deltaTime) override;

private:
  using SkinBinding = AnimatorSkin;
  using Binding = AnimatorComponent;

  void
  bind(const Entity& entity, Binding& binding, AnimatorComponent& animator);
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
  // An animator's (null: not one).
  Binding* bindingOf(const Entity& animator) const;
  // Scratch, kept to reuse (one animator posed at a time).
  std::vector<JointPose> m_pose;
  std::vector<JointPose> m_layerPose;
  std::vector<glm::mat4> m_models;
  std::vector<glm::mat4> m_jointModels;
  std::vector<glm::mat4> m_palette;
  AnimatorComponent* animatorOf(const Entity& animator);

  Stats m_stats;
};

} // namespace sfs
