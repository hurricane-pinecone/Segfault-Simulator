#pragma once

struct EditorContext;

// Something a panel has selected that isn't an entity or an asset (the
// Architecture board's box), for the Inspector to show: the panel puts it in
// EditorContext::inspected and draws its details when the Inspector asks.
class Inspectable
{
public:
  virtual ~Inspectable() = default;

  /**
   * Draw its details, inside the Inspector's window.
   *
   * @param context the editor's state (its details may open a file, say)
   */
  virtual void drawInspector(EditorContext& context) = 0;
};
