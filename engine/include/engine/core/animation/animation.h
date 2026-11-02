#pragma once

// Skeletal animation's data and math, the same on every backend and testable
// without a GPU: clips (keyframed channels moving a model's nodes, as glTF
// stores them), sampling them, blending poses (a cross-fade), and the joint
// palettes a skinned mesh deforms by.

#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

/** What a channel moves: a node's translation, rotation or scale. */
enum class AnimationPath : uint8_t
{
  Translation,
  Rotation,
  Scale,
};

/** How a channel's values run between its keys (glTF's samplers). */
enum class AnimationInterpolation : uint8_t
{
  Linear,      // straight (rotations along the shortest arc: slerp)
  Step,        // each key's value until the next
  CubicSpline, // a Hermite curve through the keys, with their tangents
};

/**
 * One keyframed property of one node: its keys' times (seconds, ascending)
 * and values. A translation's or scale's value is xyz (w unused), a
 * rotation's a quaternion as x, y, z, w. A CubicSpline channel has three
 * values a key, as glTF has them: the in-tangent, the value, the out-tangent.
 */
struct AnimationChannel
{
  std::string node; // the node (entity) it moves, by name
  AnimationPath path = AnimationPath::Translation;
  AnimationInterpolation interpolation = AnimationInterpolation::Linear;
  std::vector<float> times;
  std::vector<glm::vec4> values;
};

/**
 * A named animation (a glTF animation, a Blender action): channels moving
 * some of a model's nodes over `duration` seconds.
 */
struct AnimationClip
{
  std::string name;
  float duration = 0.0f;
  std::vector<AnimationChannel> channels;
};

/** A model's clips. */
struct AnimationSet
{
  std::vector<AnimationClip> clips;

  /** The clip with this name, or null. */
  const AnimationClip* find(std::string_view name) const;

  /** Every node any of the clips moves, each once, in the order first met. */
  std::vector<std::string> animatedNodes() const;
};

/**
 * A channel's value at `time` (clamped to its keys): interpolated as it
 * says, a rotation normalised.
 */
glm::vec4 sampleChannel(const AnimationChannel& channel, float time);

/** A node's local placement, taken apart. */
struct JointPose
{
  glm::vec3 translation{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};

  /** translate * rotate * scale, as TransformComponent::matrix(). */
  glm::mat4 matrix() const;
};

/**
 * Set the poses a clip moves to where it has them at `time`: channel i moves
 * poses[targets[i]] (a target of -1, or past the poses, is skipped).
 */
void applyClip(const AnimationClip& clip,
               float time,
               std::span<const int> targets,
               std::span<JointPose> poses);

/**
 * Poses mixed by weight: translations and scales averaged, rotations too
 * (each turned to the same hemisphere as the first, then normalised: a
 * weighted nlerp, which two poses' slerp barely differs from). The weights
 * needn't sum to 1; none at all gives the first pose.
 */
JointPose blendPoses(std::span<const JointPose> poses,
                     std::span<const float> weights);

/**
 * What an animator plays: its clips by name, each with its own time, and
 * how much each counts. The newest is the one playing; the others fade out
 * as it fades in over the cross-fade's duration, so two (or a few, when one
 * play() follows another quickly) blend at once.
 */
class AnimationPlayback
{
public:
  struct Layer
  {
    std::string clip;
    float time = 0.0f;   // seconds into it
    float weight = 1.0f; // how much it counts (the layers' sum to 1)
  };

  /** The most layers kept: the oldest goes when another would be one more. */
  static constexpr std::size_t kMostLayers = 4;

  /**
   * Start a clip from `time` seconds, fading it in over `fadeSeconds` seconds
   * (0: at once, dropping the rest). Playing the clip already newest only
   * changes nothing.
   */
  void play(std::string_view clip, float fadeSeconds = 0.0f, float time = 0.0f);

  /** Forget every layer. */
  void clear();

  /**
   * Move on by `seconds` (already scaled by speed): every layer's time, and
   * the fade. A looping clip wraps around its duration; one that doesn't
   * stops at its end. `durations` gives a clip's duration by name (0 if
   * unknown: its time just runs on).
   */
  template <typename Durations>
  void advance(float seconds, bool loop, Durations&& durations)
  {
    for (Layer& layer : m_layers)
      layer.time = wrapTime(layer.time + seconds, durations(layer.clip), loop);
    fade(seconds);
  }

  /** Just the fade (the layers' weights), by `seconds`. */
  void fade(float seconds);

  const std::vector<Layer>& layers() const { return m_layers; }
  std::vector<Layer>& layers() { return m_layers; }

  /** The newest layer (the clip playing), or null. */
  const Layer* current() const
  {
    return m_layers.empty() ? nullptr : &m_layers.back();
  }
  Layer* current() { return m_layers.empty() ? nullptr : &m_layers.back(); }

  /** Whether a cross-fade is under way (more than one layer counts). */
  bool fading() const { return m_layers.size() > 1; }

  /**
   * A clip's time, `seconds` on: wrapped into its duration when looping,
   * else held at its ends.
   */
  static float wrapTime(float seconds, float duration, bool loop);

private:
  std::vector<Layer> m_layers; // oldest first
  float m_fadeRate = 0.0f;     // how fast the newest fades in (1 / seconds)
};

/**
 * A skinned mesh's palette: for each of its joints, inverse(mesh) * joint *
 * inverseBind, `jointModels` and `meshModel` being placed in the same space
 * (any: relative to the model's root, say). A vertex the joint moves on its
 * own lands at palette * its position, in the mesh's own space.
 */
void skinPalette(const MeshSkin& skin,
                 std::span<const glm::mat4> jointModels,
                 const glm::mat4& meshModel,
                 std::vector<glm::mat4>& out);

/**
 * Bounds around a mesh posed by a palette, in its own space, from each
 * joint's (MeshSkin::jointBounds): never smaller than what it draws (a vertex
 * is a weighted mix of where its joints would put it, each of those within
 * its joint's box moved by its palette matrix).
 */
Aabb skinnedBounds(const MeshSkin& skin, std::span<const glm::mat4> palette);

/**
 * Where a palette moves positions (as the GPU does): each a weighted sum of
 * its joints' matrices times it. A vertex without weights stays put.
 */
void skinPositions(std::span<const glm::vec3> positions,
                   std::span<const SkinWeights> weights,
                   std::span<const glm::mat4> palette,
                   std::vector<glm::vec3>& out);

/**
 * Fill a skin's jointBounds from its mesh: each joint's box around the
 * vertices it moves at all (in the mesh's own space).
 */
void computeJointBounds(MeshSkin& skin,
                        std::span<const MeshVertex> vertices,
                        std::span<const SkinWeights> weights);

} // namespace sfs
