#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sfs
{

class IPool
{
public:
  virtual ~IPool() = default;

  virtual void remove(size_t index) = 0;
};

/**
 * One component type's storage, by entity id. Kept in fixed-size pages that
 * never move once made, so a reference to a component stays good while other
 * entities get theirs (the pool growing), until that component is removed.
 */
template <typename T>
class Pool : public IPool
{
public:
  // Components a page: one page is allocated for each run of this many ids
  // that has any.
  static constexpr size_t kPageSize = 64;

  Pool() = default;
  virtual ~Pool() = default;

  bool isEmpty() const { return m_count == 0; }
  // One past the highest id given a component (not the number stored).
  size_t getSize() const { return m_size; }
  void resize(size_t size)
  {
    m_pages.resize((size + kPageSize - 1) / kPageSize);
    m_size = size;
  }
  void clear()
  {
    m_pages.clear();
    m_size = 0;
    m_count = 0;
  }

  bool has(size_t index) const
  {
    const Page* page = pageOf(index);
    return page && (*page)[index % kPageSize].has_value();
  }

  void remove(size_t index) override
  {
    if (Page* page = pageOf(index))
    {
      std::optional<T>& slot = (*page)[index % kPageSize];
      if (slot)
      {
        slot.reset();
        --m_count;
      }
    }
  }

  void remove(const T& obj)
  {
    for (size_t i = 0; i < m_size; i++)
    {
      if (has(i) && get(i) == obj)
      {
        remove(i);
        return;
      }
    }
  }

  template <typename... TArgs>
  void set(size_t index, TArgs&&... args)
  {
    const size_t page = index / kPageSize;
    if (page >= m_pages.size())
      m_pages.resize(page + 1);
    if (!m_pages[page])
      m_pages[page] = std::make_unique<Page>();
    std::optional<T>& slot = (*m_pages[page])[index % kPageSize];
    if (!slot)
      ++m_count;
    m_size = std::max(m_size, index + 1);
    slot.emplace(std::forward<TArgs>(args)...);
  }

  // The component at `index`, which must be there (see has()).
  T& get(size_t index)
  {
    return *(*m_pages[index / kPageSize])[index % kPageSize];
  }

  // The component at `index`, or null.
  T* find(size_t index)
  {
    Page* page = pageOf(index);
    std::optional<T>* slot = page ? &(*page)[index % kPageSize] : nullptr;
    return slot && *slot ? &**slot : nullptr;
  }

  T& operator[](size_t index) { return get(index); }

private:
  using Page = std::array<std::optional<T>, kPageSize>;

  Page* pageOf(size_t index) const
  {
    const size_t page = index / kPageSize;
    return page < m_pages.size() ? m_pages[page].get() : nullptr;
  }

  std::vector<std::unique_ptr<Page>> m_pages;
  size_t m_size = 0;
  size_t m_count = 0;
};

} // namespace sfs
