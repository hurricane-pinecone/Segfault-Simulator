#pragma once

#include "engine/core/animation/animation.h"

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/mesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

/**
 * Plays a model's animation clips (a glTF's animations: Blender's actions)
 * on the entities below it: put it on a skinned model's root (or any entity
 * above both its joints and its skinned meshes). Its clips' channels move
 * the entities named as their nodes (the model's joints, which a held tool
 * can be parented to), and each skinned mesh below it deforms with them, on
 * the GPU (see AnimationSystem).
 *
 * Its fields are what it starts with when the scene plays, and what the
 * editor shows (posed at `time` while editing, without moving the joints).
 * A game drives it through AnimationSystem (play(), restart(), length())
 * and its fields (`playing` false holds it, `speed`, `time` seeks);
 * switching clips with a fade cross-fades between them.
 *
 * @param std::string clip - the clip it plays, by name (empty: none, its
 *        model as bound)
 * @param float speed - how fast it plays (1 as made, 0 held, negative
 *        backwards)
 * @param bool loop - whether the clip starts again at its end (else it
 *        stops there, holding its last pose)
 * @param bool playing - whether its time runs (off: held where it is)
 * @param float time - where it is in the clip, 0 at its start to 1 at its
 *        end; the system keeps it up to date as it plays, and setting it
 *        seeks
 * @param float fade - how long switching to another clip (by changing
 *        `clip`) cross-fades, in seconds; play() takes its own
 * @param std::string source - the model asset ("asset:<id>") whose clips it
 *        plays; empty: its skinned meshes' own model's
 */
// A skinned mesh below an animator, as AnimationSystem's bound it
// (AnimatorComponent::skins).
struct AnimatorSkin
{
  Entity mesh;
  std::size_t node = 0;    // its place in the animator's nodes
  MeshHandle base;         // its MeshRendererComponent's mesh
  std::vector<int> joints; // each skin joint's node (-1: none found)
};

struct AnimatorComponent
{
  std::string clip;
  [[sfs::range(-3, 3)]] float speed = 1.0f;
  bool loop = true;
  bool playing = true;
  [[sfs::range(0, 1)]] float time = 0.0f;
  [[sfs::range(0, 2)]] float fade = 0.2f;
  std::string source;

  // As it's playing (not saved; AnimationSystem's): its clips' layers; a
  // play() or restart() not yet taken up (and play()'s fade, -1: the `fade`
  // field's); whether it has started, and whether a clip that doesn't loop
  // has reached its end.
  [[sfs::hide]] AnimationPlayback playback;
  [[sfs::hide]] float fadeRequest = -1.0f;
  [[sfs::hide]] bool restart = false;
  [[sfs::hide]] bool started = false;
  [[sfs::hide]] bool finished = false;
  // As AnimationSystem's bound it: its clips once loaded and the `source`
  // they were loaded for; its `time` as last written (to tell a seek from
  // outside); previewed (editing). Its entities: those below it (down to
  // another animator), parents first, itself first of all; their names'
  // places; its skinned meshes; whether all were found (false while a mesh
  // is still loading); whether its clips move each node (from its
  // RestPoseComponent); each clip's channels' nodes (-1: none); updates since
  // it was bound, and whether it's to be bound again.
  [[sfs::hide]] std::shared_ptr<const AnimationSet> clips;
  [[sfs::hide]] std::string sourceSeen;
  [[sfs::hide]] float timeWritten = -1.0f;
  [[sfs::hide]] bool preview = false;
  [[sfs::hide]] std::vector<Entity> nodes;
  [[sfs::hide]] std::vector<int> parents; // -1 for the animator
  [[sfs::hide]] std::unordered_map<std::string, int> byName;
  [[sfs::hide]] std::vector<AnimatorSkin> skins;
  [[sfs::hide]] bool complete = true;
  [[sfs::hide]] std::vector<char> animated;
  [[sfs::hide]] const AnimationSet* targetsOf = nullptr;
  [[sfs::hide]] std::unordered_map<const AnimationClip*, std::vector<int>>
      targets;
  [[sfs::hide]] uint32_t framesSinceBind = 0;
  [[sfs::hide]] bool stale = true;
};

} // namespace sfs
