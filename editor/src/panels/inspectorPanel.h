#pragma once

#include "panels/panel.h"

// Edits the selected entity's components, drawn from reflection: any component
// the registry knows (the engine's, or a game module's) gets an editor built
// from its field descriptors and [[sfs::...]] attributes, with no hand-written
// UI. The entity's name (NameComponent) is shown as the panel's title field.
class InspectorPanel : public Panel
{
public:
  InspectorPanel() : Panel("Inspector") {}

protected:
  void drawContents(EditorContext& context) override;
};
