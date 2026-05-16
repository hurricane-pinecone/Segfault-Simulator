#include "engine/game/gameObject.h"
#include "engine/sceneManager/scene.h"

namespace sfs
{

void GameObject::destroy(Scene& scene)
{
  if (m_entity.isAlive())
  {
    scene.destroyEntity(m_entity);
    m_entity = {};
  }
}

} // namespace sfs
