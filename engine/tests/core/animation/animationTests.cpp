#include "../../testHarness.h"

#include <engine/core/animation/animation.h>

#include <glm/glm/ext/matrix_transform.hpp>
#include <glm/glm/ext/scalar_constants.hpp>
#include <glm/glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

using namespace sfs;

namespace
{
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps) &&
         testing::approx(a.z, b.z, eps);
}

glm::vec4 quatValue(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

AnimationChannel channel(AnimationPath path,
                         AnimationInterpolation interpolation,
                         std::vector<float> times,
                         std::vector<glm::vec4> values,
                         std::string node = "bone")
{
  AnimationChannel out;
  out.node = std::move(node);
  out.path = path;
  out.interpolation = interpolation;
  out.times = std::move(times);
  out.values = std::move(values);
  return out;
}
} // namespace

int main()
{
  TEST("a linear channel should run straight between its keys, and hold at "
       "its ends")
  {
    const AnimationChannel move =
        channel(AnimationPath::Translation,
                AnimationInterpolation::Linear,
                {1.0f, 3.0f},
                {glm::vec4(0.0f), glm::vec4(4.0f, 2.0f, 0.0f, 0.0f)});
    CHECK(near(glm::vec3(sampleChannel(move, 2.0f)), {2.0f, 1.0f, 0.0f}));
    CHECK(near(glm::vec3(sampleChannel(move, 0.0f)), {0.0f, 0.0f, 0.0f}));
    CHECK(near(glm::vec3(sampleChannel(move, 9.0f)), {4.0f, 2.0f, 0.0f}));
  }

  TEST("a step channel should keep each key's value until the next")
  {
    const AnimationChannel step = channel(
        AnimationPath::Scale,
        AnimationInterpolation::Step,
        {0.0f, 1.0f},
        {glm::vec4(1.0f, 1.0f, 1.0f, 0.0f), glm::vec4(2.0f, 2.0f, 2.0f, 0.0f)});
    CHECK(testing::approx(sampleChannel(step, 0.99f).x, 1.0f));
    CHECK(testing::approx(sampleChannel(step, 1.0f).x, 2.0f));
  }

  TEST("a rotation should turn along the shortest arc at a constant rate "
       "(slerp)")
  {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::quat from = glm::angleAxis(0.0f, up);
    const glm::quat to = glm::angleAxis(glm::pi<float>() * 0.5f, up);
    const AnimationChannel turn = channel(AnimationPath::Rotation,
                                          AnimationInterpolation::Linear,
                                          {0.0f, 1.0f},
                                          {quatValue(from), quatValue(to)});
    // A quarter of the way: a quarter of the angle (an nlerp would be off).
    const glm::vec4 v = sampleChannel(turn, 0.25f);
    const glm::quat q(v.w, v.x, v.y, v.z);
    CHECK(testing::approx(glm::length(v), 1.0f));
    CHECK(testing::approx(glm::angle(q), glm::pi<float>() * 0.125f, 1e-3f));
    // The far side of the sphere: still the short way round.
    const AnimationChannel flipped = channel(AnimationPath::Rotation,
                                             AnimationInterpolation::Linear,
                                             {0.0f, 1.0f},
                                             {quatValue(from), -quatValue(to)});
    const glm::vec4 w = sampleChannel(flipped, 0.5f);
    const glm::quat half(w.w, w.x, w.y, w.z);
    CHECK(testing::approx(glm::angle(half) > glm::pi<float>()
                              ? glm::two_pi<float>() - glm::angle(half)
                              : glm::angle(half),
                          glm::pi<float>() * 0.25f,
                          1e-3f));
  }

  TEST("a cubic spline should pass through its keys and follow its tangents")
  {
    // Values 0 and 2 at t = 0 and 2, every tangent a slope of 1: the
    // straight line v = t.
    const glm::vec4 one(1.0f, 0.0f, 0.0f, 0.0f);
    const AnimationChannel curve =
        channel(AnimationPath::Translation,
                AnimationInterpolation::CubicSpline,
                {0.0f, 2.0f},
                {one, glm::vec4(0.0f), one, one, 2.0f * one, one});
    CHECK(testing::approx(sampleChannel(curve, 0.0f).x, 0.0f));
    CHECK(testing::approx(sampleChannel(curve, 0.5f).x, 0.5f));
    CHECK(testing::approx(sampleChannel(curve, 1.5f).x, 1.5f));
    CHECK(testing::approx(sampleChannel(curve, 2.0f).x, 2.0f));
    // Flat tangents: an ease, slower than the line near the keys.
    const AnimationChannel ease = channel(AnimationPath::Translation,
                                          AnimationInterpolation::CubicSpline,
                                          {0.0f, 2.0f},
                                          {glm::vec4(0.0f),
                                           glm::vec4(0.0f),
                                           glm::vec4(0.0f),
                                           glm::vec4(0.0f),
                                           2.0f * one,
                                           glm::vec4(0.0f)});
    CHECK(testing::approx(sampleChannel(ease, 1.0f).x, 1.0f));
    CHECK(sampleChannel(ease, 0.25f).x < 0.25f);
  }

  TEST("a clip should pose the nodes its channels target")
  {
    AnimationClip clip;
    clip.name = "walk";
    clip.duration = 1.0f;
    clip.channels.push_back(
        channel(AnimationPath::Translation,
                AnimationInterpolation::Linear,
                {0.0f, 1.0f},
                {glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, 2.0f, 0.0f)}));
    clip.channels.push_back(channel(
        AnimationPath::Scale,
        AnimationInterpolation::Linear,
        {0.0f, 1.0f},
        {glm::vec4(1.0f, 1.0f, 1.0f, 0.0f), glm::vec4(3.0f, 3.0f, 3.0f, 0.0f)},
        "other"));
    std::vector<JointPose> poses(2);
    const int targets[] = {1, -1}; // the second channel's node isn't there
    applyClip(clip, 0.5f, targets, poses);
    CHECK(near(poses[1].translation, {0.0f, 0.0f, 1.0f}));
    CHECK(near(poses[0].translation, glm::vec3(0.0f)));
    CHECK(near(poses[1].scale, glm::vec3(1.0f)));
  }

  TEST("blending poses should mix by weight, rotations the short way round")
  {
    JointPose a;
    JointPose b;
    b.translation = {2.0f, 0.0f, 0.0f};
    b.rotation = -glm::angleAxis(
        glm::pi<float>() * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f)); // as a 90 turn
    const JointPose poses[] = {a, b};
    const float half[] = {0.5f, 0.5f};
    const JointPose mixed = blendPoses(poses, half);
    CHECK(near(mixed.translation, {1.0f, 0.0f, 0.0f}));
    CHECK(
        testing::approx(glm::angle(mixed.rotation) > glm::pi<float>()
                            ? glm::two_pi<float>() - glm::angle(mixed.rotation)
                            : glm::angle(mixed.rotation),
                        glm::pi<float>() * 0.25f,
                        1e-3f));
    const float onlyB[] = {0.0f, 2.0f};
    CHECK(near(blendPoses(poses, onlyB).translation, {2.0f, 0.0f, 0.0f}));
  }

  TEST("a cross-fade should hand the weight over over its duration")
  {
    AnimationPlayback playback;
    const auto duration = [](const std::string&) { return 1.0f; };
    playback.play("idle");
    CHECK(playback.layers().size() == 1u);
    CHECK(testing::approx(playback.layers().back().weight, 1.0f));
    playback.advance(0.3f, true, duration);
    playback.play("walk", 0.5f);
    CHECK(playback.layers().size() == 2u);
    CHECK(playback.fading());
    CHECK(testing::approx(playback.layers().back().weight, 0.0f));
    CHECK(testing::approx(playback.layers().front().weight, 1.0f));
    playback.advance(0.25f, true, duration);
    CHECK(testing::approx(playback.layers().back().weight, 0.5f));
    CHECK(testing::approx(playback.layers().front().weight, 0.5f));
    // Each layer's time runs on its own (idle wrapped round past 1).
    CHECK(testing::approx(playback.layers().front().time, 0.55f));
    CHECK(testing::approx(playback.layers().back().time, 0.25f));
    playback.advance(0.25f, true, duration);
    CHECK(playback.layers().size() == 1u);
    CHECK(playback.current()->clip == "walk");
    CHECK(!playback.fading());
  }

  TEST("a fade started mid-fade should keep three clips summing to 1")
  {
    AnimationPlayback playback;
    const auto duration = [](const std::string&) { return 2.0f; };
    playback.play("a");
    playback.play("b", 1.0f);
    playback.advance(0.5f, true, duration);
    playback.play("c", 1.0f);
    playback.advance(0.5f, true, duration);
    float total = 0.0f;
    for (const AnimationPlayback::Layer& layer : playback.layers())
      total += layer.weight;
    CHECK(playback.layers().size() == 3u);
    CHECK(testing::approx(total, 1.0f));
    CHECK(testing::approx(playback.current()->weight, 0.5f));
  }

  TEST("a clip that doesn't loop should stop at its end; one that does wraps")
  {
    CHECK(
        testing::approx(AnimationPlayback::wrapTime(2.5f, 2.0f, false), 2.0f));
    CHECK(testing::approx(AnimationPlayback::wrapTime(2.5f, 2.0f, true), 0.5f));
    CHECK(
        testing::approx(AnimationPlayback::wrapTime(-0.5f, 2.0f, true), 1.5f));
  }

  TEST("a palette should move a vertex with its joint, relative to the mesh")
  {
    // One joint bound 1 up from the mesh's origin, then raised 2 more; the
    // mesh itself placed 5 along x (in the same space).
    MeshSkin skin;
    skin.joints = {"bone"};
    const glm::mat4 bound =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    skin.inverseBind = {glm::inverse(bound)};
    const glm::mat4 mesh =
        glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
    const glm::mat4 joint =
        mesh * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 3.0f));
    std::vector<glm::mat4> palette;
    skinPalette(skin, std::span<const glm::mat4>(&joint, 1), mesh, palette);
    CHECK(palette.size() == 1u);
    CHECK(near(glm::vec3(palette[0] * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)),
               {0.0f, 0.0f, 3.0f}));

    // Skinned positions: half on the raised joint, half on none (the
    // weights renormalised: all of it on the joint); and one unweighted.
    const glm::vec3 positions[] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
    SkinWeights weights[2];
    weights[0].weights = {0.5f, 0.0f, 0.0f, 0.0f};
    std::vector<glm::vec3> moved;
    skinPositions(positions, weights, palette, moved);
    CHECK(near(moved[0], {0.0f, 0.0f, 3.0f}));
    CHECK(near(moved[1], {1.0f, 0.0f, 0.0f}));
  }

  TEST("posed bounds should hold every vertex whatever the pose")
  {
    MeshSkin skin;
    skin.joints = {"a", "b"};
    skin.inverseBind = {glm::mat4(1.0f), glm::mat4(1.0f)};
    std::vector<MeshVertex> vertices(3);
    vertices[0].position = {0.0f, 0.0f, 0.0f};
    vertices[1].position = {1.0f, 0.0f, 0.0f};
    vertices[2].position = {0.0f, 1.0f, 0.0f};
    std::vector<SkinWeights> weights(3);
    weights[0].weights = {1.0f, 0.0f, 0.0f, 0.0f};
    weights[1].joints = {1, 0, 0, 0};
    weights[1].weights = {1.0f, 0.0f, 0.0f, 0.0f};
    weights[2].joints = {0, 1, 0, 0};
    weights[2].weights = {0.5f, 0.5f, 0.0f, 0.0f};
    computeJointBounds(skin, vertices, weights);
    CHECK(near(skin.jointBounds[0].max, {0.0f, 1.0f, 0.0f}));
    CHECK(near(skin.jointBounds[1].min, {0.0f, 0.0f, 0.0f}));

    const glm::mat4 palette[] = {
        glm::rotate(glm::mat4(1.0f), 1.0f, glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 4.0f))};
    std::vector<glm::vec3> positions;
    for (const MeshVertex& vertex : vertices)
      positions.push_back(vertex.position);
    std::vector<glm::vec3> moved;
    skinPositions(positions, weights, palette, moved);
    const Aabb bounds = skinnedBounds(skin, palette);
    for (const glm::vec3& p : moved)
      CHECK(glm::all(glm::greaterThanEqual(p, bounds.min - 1e-5f)) &&
            glm::all(glm::lessThanEqual(p, bounds.max + 1e-5f)));
  }

  return testing::report("animationTests");
}
