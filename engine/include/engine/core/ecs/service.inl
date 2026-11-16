#pragma once

#include "registry.h"

#include <utility>

namespace sfs
{

template <typename T>
T* Service::sceneData()
{
  return m_registry ? m_registry->template sceneData<T>() : nullptr;
}

template <typename T, typename... TArgs>
T* Service::addSceneData(TArgs&&... args)
{
  if (!m_registry)
  {
    LOG_ERROR("A service added scene data before it was added to a registry; "
              "nothing added");
    return nullptr;
  }
  return &m_registry->template addSceneData<T>(std::forward<TArgs>(args)...);
}

template <typename T>
void Service::removeSceneData()
{
  if (m_registry)
    m_registry->template removeSceneData<T>();
}

} // namespace sfs
