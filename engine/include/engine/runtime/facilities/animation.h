#pragma once

#include "engine/core/animation/animation.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"

#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

class AnimationSystem;

namespace facility
{

/**
 * Animators' clips, as game code uses it (`animation()` on a System,
 * Service, GameObject or Scene): what clips there are, and one-off orders
 * (play a clip with its own cross-fade, restart one). The steady state is the
 * AnimatorComponent's fields (`playing`, `speed`, `time` seeks). A value
 * handle over the scene's AnimationSystem, valid for the step that got it:
 * get it where it's used and keep it in no member. In a scene without one its
 * commands do nothing and its queries give empty answers, with one warning a
 * run (available() tells).
 */
class Animation
{
public:
  /** @return whether the scene has animation behind this facade */
  inline bool available() const { return m_animation != nullptr; }

  /**
   * Every clip an animator can play, by name.
   * @param animator the entity with the AnimatorComponent
   * @return the names; empty until its model has loaded
   */
  std::vector<std::string> clipNames(const Entity& animator) const;

  /**
   * An animator's clips, valid for this step only.
   * @param animator the entity with the AnimatorComponent
   * @return its clips; null before its model has loaded, or with none
   */
  const AnimationSet* clips(const Entity& animator) const;

  /**
   * The animation of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no AnimationSystem
   */
  static Animation in(Registry* registry);

  /**
   * A clip's length.
   * @param animator the entity with the AnimatorComponent
   * @param clip the clip (empty: the one playing)
   * @return seconds; 0 for no such clip, or before its model has loaded
   */
  float length(const Entity& animator, std::string_view clip = {}) const;

  /**
   * Plays a clip from its start, cross-fading from what's playing over
   * `fade` seconds (0: at once), taken up at the animation's next update.
   * Playing the clip that's playing just carries on (restart() goes back to
   * its start).
   * @param animator the entity with the AnimatorComponent
   * @param clip the clip, by name
   * @param fade seconds of cross-fade, for this change only
   */
  void
  play(const Entity& animator, std::string_view clip, float fade = 0.0f) const;

  /**
   * Plays the clip that's playing again from its start, at once: clips still
   * fading out are dropped.
   * @param animator the entity with the AnimatorComponent
   */
  void restart(const Entity& animator) const;

private:
  inline explicit Animation(AnimationSystem* animation) : m_animation(animation)
  {
  }

  // The animation; null (and warned about once) when the scene has none.
  AnimationSystem* backend() const;

  AnimationSystem* m_animation = nullptr;
};

} // namespace facility

inline facility::Animation System::animation() const
{
  return facility::Animation::in(registry);
}

inline facility::Animation Service::animation() const
{
  return facility::Animation::in(m_registry);
}

inline facility::Animation GameObject::animation() const
{
  return m_scene ? m_scene->animation() : facility::Animation::in(nullptr);
}

inline facility::Animation Scene::animation()
{
  return facility::Animation::in(&m_registry);
}

} // namespace sfs
