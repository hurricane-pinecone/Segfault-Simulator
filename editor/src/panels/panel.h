#pragma once

#include <string>
#include <utility>

struct EditorContext;

// A dockable editor window. Each panel draws itself inside its own ImGui window
// (and so can be docked, tabbed, or dragged out into an OS window); the editor
// owns the panels and toggles them from the Window menu.
class Panel
{
public:
  virtual ~Panel() = default;

  /** Draw the panel if open. */
  void draw(EditorContext& context);

  bool& open() { return m_open; }

  /** Window title; also the ImGui window id, so it must be unique. */
  const char* title() const { return m_title.c_str(); }

  /**
   * Per-frame work that must happen before the scene renders (e.g. resizing a
   * render target the scene draws into). Called every frame, open or not.
   */
  virtual void update(EditorContext&) {}

protected:
  explicit Panel(std::string title) : m_title(std::move(title)) {}

  /** Called just before the panel's window begins (e.g. SetNextWindow*). */
  virtual void beforeBegin() {}

  /** The panel's contents, between ImGui::Begin and End. */
  virtual void drawContents(EditorContext& context) = 0;

  /** Window flags for this panel (default: none). */
  virtual int windowFlags() const { return 0; }

private:
  std::string m_title;
  bool m_open = true;
};
