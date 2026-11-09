#include "engine/core/animation/animation.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace sfs
{

const AnimationClip* AnimationSet::find(std::string_view name) const
{
  for (const AnimationClip& clip : clips)
    if (clip.name == name)
      return &clip;
  return nullptr;
}

std::vector<std::string> AnimationSet::animatedNodes() const
{
  std::vector<std::string> nodes;
  std::unordered_set<std::string> seen;
  for (const AnimationClip& clip : clips)
    for (const AnimationChannel& channel : clip.channels)
      if (seen.insert(channel.node).second)
        nodes.push_back(channel.node);
  return nodes;
}

namespace
{
glm::quat toQuat(const glm::vec4& v) { return glm::quat(v.w, v.x, v.y, v.z); }
glm::vec4 fromQuat(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

glm::vec4 normalisedRotation(glm::vec4 v)
{
  const float length = glm::length(v);
  return length > 1e-12f ? v / length : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
}
} // namespace

glm::vec4 sampleChannel(const AnimationChannel& channel, float time)
{
  const std::size_t keys = channel.times.size();
  const bool cubic =
      channel.interpolation == AnimationInterpolation::CubicSpline;
  const std::size_t stride = cubic ? 3 : 1;
  if (keys == 0 || channel.values.size() < keys * stride)
    return channel.path == AnimationPath::Rotation ? glm::vec4(0, 0, 0, 1)
           : channel.path == AnimationPath::Scale  ? glm::vec4(1, 1, 1, 0)
                                                   : glm::vec4(0.0f);
  const bool rotation = channel.path == AnimationPath::Rotation;
  // A key's value (the middle of a cubic key's three).
  const auto value = [&](std::size_t key)
  { return channel.values[key * stride + (cubic ? 1 : 0)]; };
  const auto finish = [&](glm::vec4 v)
  { return rotation ? normalisedRotation(v) : v; };

  if (keys == 1 || time <= channel.times.front())
    return finish(value(0));
  if (time >= channel.times.back())
    return finish(value(keys - 1));

  // The keys either side.
  const auto after =
      std::upper_bound(channel.times.begin(), channel.times.end(), time);
  const std::size_t next =
      static_cast<std::size_t>(after - channel.times.begin());
  const std::size_t previous = next - 1;
  const float t0 = channel.times[previous];
  const float t1 = channel.times[next];
  const float span = t1 - t0;
  const float t = span > 0.0f ? (time - t0) / span : 0.0f;

  switch (channel.interpolation)
  {
  case AnimationInterpolation::Step:
    return finish(value(previous));
  case AnimationInterpolation::Linear:
    if (rotation)
      return fromQuat(glm::normalize(
          glm::slerp(toQuat(value(previous)), toQuat(value(next)), t)));
    return glm::mix(value(previous), value(next), t);
  case AnimationInterpolation::CubicSpline:
  {
    // glTF's Hermite spline: p0, its out-tangent m0, p1, its in-tangent m1,
    // the tangents scaled by the keys' spacing.
    const glm::vec4 p0 = channel.values[previous * 3 + 1];
    const glm::vec4 m0 = channel.values[previous * 3 + 2] * span;
    const glm::vec4 p1 = channel.values[next * 3 + 1];
    const glm::vec4 m1 = channel.values[next * 3] * span;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return finish((2.0f * t3 - 3.0f * t2 + 1.0f) * p0 +
                  (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * p1 +
                  (t3 - t2) * m1);
  }
  }
  return finish(value(previous));
}

glm::mat4 JointPose::matrix() const
{
  glm::mat4 m = glm::mat4_cast(rotation);
  m[0] *= scale.x;
  m[1] *= scale.y;
  m[2] *= scale.z;
  m[3] = glm::vec4(translation, 1.0f);
  return m;
}

void applyClip(const AnimationClip& clip,
               float time,
               std::span<const int> targets,
               std::span<JointPose> poses)
{
  const std::size_t count = std::min(clip.channels.size(), targets.size());
  for (std::size_t i = 0; i < count; ++i)
  {
    const int target = targets[i];
    if (target < 0 || static_cast<std::size_t>(target) >= poses.size())
      continue;
    const AnimationChannel& channel = clip.channels[i];
    const glm::vec4 v = sampleChannel(channel, time);
    JointPose& pose = poses[static_cast<std::size_t>(target)];
    switch (channel.path)
    {
    case AnimationPath::Translation:
      pose.translation = glm::vec3(v);
      break;
    case AnimationPath::Rotation:
      pose.rotation = toQuat(v);
      break;
    case AnimationPath::Scale:
      pose.scale = glm::vec3(v);
      break;
    }
  }
}

JointPose blendPoses(std::span<const JointPose> poses,
                     std::span<const float> weights)
{
  if (poses.empty())
    return {};
  const std::size_t count = std::min(poses.size(), weights.size());
  float total = 0.0f;
  for (std::size_t i = 0; i < count; ++i)
    total += std::max(weights[i], 0.0f);
  if (total <= 0.0f)
    return poses.front();

  JointPose out;
  out.translation = glm::vec3(0.0f);
  out.scale = glm::vec3(0.0f);
  glm::vec4 rotation(0.0f);
  const glm::vec4 first = fromQuat(poses.front().rotation);
  for (std::size_t i = 0; i < count; ++i)
  {
    const float w = std::max(weights[i], 0.0f) / total;
    if (w <= 0.0f)
      continue;
    out.translation += poses[i].translation * w;
    out.scale += poses[i].scale * w;
    glm::vec4 q = fromQuat(poses[i].rotation);
    if (glm::dot(q, first) < 0.0f)
      q = -q; // the same turn the short way round
    rotation += q * w;
  }
  out.rotation = toQuat(normalisedRotation(rotation));
  return out;
}

float AnimationPlayback::wrapTime(float seconds, float duration, bool loop)
{
  if (duration <= 0.0f)
    return std::max(seconds, 0.0f);
  if (!loop)
    return std::clamp(seconds, 0.0f, duration);
  const float wrapped = std::fmod(seconds, duration);
  return wrapped < 0.0f ? wrapped + duration : wrapped;
}

void AnimationPlayback::play(std::string_view clip,
                             float fadeSeconds,
                             float time)
{
  if (!m_layers.empty() && m_layers.back().clip == clip)
    return;
  if (fadeSeconds <= 0.0f || m_layers.empty())
  {
    m_layers.clear();
    m_layers.push_back({std::string(clip), std::max(time, 0.0f), 1.0f});
    m_fadeRate = 0.0f;
    return;
  }
  // A clip fading out that comes back picks up where it is.
  float weight = 0.0f;
  float at = std::max(time, 0.0f);
  for (auto it = m_layers.begin(); it != m_layers.end(); ++it)
    if (it->clip == clip)
    {
      weight = it->weight;
      at = it->time;
      m_layers.erase(it);
      break;
    }
  if (m_layers.size() + 1 > kMostLayers)
    m_layers.erase(m_layers.begin(),
                   m_layers.begin() + static_cast<std::ptrdiff_t>(
                                          m_layers.size() + 1 - kMostLayers));
  m_layers.push_back({std::string(clip), at, weight});
  m_fadeRate = 1.0f / fadeSeconds;
  // The rest share what's left, in proportion.
  fade(0.0f);
}

void AnimationPlayback::clear()
{
  m_layers.clear();
  m_fadeRate = 0.0f;
}

void AnimationPlayback::fade(float seconds)
{
  if (m_layers.empty())
    return;
  Layer& newest = m_layers.back();
  newest.weight =
      m_fadeRate > 0.0f
          ? std::min(newest.weight + std::max(seconds, 0.0f) * m_fadeRate, 1.0f)
          : 1.0f;
  if (newest.weight >= 1.0f - 1e-4f)
  {
    newest.weight = 1.0f;
    m_layers.erase(m_layers.begin(), m_layers.end() - 1);
    m_fadeRate = 0.0f;
    return;
  }
  float others = 0.0f;
  for (std::size_t i = 0; i + 1 < m_layers.size(); ++i)
    others += m_layers[i].weight;
  const float left = 1.0f - newest.weight;
  for (std::size_t i = 0; i + 1 < m_layers.size(); ++i)
    m_layers[i].weight = others > 0.0f
                             ? m_layers[i].weight * left / others
                             : left / static_cast<float>(m_layers.size() - 1);
  // Those faded all the way out go (never the newest).
  for (std::size_t i = m_layers.size() - 1; i-- > 0;)
    if (m_layers[i].weight < 1e-4f)
      m_layers.erase(m_layers.begin() + static_cast<std::ptrdiff_t>(i));
}

void skinPalette(const MeshSkin& skin,
                 std::span<const glm::mat4> jointModels,
                 const glm::mat4& meshModel,
                 std::vector<glm::mat4>& out)
{
  const glm::mat4 toMesh = glm::inverse(meshModel);
  out.resize(skin.joints.size());
  for (std::size_t j = 0; j < out.size(); ++j)
  {
    const glm::mat4 joint = j < jointModels.size() ? jointModels[j] : meshModel;
    const glm::mat4 bind =
        j < skin.inverseBind.size() ? skin.inverseBind[j] : glm::mat4(1.0f);
    out[j] = toMesh * joint * bind;
  }
}

Aabb skinnedBounds(const MeshSkin& skin, std::span<const glm::mat4> palette)
{
  Aabb bounds;
  const std::size_t count = std::min(skin.jointBounds.size(), palette.size());
  for (std::size_t j = 0; j < count; ++j)
    if (!skin.jointBounds[j].empty())
      bounds.expand(skin.jointBounds[j].transformed(palette[j]));
  return bounds;
}

void skinPositions(std::span<const glm::vec3> positions,
                   std::span<const SkinWeights> weights,
                   std::span<const glm::mat4> palette,
                   std::vector<glm::vec3>& out)
{
  out.resize(positions.size());
  for (std::size_t v = 0; v < positions.size(); ++v)
  {
    if (v >= weights.size())
    {
      out[v] = positions[v];
      continue;
    }
    const SkinWeights& skin = weights[v];
    glm::vec3 sum(0.0f);
    float total = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
      const float w = skin.weights[i];
      if (w <= 0.0f || skin.joints[i] >= palette.size())
        continue;
      sum += w *
             glm::vec3(palette[skin.joints[i]] * glm::vec4(positions[v], 1.0f));
      total += w;
    }
    out[v] = total > 1e-6f ? sum / total : positions[v];
  }
}

void computeJointBounds(MeshSkin& skin,
                        std::span<const MeshVertex> vertices,
                        std::span<const SkinWeights> weights)
{
  skin.jointBounds.assign(skin.joints.size(), Aabb{});
  const std::size_t count = std::min(vertices.size(), weights.size());
  for (std::size_t v = 0; v < count; ++v)
    for (int i = 0; i < 4; ++i)
      if (weights[v].weights[i] > 0.0f &&
          weights[v].joints[i] < skin.jointBounds.size())
        skin.jointBounds[weights[v].joints[i]].expand(vertices[v].position);
}

} // namespace sfs
