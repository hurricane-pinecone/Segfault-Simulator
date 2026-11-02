#pragma once

#include <filesystem>
#include <string>
#include <vector>

class Panel;
struct SDL_Window;

// The editor's per-user UI state, kept between sessions in imgui.ini in the
// user's settings folder (%APPDATA%\SegfaultSimulator\SfsEditor on Windows,
// ~/.local/share/SegfaultSimulator/SfsEditor on Linux): ImGui's own (where
// the panels are docked, which tabs show), the panels' settings, and here
// which panels are open and the OS window's size and place
// ([SfsLayout][Window], [SfsLayout][Panels]).
//
// SFS_EDITOR_SETTINGS_DIR names another folder (to try the editor out without
// touching your own settings).
class EditorSettings
{
public:
  /**
   * Point ImGui at the settings file and read it (before the first frame,
   * once the panels have added their settings handlers), opening the panels
   * it says and putting the still hidden `window` where it was.
   */
  void load(SDL_Window* window, std::vector<Panel*> panels);

  /** Each frame: note the window's size and place (saved when they change). */
  void update();

  /** Open every panel (Window > Reset Layout). */
  void openAllPanels();

private:
  // The window as it was last, not maximised (what it goes back to).
  struct WindowState
  {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
    bool known = false; // read from the file, or seen since
  };

  void readLine(const char* entry, const char* line);
  std::string write() const;
  void restoreWindow();

  std::string m_iniPath; // UTF-8; ImGui keeps a pointer to it
  SDL_Window* m_window = nullptr;
  std::vector<Panel*> m_panels;
  WindowState m_state;
  // Read from the file: each panel's title and whether it's open.
  std::vector<std::pair<std::string, bool>> m_panelsOpen;
};
