#include "engine/runtime/facilities/animation.h"

#include "absentBackend.h"
#include "engine/core/animation/animation.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/animation/animationSystem.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

AnimationSystem* Animation::backend() const
{
  if (!m_animation)
    warnAbsent(s_warned, "animation", "AnimationSystem");

  return m_animation;
}

std::vector<std::string> Animation::clipNames(const Entity& animator) const
{
  const AnimationSystem* animation = backend();
  return animation ? animation->clipNames(animator)
                   : std::vector<std::string>{};
}

const AnimationSet* Animation::clips(const Entity& animator) const
{
  const AnimationSystem* animation = backend();
  return animation ? animation->clips(animator) : nullptr;
}

Animation Animation::in(Registry* registry)
{
  return Animation(registry ? registry->tryGetSystem<AnimationSystem>()
                            : nullptr);
}

float Animation::length(const Entity& animator, std::string_view clip) const
{
  const AnimationSystem* animation = backend();
  return animation ? animation->length(animator, clip) : 0.0f;
}

void Animation::play(const Entity& animator,
                     std::string_view clip,
                     float fade) const
{
  if (AnimationSystem* animation = backend())
    animation->play(animator, clip, fade);
}

void Animation::restart(const Entity& animator) const
{
  if (AnimationSystem* animation = backend())
    animation->restart(animator);
}

} // namespace sfs::facility
