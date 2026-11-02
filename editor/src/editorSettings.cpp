#include "editorSettings.h"

#include "panels/panel.h"

#include "engine/core/logger/logger.h"

#include "imgui.h"
#include "imgui_internal.h" // settings handlers

#include <SDL_filesystem.h>
#include <SDL_video.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace
{
// The folder every Segfault Simulator program keeps its files under (a
// game's log too).
constexpr const char* kOrganization = "SegfaultSimulator";

// The smallest a restored window is made.
constexpr int kMinWidth = 640;
constexpr int kMinHeight = 400;

// SDL's paths are UTF-8, as ImGui's file names are.
std::filesystem::path fromUtf8(const std::string& text)
{
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string toUtf8(const std::filesystem::path& path)
{
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// The settings file, in the user's folder for the editor. The first time,
// one left where the editor used to keep it (its working directory) is
// copied there, so the layout carries over.
std::filesystem::path settingsFile()
{
  std::error_code error;
  if (const char* folder = std::getenv("SFS_EDITOR_SETTINGS_DIR");
      folder && *folder)
  {
    std::filesystem::create_directories(folder, error);
    return std::filesystem::path(folder) / "imgui.ini";
  }

  char* pref = SDL_GetPrefPath(kOrganization, "SfsEditor");
  if (!pref)
  {
    LOG_WARN(std::string("No folder for the editor's settings, keeping them "
                         "in the working directory: ") +
             SDL_GetError());
    return "imgui.ini";
  }
  const std::filesystem::path file = fromUtf8(pref) / "imgui.ini";
  SDL_free(pref);

  if (!std::filesystem::exists(file, error))
  {
    const std::filesystem::path old =
        std::filesystem::absolute("imgui.ini", error);
    if (std::filesystem::is_regular_file(old, error) &&
        std::filesystem::copy_file(old, file, error))
      LOG_INFO("Editor settings copied to " + toUtf8(file));
  }
  return file;
}

} // namespace

void EditorSettings::load(SDL_Window* window, std::vector<Panel*> panels)
{
  m_window = window;
  m_panels = std::move(panels);
  m_iniPath = toUtf8(settingsFile());
  ImGui::GetIO().IniFilename = m_iniPath.c_str();

  ImGuiSettingsHandler handler;
  handler.TypeName = "SfsLayout";
  handler.TypeHash = ImHashStr("SfsLayout");
  handler.UserData = this;
  handler.ReadOpenFn =
      [](ImGuiContext*, ImGuiSettingsHandler* h, const char* name) -> void*
  {
    auto* self = static_cast<EditorSettings*>(h->UserData);
    if (std::strcmp(name, "Window") == 0)
      return &self->m_state;
    if (std::strcmp(name, "Panels") == 0)
      return &self->m_panelsOpen;
    return nullptr;
  };
  handler.ReadLineFn =
      [](ImGuiContext*, ImGuiSettingsHandler* h, void* entry, const char* line)
  {
    auto* self = static_cast<EditorSettings*>(h->UserData);
    self->readLine(entry == &self->m_state ? "Window" : "Panels", line);
  };
  handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h)
  {
    auto* self = static_cast<EditorSettings*>(h->UserData);
    for (Panel* panel : self->m_panels)
      for (const auto& [title, open] : self->m_panelsOpen)
        if (title == panel->title())
          panel->open() = open;
  };
  handler.WriteAllFn =
      [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
  { out->append(static_cast<EditorSettings*>(h->UserData)->write().c_str()); };
  ImGui::AddSettingsHandler(&handler);

  // Now rather than at the first frame (as ImGui would), so the window is
  // where it was before it's first shown.
  ImGui::LoadIniSettingsFromDisk(m_iniPath.c_str());
  restoreWindow();
}

void EditorSettings::readLine(const char* entry, const char* line)
{
  if (std::strcmp(entry, "Window") == 0)
  {
    int a = 0;
    int b = 0;
    if (std::sscanf(line, "Pos=%d,%d", &a, &b) == 2)
    {
      m_state.x = a;
      m_state.y = b;
    }
    else if (std::sscanf(line, "Size=%d,%d", &a, &b) == 2)
    {
      m_state.width = a;
      m_state.height = b;
      m_state.known = a > 0 && b > 0;
    }
    else if (std::sscanf(line, "Maximized=%d", &a) == 1)
      m_state.maximized = a != 0;
    return;
  }
  // A panel's title (which may have spaces), and whether it's open.
  const char* equals = std::strrchr(line, '=');
  if (equals && equals != line)
    m_panelsOpen.emplace_back(
        std::string(line, equals), std::atoi(equals + 1) != 0);
}

std::string EditorSettings::write() const
{
  std::string out;
  if (m_state.known)
  {
    char window[160];
    std::snprintf(
        window,
        sizeof(window),
        "[SfsLayout][Window]\nPos=%d,%d\nSize=%d,%d\nMaximized=%d\n\n",
        m_state.x,
        m_state.y,
        m_state.width,
        m_state.height,
        m_state.maximized ? 1 : 0);
    out += window;
  }
  out += "[SfsLayout][Panels]\n";
  for (Panel* panel : m_panels)
    out +=
        std::string(panel->title()) + "=" + (panel->open() ? "1" : "0") + "\n";
  out += "\n";
  return out;
}

void EditorSettings::restoreWindow()
{
  if (!m_window || !m_state.known)
    return;

  // The display most of it was on, or the main one if none is any more (one
  // unplugged since, or the resolution changed).
  const SDL_Rect saved{m_state.x, m_state.y, m_state.width, m_state.height};
  int display = -1;
  int bestArea = 0;
  for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)
  {
    SDL_Rect bounds;
    SDL_Rect overlap;
    if (SDL_GetDisplayUsableBounds(i, &bounds) == 0 &&
        SDL_IntersectRect(&saved, &bounds, &overlap) &&
        overlap.w * overlap.h > bestArea)
    {
      display = i;
      bestArea = overlap.w * overlap.h;
    }
  }
  const bool onDisplay = display >= 0;
  SDL_Rect bounds;
  if (SDL_GetDisplayUsableBounds(onDisplay ? display : 0, &bounds) != 0)
    return;

  // All of it on the display, its title bar too (where it can be grabbed).
  int top = 0;
  SDL_GetWindowBordersSize(m_window, &top, nullptr, nullptr, nullptr);
  const int width = std::min(std::max(m_state.width, kMinWidth), bounds.w);
  const int height =
      std::min(std::max(m_state.height, kMinHeight), bounds.h - top);
  int x = onDisplay ? m_state.x : bounds.x + (bounds.w - width) / 2;
  int y =
      onDisplay ? m_state.y : bounds.y + top + (bounds.h - top - height) / 2;
  x = std::clamp(x, bounds.x, std::max(bounds.x, bounds.x + bounds.w - width));
  y = std::clamp(y,
                 bounds.y + top,
                 std::max(bounds.y + top, bounds.y + bounds.h - height));

  SDL_SetWindowSize(m_window, width, height);
  SDL_SetWindowPosition(m_window, x, y);
  m_state = {x, y, width, height, m_state.maximized, true};
  // Maximising shows the window (before its first frame, which is drawn at
  // its full size).
  if (m_state.maximized)
    SDL_MaximizeWindow(m_window);
}

void EditorSettings::update()
{
  if (!m_window)
    return;
  const Uint32 flags = SDL_GetWindowFlags(m_window);
  if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
    return;

  // Its place and size while not maximised: what it's restored to.
  WindowState now = m_state;
  now.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
  if (!now.maximized || !m_state.known)
  {
    SDL_GetWindowPosition(m_window, &now.x, &now.y);
    SDL_GetWindowSize(m_window, &now.width, &now.height);
    now.known = true;
  }
  if (now.x != m_state.x || now.y != m_state.y || now.width != m_state.width ||
      now.height != m_state.height || now.maximized != m_state.maximized ||
      now.known != m_state.known)
  {
    m_state = now;
    ImGui::MarkIniSettingsDirty();
  }
}

void EditorSettings::openAllPanels()
{
  for (Panel* panel : m_panels)
    panel->open() = true;
}
