#pragma once

#include "engine/core/animation/animation.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

class AnimationSystem;

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
 * A game drives it with play(), stop() and the rest; switching clips with a
 * fade cross-fades between them.
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
struct AnimatorComponent
{
  std::string clip;
  [[sfs::range(-3, 3)]] float speed = 1.0f;
  bool loop = true;
  bool playing = true;
  [[sfs::range(0, 1)]] float time = 0.0f;
  [[sfs::range(0, 2)]] float fade = 0.2f;
  std::string source;

  /**
   * Play a clip from its start, cross-fading from what's playing over `fade`
   * seconds (0: at once). Playing the clip that's playing just carries on
   * (use restart() to go back to its start).
   */
  void play(std::string_view name, float fadeSeconds = 0.0f)
  {
    if (name == clip && m_started)
    {
      playing = true;
      return;
    }
    clip = std::string(name);
    playing = true;
    m_fadeRequest = fadeSeconds;
    m_restart = true;
  }

  /** Back to the start of the clip playing (and playing). */
  void restart()
  {
    playing = true;
    time = 0.0f;
    m_restart = true;
    m_fadeRequest = 0.0f;
  }

  /** Hold the pose where it is; resume() carries on from there. */
  void stop() { playing = false; }
  void resume() { playing = true; }
  bool isPlaying() const { return playing && !clip.empty() && !finished(); }

  void setSpeed(float value) { speed = value; }

  /** Where it is in the clip playing, 0 at its start to 1 at its end. */
  float normalizedTime() const { return time; }
  void setNormalizedTime(float value) { time = std::clamp(value, 0.0f, 1.0f); }

  /** The clip playing (the newest, while cross-fading). */
  const std::string& current() const { return clip; }

  /**
   * A clip's length in seconds (the one playing if `name` is empty); 0 if
   * there's no such clip or its model hasn't loaded yet (the system loads
   * it the first time it updates).
   */
  float length(std::string_view name = {}) const
  {
    const AnimationClip* found =
        m_clips ? m_clips->find(name.empty() ? std::string_view(clip) : name)
                : nullptr;
    return found ? found->duration : 0.0f;
  }

  /** Every clip it can play, by name (empty until its model has loaded). */
  std::vector<std::string> clipNames() const
  {
    std::vector<std::string> names;
    if (m_clips)
      for (const AnimationClip& candidate : m_clips->clips)
        names.push_back(candidate.name);
    return names;
  }

  /** Whether a clip that doesn't loop has reached its end. */
  bool finished() const { return !loop && m_finished; }

  /** Whether a cross-fade is under way. */
  bool fading() const { return m_playback.fading(); }

  /** Its clips, once loaded (null before, or if its model has none). */
  const std::shared_ptr<const AnimationSet>& clips() const { return m_clips; }

private:
  // As it's playing (not saved): the clips' layers, what the fields were when
  // the system last looked (to tell a change from its own), and its clips.
  AnimationPlayback m_playback;
  std::shared_ptr<const AnimationSet> m_clips;
  std::string m_sourceSeen;
  float m_fadeRequest = -1.0f; // play()'s fade; -1: the `fade` field's
  float m_timeWritten = -1.0f; // `time` as the system last set it
  bool m_restart = false;
  bool m_started = false;
  bool m_finished = false;

  friend class AnimationSystem;
};

} // namespace sfs
