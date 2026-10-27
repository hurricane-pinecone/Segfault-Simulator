#pragma once

#include "panels/panel.h"

// Lists the scene's entities by name; click to select. Right-click empty space
// (or the + button) to create entities, right-click an entity to delete it.
class HierarchyPanel : public Panel
{
public:
  HierarchyPanel() : Panel("Hierarchy") {}

protected:
  void drawContents(EditorContext& context) override;

private:
  void drawCreateMenu(EditorContext& context);
};
