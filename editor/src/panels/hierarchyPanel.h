#pragma once

#include "panels/panel.h"

#include <cstdint>
#include <string>

// Lists the scene's entities by name; click to select. Right-click empty space
// (or the + button) to create entities, right-click an entity to delete it.
// The filter box narrows the list by name; picking an entity elsewhere (in the
// viewport) scrolls the list to it.
class HierarchyPanel : public Panel
{
public:
  HierarchyPanel() : Panel("Hierarchy") {}

protected:
  void drawContents(EditorContext& context) override;

private:
  void drawCreateMenu(EditorContext& context);

  std::string m_filter;
  uint32_t m_shownSelection = UINT32_MAX; // the selection the list last scrolled to
};
