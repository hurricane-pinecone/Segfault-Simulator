#include "panels/codePanel.h"

#include "build/buildEnvironment.h"
#include "editorContext.h"

#include "engine/core/logger/logger.h"

#include "imgui.h"
#include "imgui_internal.h" // PlatformImeData (text input without an InputText), settings handler

#include <SDL_filesystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr std::array<const char*, 3> kModeLabels = {"Auto", "Engine config", "Your config"};
constexpr float kMinFontSize = 8.0f;
constexpr float kMaxFontSize = 40.0f;

// Where the engine's Neovim config ships: next to the editor executable.
std::filesystem::path engineConfigFolder()
{
  const char* base = SDL_GetBasePath();
  return std::filesystem::path(base ? base : "") / "nvim";
}

ImU32 toImColor(uint32_t rgb)
{
  return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
}

// Keys with a name in Neovim's notation (<CR>, <Up>, ...).
struct NamedKey
{
  ImGuiKey key;
  const char* name;
};
constexpr NamedKey kNamedKeys[] = {
    {ImGuiKey_Enter, "CR"},        {ImGuiKey_KeypadEnter, "CR"}, {ImGuiKey_Escape, "Esc"},
    {ImGuiKey_Tab, "Tab"},         {ImGuiKey_Backspace, "BS"},   {ImGuiKey_Delete, "Del"},
    {ImGuiKey_Insert, "Insert"},   {ImGuiKey_UpArrow, "Up"},     {ImGuiKey_DownArrow, "Down"},
    {ImGuiKey_LeftArrow, "Left"},  {ImGuiKey_RightArrow, "Right"}, {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},         {ImGuiKey_PageUp, "PageUp"},  {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_F1, "F1"},   {ImGuiKey_F2, "F2"},   {ImGuiKey_F3, "F3"},   {ImGuiKey_F4, "F4"},
    {ImGuiKey_F5, "F5"},   {ImGuiKey_F6, "F6"},   {ImGuiKey_F7, "F7"},   {ImGuiKey_F8, "F8"},
    {ImGuiKey_F9, "F9"},   {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
};

// Keys that only need translating with Ctrl/Alt held (otherwise they arrive
// as typed text).
constexpr NamedKey kChordKeys[] = {
    {ImGuiKey_Space, "Space"},     {ImGuiKey_LeftBracket, "["}, {ImGuiKey_RightBracket, "]"},
    {ImGuiKey_Backslash, "Bslash"}, {ImGuiKey_Slash, "/"},      {ImGuiKey_Minus, "-"},
    {ImGuiKey_Equal, "="},         {ImGuiKey_Period, "."},     {ImGuiKey_Comma, ","},
    {ImGuiKey_Semicolon, ";"},     {ImGuiKey_Apostrophe, "'"}, {ImGuiKey_GraveAccent, "`"},
};

std::string modifierPrefix(bool ctrl, bool shift, bool alt)
{
  std::string prefix;
  if (ctrl)
    prefix += "C-";
  if (shift)
    prefix += "S-";
  if (alt)
    prefix += "M-";
  return prefix;
}

void appendUtf8(std::string& out, unsigned int c)
{
  if (c < 0x80)
    out += static_cast<char>(c);
  else if (c < 0x800)
  {
    out += static_cast<char>(0xc0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
  else if (c < 0x10000)
  {
    out += static_cast<char>(0xe0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
  else
  {
    out += static_cast<char>(0xf0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
}

// The panel's settings live in imgui.ini ([SfsEditor][Code]), with the rest of
// the editor's per-user UI state.
CodePanel::Settings* g_settings = nullptr;

void registerSettings(CodePanel::Settings* settings)
{
  g_settings = settings;
  ImGuiSettingsHandler handler;
  handler.TypeName = "SfsEditor";
  handler.TypeHash = ImHashStr("SfsEditor");
  handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void*
  { return std::strcmp(name, "Code") == 0 ? g_settings : nullptr; };
  handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
  {
    auto* settings = static_cast<CodePanel::Settings*>(entry);
    int mode = 0;
    float size = 0.0f;
    if (!settings)
      return;
    if (std::sscanf(line, "Config=%d", &mode) == 1 && mode >= 0 &&
        mode < static_cast<int>(kModeLabels.size()))
      settings->mode = static_cast<NvimClient::ConfigMode>(mode);
    else if (std::sscanf(line, "FontSize=%f", &size) == 1)
      settings->fontSize = std::clamp(size, kMinFontSize, kMaxFontSize);
  };
  handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
  {
    if (!g_settings)
      return;
    out->appendf("[%s][Code]\nConfig=%d\nFontSize=%.0f\n\n", h->TypeName,
                 static_cast<int>(g_settings->mode), g_settings->fontSize);
  };
  ImGui::AddSettingsHandler(&handler);
}

// The first installed Nerd Font Mono (icons in statuslines and file trees) in
// `folder`, if any.
std::optional<std::filesystem::path> findNerdFont(const std::filesystem::path& folder)
{
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error))
  {
    const std::string name = entry.path().filename().string();
    if (name.find("NerdFontMono-Regular.") != std::string::npos)
      return entry.path();
  }
  return std::nullopt;
}

// A monospace font for the code grid: $SFS_EDITOR_CODE_FONT, else a Nerd Font,
// else the platform's standard coding font. Empty if none is found (the UI
// font, also monospaced, is used then).
std::filesystem::path codeFontPath()
{
  if (const char* custom = std::getenv("SFS_EDITOR_CODE_FONT"); custom && *custom)
    return custom;

  std::vector<std::filesystem::path> nerdFolders, fallbacks;
#if defined(_WIN32)
  const char* local = std::getenv("LOCALAPPDATA");
  if (local)
    nerdFolders.push_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
  nerdFolders.push_back("C:\\Windows\\Fonts");
  fallbacks = {"C:\\Windows\\Fonts\\CascadiaMono.ttf", "C:\\Windows\\Fonts\\consola.ttf"};
#else
  if (const char* home = std::getenv("HOME"))
    nerdFolders.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
  nerdFolders.push_back("/usr/share/fonts");
  fallbacks = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
               "/usr/share/fonts/TTF/DejaVuSansMono.ttf"};
#endif
  for (const auto& folder : nerdFolders)
    if (const auto font = findNerdFont(folder))
      return *font;
  for (const auto& font : fallbacks)
    if (std::filesystem::exists(font))
      return font;
  return {};
}

ImFont* loadCodeFont()
{
  const std::filesystem::path path = codeFontPath();
  if (path.empty())
    return nullptr;
  // The first font is the UI's: keep ImGui's default there (it is otherwise
  // only added when the atlas is still empty at the first frame).
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (atlas->Fonts.empty())
    atlas->AddFontDefault();

  ImFontConfig config;
  config.OversampleH = 1; // crisp at the pixel sizes code is read at
  // Sizes are chosen when pushed (ImGui 1.92 bakes glyphs on demand).
  ImFont* font = atlas->AddFontFromFileTTF(path.string().c_str(), 0.0f, &config);
  if (font)
    LOG_INFO("Code panel font: " + path.string());
  return font;
}
} // namespace

CodePanel::CodePanel(const BuildEnvironment& environment)
    : Panel("Code"), m_environment(environment), m_nvim(environment)
{
  registerSettings(&m_settings);
  m_font = loadCodeFont();
}

CodePanel::~CodePanel()
{
  g_settings = nullptr;
}

int CodePanel::windowFlags() const
{
  // Arrow keys, Tab and Esc are Neovim's, not ImGui navigation's.
  return ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollbar |
         ImGuiWindowFlags_NoScrollWithMouse;
}

void CodePanel::openFile(const std::filesystem::path& file)
{
  open() = true;
  m_focusRequested = true;
  m_failed = false; // an explicit request is worth another try
  if (!m_nvim.running())
    m_started = false; // ...and restarts a Neovim that was quit
  m_pendingFile = file;
}

void CodePanel::update(EditorContext& context)
{
  context.keyboardCaptured = false; // set again while drawing, if focused

  if (!m_nvim.running())
    return;
  m_nvim.poll();

  const std::filesystem::path root = context.project ? context.project->root() : std::filesystem::path();
  if (root != m_projectRoot)
  {
    m_projectRoot = root;
    m_nvim.setProject(root);
  }

  if (m_pendingFile)
  {
    m_nvim.open(*m_pendingFile);
    m_pendingFile.reset();
  }
}

bool CodePanel::ensureStarted(const EditorContext& context)
{
  if (m_nvim.running())
    return true;
  if (m_failed || m_started) // a quit Neovim stays quit until Restart
    return false;

  NvimClient::LaunchOptions options;
  options.mode = m_settings.mode;
  options.engineConfig = engineConfigFolder();
  options.workingDirectory = context.project ? context.project->root() : std::filesystem::current_path();
  m_projectRoot = context.project ? context.project->root() : std::filesystem::path();

  m_started = m_nvim.start(options, m_columns, m_rows);
  m_failed = !m_started;
  return m_started;
}

void CodePanel::beforeBegin()
{
  // A new Code window joins the viewport's dock node as a tab.
  if (!m_dockChecked)
  {
    m_dockChecked = true;
    const ImGuiWindow* viewport = ImGui::FindWindowByName("Viewport");
    const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(title()));
    if (viewport && viewport->DockId && !settings)
      ImGui::SetNextWindowDockID(viewport->DockId);
  }
  if (m_focusRequested)
  {
    ImGui::SetNextWindowFocus();
    m_focusRequested = false;
  }
}

void CodePanel::drawContents(EditorContext& context)
{
  drawToolbar(context);

  if (!ensureStarted(context))
  {
    ImGui::Spacing();
    if (m_failed)
      ImGui::TextWrapped("Neovim couldn't be started. Is `nvim` installed on the build machine "
                         "(on PATH for a login shell)? See the Console for details.");
    else
      ImGui::TextUnformatted("Neovim has exited. Press Restart to start it again.");
    return;
  }

  const NvimScreen& screen = m_nvim.screen();
  ImGui::PushFont(m_font, m_settings.fontSize);
  const float cellWidth = ImGui::CalcTextSize("M").x;
  const float cellHeight = std::round(ImGui::GetTextLineHeight());
  const ImVec2 available = ImGui::GetContentRegionAvail();
  m_columns = std::max(static_cast<int>(available.x / cellWidth), 1);
  m_rows = std::max(static_cast<int>(available.y / cellHeight), 1);
  m_nvim.resize(m_columns, m_rows);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // The whole area as one item: it takes clicks (and keeps the window from
  // being dragged by them) and gives keyboard focus.
  ImGui::InvisibleButton("##nvim", ImVec2(std::max(available.x, 1.0f), std::max(available.y, 1.0f)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(origin, ImVec2(origin.x + available.x, origin.y + available.y),
                          toImColor(screen.defaultBackground()));
  drawScreen(origin, cellWidth, cellHeight);

  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (focused)
  {
    context.keyboardCaptured = true;
    // Ask the platform for text input (as an active InputText would), with the
    // IME placed at Neovim's cursor.
    ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
    ime.WantTextInput = true;
    ime.ViewportId = ImGui::GetWindowViewport()->ID;
    ime.InputPos = ImVec2(origin.x + screen.cursorColumn() * cellWidth,
                          origin.y + screen.cursorRow() * cellHeight);
    ime.InputLineHeight = cellHeight;
    handleKeyboard();
  }
  handleMouse(origin, cellWidth, cellHeight);
  ImGui::PopFont();
}

void CodePanel::drawToolbar(EditorContext& context)
{
  int mode = static_cast<int>(m_settings.mode);
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::Combo("##config", &mode, kModeLabels.data(), static_cast<int>(kModeLabels.size())))
  {
    m_settings.mode = static_cast<NvimClient::ConfigMode>(mode);
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Auto: your Neovim config if you have one, otherwise the engine's.\n"
                      "Takes effect on Restart.");
  ImGui::SameLine();
  if (ImGui::Button("Restart"))
  {
    m_nvim.stop();
    m_started = false;
    m_failed = false;
    ensureStarted(context);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", m_nvim.screen().title().c_str());
}

void CodePanel::drawScreen(const ImVec2& origin, float cellWidth, float cellHeight)
{
  const NvimScreen& screen = m_nvim.screen();
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const uint32_t defaultBackground = screen.defaultBackground();

  for (int row = 0; row < screen.rows(); ++row)
  {
    const float y = origin.y + row * cellHeight;

    // Backgrounds, merged into runs of one colour.
    for (int column = 0; column < screen.columns();)
    {
      const uint32_t background = screen.colorsOf(screen.cell(row, column)).background;
      int end = column + 1;
      while (end < screen.columns() &&
             screen.colorsOf(screen.cell(row, end)).background == background)
        ++end;
      if (background != defaultBackground)
        drawList->AddRectFilled(ImVec2(origin.x + column * cellWidth, y),
                                ImVec2(origin.x + end * cellWidth, y + cellHeight),
                                toImColor(background));
      column = end;
    }

    // Text, each character placed on its own cell: a font's advance is never
    // exactly the cell width, so whole strings would drift off the grid.
    for (int column = 0; column < screen.columns(); ++column)
    {
      const NvimScreen::Cell& cell = screen.cell(row, column);
      const NvimScreen::Highlight& hl = screen.highlight(cell.highlight);
      const bool blank = cell.text.empty() || cell.text == " "; // "": right half of a wide char
      if (blank && !hl.underline && !hl.undercurl && !hl.strikethrough)
        continue;

      const NvimScreen::Colors colors = screen.colorsOf(cell);
      const float x = origin.x + column * cellWidth;
      if (!blank)
        drawList->AddText(ImVec2(x, y), toImColor(colors.foreground),
                          cell.text.data(), cell.text.data() + cell.text.size());
      if (hl.underline || hl.undercurl)
        drawList->AddLine(ImVec2(x, y + cellHeight - 1), ImVec2(x + cellWidth, y + cellHeight - 1),
                          toImColor(hl.undercurl ? colors.special : colors.foreground));
      if (hl.strikethrough)
        drawList->AddLine(ImVec2(x, y + cellHeight * 0.5f), ImVec2(x + cellWidth, y + cellHeight * 0.5f),
                          toImColor(colors.foreground));
    }
  }

  // The cursor: solid when focused, an outline when not.
  const NvimScreen::Cell& cursorCell = screen.cell(screen.cursorRow(), screen.cursorColumn());
  const NvimScreen::Colors colors = screen.colorsOf(cursorCell);
  const ImVec2 cellMin(origin.x + screen.cursorColumn() * cellWidth,
                       origin.y + screen.cursorRow() * cellHeight);
  const ImVec2 cellMax(cellMin.x + cellWidth, cellMin.y + cellHeight);
  const float fraction = std::clamp(screen.cursorPercentage(), 1, 100) / 100.0f;

  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    drawList->AddRect(cellMin, cellMax, toImColor(colors.foreground));
  else if (screen.cursorShape() == NvimScreen::CursorShape::Vertical)
    drawList->AddRectFilled(cellMin, ImVec2(cellMin.x + std::max(cellWidth * fraction, 1.0f), cellMax.y),
                            toImColor(colors.foreground));
  else if (screen.cursorShape() == NvimScreen::CursorShape::Horizontal)
    drawList->AddRectFilled(ImVec2(cellMin.x, cellMax.y - std::max(cellHeight * fraction, 1.0f)), cellMax,
                            toImColor(colors.foreground));
  else
  {
    drawList->AddRectFilled(cellMin, cellMax, toImColor(colors.foreground));
    if (!cursorCell.text.empty())
      drawList->AddText(cellMin, toImColor(colors.background), cursorCell.text.c_str());
  }
}

void CodePanel::handleKeyboard()
{
  const ImGuiIO& io = ImGui::GetIO();
  const bool ctrl = io.KeyCtrl;
  const bool alt = io.KeyAlt;
  const bool shift = io.KeyShift;
  // Exactly one of Ctrl/Alt makes a chord (Ctrl+Alt is AltGr on Windows, which
  // types characters).
  const bool chord = ctrl != alt;

  std::string keys;
  for (const NamedKey& named : kNamedKeys)
    if (ImGui::IsKeyPressed(named.key, true))
      keys += "<" + modifierPrefix(ctrl, shift, alt) + named.name + ">";

  if (chord)
  {
    for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key)
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), true))
        keys += "<" + modifierPrefix(ctrl, shift && alt, alt) +
                static_cast<char>('a' + (key - ImGuiKey_A)) + ">";
    for (int key = ImGuiKey_0; key <= ImGuiKey_9; ++key)
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), true))
        keys += "<" + modifierPrefix(ctrl, false, alt) +
                static_cast<char>('0' + (key - ImGuiKey_0)) + ">";
    for (const NamedKey& named : kChordKeys)
      if (ImGui::IsKeyPressed(named.key, true))
        keys += "<" + modifierPrefix(ctrl, false, alt) + named.name + ">";
  }
  else
  {
    for (const ImWchar c : io.InputQueueCharacters)
    {
      if (c == '<')
        keys += "<lt>";
      else if (c >= 0x20 && c != 0x7f)
        appendUtf8(keys, c);
    }
  }
  m_nvim.input(keys);
}

void CodePanel::handleMouse(const ImVec2& origin, float cellWidth, float cellHeight)
{
  const ImGuiIO& io = ImGui::GetIO();
  const NvimScreen& screen = m_nvim.screen();
  const int column = std::clamp(static_cast<int>((io.MousePos.x - origin.x) / cellWidth), 0,
                                std::max(screen.columns() - 1, 0));
  const int row = std::clamp(static_cast<int>((io.MousePos.y - origin.y) / cellHeight), 0,
                             std::max(screen.rows() - 1, 0));
  std::string modifiers;
  if (io.KeyCtrl)
    modifiers += "C";
  if (io.KeyShift)
    modifiers += "S";
  if (io.KeyAlt)
    modifiers += "A";

  constexpr std::array<const char*, 3> kButtons = {"left", "right", "middle"};

  if (ImGui::IsItemHovered())
  {
    if (io.MouseWheel != 0.0f && io.KeyCtrl) // zoom
    {
      m_settings.fontSize = std::clamp(m_settings.fontSize + (io.MouseWheel > 0.0f ? 1.0f : -1.0f),
                                       kMinFontSize, kMaxFontSize);
      ImGui::MarkIniSettingsDirty();
    }
    else if (io.MouseWheel != 0.0f)
      m_nvim.mouse("wheel", io.MouseWheel > 0.0f ? "up" : "down", modifiers, row, column);
    for (int button = 0; button < 3; ++button)
      if (ImGui::IsMouseClicked(button))
      {
        m_nvim.mouse(kButtons[button], "press", modifiers, row, column);
        m_mouseButtonDown = button;
        m_mouseRow = row;
        m_mouseColumn = column;
      }
  }

  if (m_mouseButtonDown < 0)
    return;
  if (!ImGui::IsMouseDown(m_mouseButtonDown))
  {
    m_nvim.mouse(kButtons[m_mouseButtonDown], "release", modifiers, row, column);
    m_mouseButtonDown = -1;
  }
  else if (row != m_mouseRow || column != m_mouseColumn)
  {
    m_nvim.mouse(kButtons[m_mouseButtonDown], "drag", modifiers, row, column);
    m_mouseRow = row;
    m_mouseColumn = column;
  }
}
