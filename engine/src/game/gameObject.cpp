#include "engine/game/gameObject.h"
#include "engine/sceneManager/scene.h"

namespace sfs
{

void GameObject::destroy(Scene& scene)
{
  if (m_entity.isValid())
  {
    scene.destroyEntity(m_entity.getId());
  }
}

} // namespace sfs
