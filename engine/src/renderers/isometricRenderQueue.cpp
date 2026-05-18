
#include "engine/renderers/isometricRenderQueue.h"

namespace sfs
{

void IsometricRenderQueue::clear() { m_items.clear(); }

void IsometricRenderQueue::submit(const IsometricRenderItem& item)
{
  m_items.push_back(item);
}

void IsometricRenderQueue::submit(IsometricRenderItem&& item)
{
  m_items.push_back(std::move(item));
}

void IsometricRenderQueue::submitAll(
    const std::vector<IsometricRenderItem>& items)
{
  m_items.insert(m_items.end(), items.begin(), items.end());
}

std::vector<IsometricRenderItem>& IsometricRenderQueue::mutableItems()
{
  return m_items;
}

const std::vector<IsometricRenderItem>& IsometricRenderQueue::items() const
{
  return m_items;
}

bool IsometricRenderQueue::empty() const { return m_items.empty(); }

std::size_t IsometricRenderQueue::size() const { return m_items.size(); }

} // namespace sfs
