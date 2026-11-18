#pragma once

#include <memory>
#include <span>
#include <string>
#include <utility>

struct EditorContext;

// A dockable editor window. Each panel draws itself inside its own ImGui window
// (and so can be docked, tabbed, or dragged out into an OS window); the editor
// owns the panels and toggles them from the Window menu.
class Panel
{
public:
  /** What the Edit menu and edit shortcuts act on from a panel. */
  enum class Edits
  {
    /** Nothing: the panel only shows things, and the Edit menu is disabled. */
    None,
    /** The scene being edited: its history and its selection. */
    Scene,
    /** The panel's own document (a material's graph, the project's files). */
    Own,
  };

  virtual ~Panel() = default;

  /** Draw the panel if open. */
  void draw(EditorContext& context);

  /**
   * Whether this panel was the last one focused (menus, popups and tooltips
   * don't count), so the Edit menu and the edit shortcuts are its. Kept as
   * it's drawn: false while it's closed.
   */
  bool editFocused() const { return m_editFocused; }

  /** What its edits act on (default: nothing, a panel that only shows). */
  virtual Edits edits() const { return Edits::None; }

  bool& open() { return m_open; }

  /**
   * Whether the default layout opens it (the editor's first run, Window >
   * Reset Layout). A panel that starts work when it opens says no: it's
   * opened from the Window menu.
   */
  virtual bool openByDefault() const { return true; }

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

  /**
   * From beforeBegin(): the first time the window is shown with no place of
   * its own saved, it opens as a tab beside the Viewport.
   */
  void dockNewBesideViewport();

  /** The panel's contents, between ImGui::Begin and End. */
  virtual void drawContents(EditorContext& context) = 0;

  /** Window flags for this panel (default: none). */
  virtual int windowFlags() const { return 0; }

private:
  bool m_dockChecked = false;
  bool m_editFocused = false;
  bool m_open = true;
  std::string m_title;
};

/**
 * The panel the Edit menu and the edit shortcuts are for: the open one last
 * focused. Edits reach the scene only through a panel whose edits() is Scene.
 *
 * @param panels the editor's panels
 * @return that panel; null if none has been focused yet
 */
Panel* editFocusPanel(std::span<const std::unique_ptr<Panel>> panels);
