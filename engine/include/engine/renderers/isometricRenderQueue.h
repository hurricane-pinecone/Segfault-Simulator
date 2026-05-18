#pragma once

#include "engine/renderers/isometricRenderItem.h"
#include <vector>

namespace sfs
{

class IsometricRenderQueue
{
public:
  void clear();

  void submit(const IsometricRenderItem& item);
  void submit(IsometricRenderItem&& item);
  void submitAll(const std::vector<IsometricRenderItem>& items);

  std::vector<IsometricRenderItem>& mutableItems();
  const std::vector<IsometricRenderItem>& items() const;

  bool empty() const;
  std::size_t size() const;

private:
  std::vector<IsometricRenderItem> m_items;
};

} // namespace sfs
