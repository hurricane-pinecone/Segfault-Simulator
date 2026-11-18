#include "../../engine/tests/testHarness.h"
#include "editorContext.h"
#include "engine/core/logger/logger.h"
#include "panels/consolePanel.h"
#include "panels/panel.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: panels docked together

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
// The clipboard, kept here rather than in the system's.
std::string clipboard;

// An ImGui with no window or renderer, driven a frame at a time.
struct Gui
{
  Gui()
  {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*)
    { return clipboard.c_str(); };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text)
    { clipboard = text; };
  }
  ~Gui() { ImGui::DestroyContext(); }

  void frame(const std::function<void()>& draw)
  {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
  }
};

// A panel that edits what it's told to, and takes its own Delete key when its
// edits are its own, as the Assets panel and a material's tab do.
class TestPanel : public Panel
{
public:
  TestPanel(std::string title, Edits edits)
      : Panel(std::move(title)), m_edits(edits)
  {
  }

  Edits edits() const override { return m_edits; }

  int ownDeletes = 0;
  bool openPopup = false;

protected:
  void drawContents(EditorContext&) override
  {
    if (m_edits == Edits::Own && ImGui::Shortcut(ImGuiKey_Delete))
      ++ownDeletes;

    if (std::exchange(openPopup, false))
      ImGui::OpenPopup("menu");

    if (ImGui::BeginPopup("menu"))
      ImGui::EndPopup();
  }

private:
  Edits m_edits;
};

// The editor's panels docked as tabs of one node, and its scene's Delete: a
// global shortcut that acts only while a panel editing the scene has the
// focus, as the editor's handleShortcuts() does.
struct Editor
{
  Gui gui;
  EditorContext context;
  std::vector<std::unique_ptr<Panel>> panels;
  TestPanel* viewport = nullptr;
  TestPanel* assets = nullptr;
  TestPanel* board = nullptr;
  ConsolePanel* console = nullptr;
  ImGuiID dock = 0;
  const char* focus = nullptr;
  int sceneDeletes = 0;

  Editor()
  {
    viewport = add("Viewport", Panel::Edits::Scene);
    assets = add("Assets", Panel::Edits::Own);
    board = add("Architecture", Panel::Edits::None);
    auto consolePanel = std::make_unique<ConsolePanel>();
    console = consolePanel.get();
    panels.push_back(std::move(consolePanel));
  }

  TestPanel* add(const char* title, Panel::Edits edits)
  {
    auto panel = std::make_unique<TestPanel>(title, edits);
    TestPanel* added = panel.get();
    panels.push_back(std::move(panel));
    return added;
  }

  void frame()
  {
    gui.frame(
        [&]
        {
          if (!dock)
          {
            dock = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodePos(dock, ImVec2(0.0f, 0.0f));
            ImGui::DockBuilderSetNodeSize(dock, ImVec2(600.0f, 400.0f));

            for (const auto& panel : panels)
              ImGui::DockBuilderDockWindow(panel->title(), dock);

            ImGui::DockBuilderFinish(dock);
          }

          if (const Panel* focused = editFocusPanel(panels);
              focused && focused->edits() == Panel::Edits::Scene &&
              ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal))
            ++sceneDeletes;

          if (const char* title = std::exchange(focus, nullptr))
            ImGui::SetWindowFocus(title);

          for (const auto& panel : panels)
            panel->draw(context);
        });
  }

  void settle()
  {
    for (int i = 0; i < 3; ++i)
      frame();
  }

  void focusOn(const char* title)
  {
    focus = title;
    settle();
  }

  void pressDelete()
  {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    settle();
  }

  void press(ImGuiKeyChord chord)
  {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    const ImGuiKeyChord mods = chord & ImGuiMod_Mask_;
    hold(mods, true);
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    hold(mods, false);
    settle();
  }

  // Hold the modifier keys in `mods` down, or let them go.
  void hold(ImGuiKeyChord mods, bool down)
  {
    ImGuiIO& io = ImGui::GetIO();

    for (const ImGuiKey mod : {ImGuiMod_Ctrl, ImGuiMod_Shift})
      if (mods & mod)
        io.AddKeyEvent(mod, down);
  }

  void click(ImVec2 at, ImGuiKeyChord mods = 0)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    frame();
    hold(mods, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    hold(mods, false);

    // Far enough apart in time that the next click isn't a double-click.
    for (int i = 0; i < 30; ++i)
      frame();
  }

  void drag(ImVec2 from, ImVec2 to)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(from.x, from.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();

    for (int step = 1; step <= 8; ++step)
    {
      io.AddMousePosEvent(from.x + (to.x - from.x) * step / 8.0f,
                          from.y + (to.y - from.y) * step / 8.0f);
      frame();
    }

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    settle();
  }

  // The Console's window whose name has `part` in it (its list of lines, its
  // detail's text box).
  static const ImGuiWindow* consoleWindow(std::string_view part)
  {
    for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
      const std::string_view name = window->Name;

      if (name.starts_with("Console/") && name.find(part) != name.npos)
        return window;
    }

    return nullptr;
  }

  // The middle of the Console's `index`th row, scrolled to the top.
  static ImVec2 consoleLine(int index)
  {
    const ImGuiWindow* lines = consoleWindow("Lines");

    if (!lines)
      return {};

    return ImVec2(lines->DC.CursorStartPos.x + 40.0f,
                  lines->DC.CursorStartPos.y +
                      index * ImGui::GetTextLineHeightWithSpacing() +
                      ImGui::GetTextLineHeight() * 0.5f);
  }
};

// Collects what the logger sends, as the Console gets it.
struct LogCapture
{
  std::vector<std::string> lines;
  sfs::Logger::SinkId sink;

  LogCapture()
  {
    sink = sfs::Logger::addSink(
        [this](sfs::Logger::Level, const std::string& message)
        { lines.push_back(message); });
  }

  ~LogCapture() { sfs::Logger::removeSink(sink); }

  // The lines from `first` to `last`, as the Console copies them.
  std::string text(std::size_t first, std::size_t last) const
  {
    std::string out;

    for (std::size_t i = first; i <= last; ++i)
      out += (i > first ? "\n" : "") + lines[i];

    return out;
  }
};

// The editor with the Console in front, and five lines in it.
struct ConsoleEditor
{
  Editor editor;
  LogCapture log;

  ConsoleEditor()
  {
    LOG_INFO("Console line zero");
    LOG_WARN("Console line one");
    LOG_ERROR("Console line two");
    LOG_INFO("Console line three");
    LOG_INFO("Console line four");
    editor.settle();
    editor.focusOn("Console");
  }

  void copy()
  {
    clipboard = "(nothing copied)";
    editor.press(ImGuiMod_Ctrl | ImGuiKey_C);
  }
};

} // namespace

int main()
{
  TEST("the last focused panel should have the edit focus, and only it")
  {
    Editor editor;
    editor.settle();

    for (Panel* panel : {static_cast<Panel*>(editor.viewport),
                         static_cast<Panel*>(editor.assets),
                         static_cast<Panel*>(editor.board),
                         static_cast<Panel*>(editor.console)})
    {
      editor.focusOn(panel->title());
      CHECK(editFocusPanel(editor.panels) == panel);

      for (const auto& other : editor.panels)
        CHECK(other->editFocused() == (other.get() == panel));
    }
  }

  TEST("a popup or menu should leave the edit focus where it was")
  {
    Editor editor;
    editor.settle();
    editor.focusOn("Viewport");
    editor.viewport->openPopup = true;
    editor.settle();
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    CHECK(editFocusPanel(editor.panels) == editor.viewport);
  }

  TEST("a closed panel should lose the edit focus")
  {
    Editor editor;
    editor.settle();
    editor.focusOn("Architecture");
    CHECK(editor.board->editFocused());
    editor.board->open() = false;
    editor.settle();
    CHECK(!editor.board->editFocused());
  }

  TEST("Delete should reach the scene only from a panel that edits it")
  {
    Editor editor;
    editor.settle();
    editor.focusOn("Viewport");
    editor.pressDelete();
    CHECK(editor.sceneDeletes == 1);

    // A read-only panel takes no edits at all.
    for (const char* readOnly : {"Architecture", "Console"})
    {
      editor.focusOn(readOnly);
      editor.pressDelete();
      CHECK(editor.sceneDeletes == 1);
      CHECK(editor.assets->ownDeletes == 0);
    }

    // A panel with edits of its own takes its own Delete.
    editor.focusOn("Assets");
    editor.pressDelete();
    CHECK(editor.sceneDeletes == 1);
    CHECK(editor.assets->ownDeletes == 1);

    editor.focusOn("Viewport");
    editor.pressDelete();
    CHECK(editor.sceneDeletes == 2);
    CHECK(editor.assets->ownDeletes == 1);
  }

  TEST("Console lines should select with click, Shift+click, Ctrl+click and "
       "Ctrl+A, and Ctrl+C copy the selected ones")
  {
    ConsoleEditor console;
    Editor& editor = console.editor;
    CHECK(console.log.lines.size() == 5);

    // Nothing selected: nothing to copy.
    console.copy();
    CHECK(clipboard == "(nothing copied)");

    editor.click(Editor::consoleLine(1));
    console.copy();
    CHECK(clipboard == console.log.text(1, 1));

    editor.click(Editor::consoleLine(3), ImGuiMod_Shift);
    console.copy();
    CHECK(clipboard == console.log.text(1, 3));

    editor.click(Editor::consoleLine(2), ImGuiMod_Ctrl);
    console.copy();
    CHECK(clipboard == console.log.text(1, 1) + "\n" + console.log.text(3, 3));

    editor.press(ImGuiMod_Ctrl | ImGuiKey_A);
    console.copy();
    CHECK(clipboard == console.log.text(0, 4));

    // Escape clears the selection.
    editor.press(ImGuiKey_Escape);
    console.copy();
    CHECK(clipboard == "(nothing copied)");
  }

  TEST("dragging over Console lines should select them")
  {
    ConsoleEditor console;
    console.editor.drag(Editor::consoleLine(0), Editor::consoleLine(2));
    console.copy();
    CHECK(clipboard == console.log.text(0, 2));
  }

  TEST("a message of several lines should be a row per line, copied back as "
       "it was logged")
  {
    Editor editor;
    LogCapture log;
    LOG_INFO("Console first line\nsecond line\nthird line");
    editor.settle();
    editor.focusOn("Console");
    editor.click(Editor::consoleLine(2));
    clipboard.clear();
    editor.press(ImGuiMod_Ctrl | ImGuiKey_C);
    CHECK(clipboard == "third line");

    editor.press(ImGuiMod_Ctrl | ImGuiKey_A);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_C);
    CHECK(clipboard == log.text(0, 0));
  }

  TEST("the Console's Ctrl+A and Ctrl+C should act only while it has the "
       "focus")
  {
    ConsoleEditor console;
    Editor& editor = console.editor;
    editor.click(Editor::consoleLine(4));

    for (const char* other : {"Viewport", "Assets", "Architecture"})
    {
      editor.focusOn(other);
      console.copy();
      CHECK(clipboard == "(nothing copied)");
      editor.press(ImGuiMod_Ctrl | ImGuiKey_A);
    }

    editor.focusOn("Console");
    console.copy();
    CHECK(clipboard == console.log.text(4, 4));
    CHECK(editor.sceneDeletes == 0);
  }

  TEST("the detail's text box should take its own Ctrl+A and Ctrl+C while "
       "it's being used")
  {
    ConsoleEditor console;
    Editor& editor = console.editor;
    editor.click(Editor::consoleLine(2));
    const ImGuiWindow* detail = Editor::consoleWindow("detail");
    CHECK(detail != nullptr);

    // Its Ctrl+A selects all its text, not every line, and its Ctrl+C copies
    // what's selected in it: all but the last letter here.
    const ImVec2 inside =
        detail ? ImVec2(detail->Pos.x + 30.0f, detail->Pos.y + 10.0f)
               : ImVec2();
    editor.click(inside);
    CHECK(ImGui::GetActiveID() != 0);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_A);
    editor.press(ImGuiMod_Shift | ImGuiKey_LeftArrow);
    console.copy();
    const std::string line = console.log.text(2, 2);
    CHECK(clipboard == line.substr(0, line.size() - 1));

    // Done with it: the lines' own keys again, the selection as it was.
    editor.press(ImGuiKey_Escape);
    CHECK(ImGui::GetActiveID() == 0);
    console.copy();
    CHECK(clipboard == line);
  }

  return testing::report("panelTests");
}
