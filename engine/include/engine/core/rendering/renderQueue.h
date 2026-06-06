#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace sfs
{

template <typename TItem>
class RenderQueue
{
public:
  void clear() { m_items.clear(); };

  void submit(const TItem& item);
  void submit(TItem&& item);

  template <typename TOther>
  void submitAll(const std::vector<TOther>& items);

  std::vector<TItem>& mutableItems();
  const std::vector<TItem>& items() const;

  bool empty() const { return m_items.empty(); };
  std::size_t size() const { return m_items.size(); };

private:
  std::vector<TItem> m_items;
};

template <typename TItem>
void RenderQueue<TItem>::submit(const TItem& item)
{
  m_items.push_back(item);
}

template <typename TItem>
void RenderQueue<TItem>::submit(TItem&& item)
{
  m_items.push_back(std::move(item));
}

template <typename TItem>
template <typename TOther>
void RenderQueue<TItem>::submitAll(const std::vector<TOther>& items)
{
  static_assert(std::is_constructible_v<TItem, TOther>,
                "Queue item type must be constructible subtype of TItem");
  m_items.insert(m_items.end(), items.begin(), items.end());
}

template <typename TItem>
std::vector<TItem>& RenderQueue<TItem>::mutableItems()
{
  return m_items;
}

template <typename TItem>
const std::vector<TItem>& RenderQueue<TItem>::items() const
{
  return m_items;
}

} // namespace sfs
