#include <engine/core/ecs/registry.h>
#include <engine/core/ecs/system.h>
#include <engine/core/logger/logger.h>

namespace sfs
{

const QuerySignature& System::getComponentSignature() const
{
  return m_signature;
}

const QuerySignature& System::signatureOf(std::size_t index) const
{
  return index == 0 ? m_signature : m_observers[index - 1].signature;
}

void System::addObserver(const QuerySignature& signature,
                         EntityCallback added,
                         EntityCallback removed)
{
  m_observers.push_back({signature, std::move(added), std::move(removed)});
  if (registry && !m_lists.empty())
    registry->follow(*this, m_observers.size());
}

void System::tellJoined(std::size_t index, const Entity& entity)
{
  if (index == 0)
    onEntityAdded(entity);
  else if (const EntityCallback& added = m_observers[index - 1].added)
    added(entity);
}

void System::tellLeft(std::size_t index, const Entity& entity)
{
  if (index == 0)
    onEntityRemoved(entity);
  else if (const EntityCallback& removed = m_observers[index - 1].removed)
    removed(entity);
}

void System::warnLateRegister() const
{
  LOG_WARN("registerComponent() after the system was added to a registry: "
           "ignored (register components in create())");
}

void System::warnNotImplied(std::string_view termKey) const
{
  LOG_ERROR("getEntities<...>() in " + detail::readableTypeName(typeid(*this)) +
            ": its registered terms don't imply " +
            detail::readableTypeName(termKey) +
            ", so its entities may lack it; giving an empty list (register "
            "it, or query it)");
}

void System::warnSecondAny(std::string_view groupKey) const
{
  LOG_ERROR("registerComponent<" + detail::readableTypeName(groupKey) +
            ">() in " + detail::readableTypeName(typeid(*this)) +
            ": a system takes one sfs::Any<...> group; this one is ignored "
            "(list every alternative in one group)");
}

} // namespace sfs
