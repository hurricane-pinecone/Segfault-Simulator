#include "../../engine/tests/testHarness.h"

#include "widgets/widgets.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder: two panels docked together

#include <functional>
#include <string>

namespace
{
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

// A window with one big button at its top left (about 8..208, 8..108).
bool button()
{
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(400.0f, 400.0f));
  ImGui::Begin(
      "Panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
  ImGui::Button("Row", ImVec2(200.0f, 100.0f));
  const bool clicked = widgets::itemReleasedWithoutDrag();
  ImGui::End();
  return clicked;
}

// Press at `from`, move to `to`, release: whether a frame saw a click.
bool pressAndRelease(Gui& gui, ImVec2 from, ImVec2 to)
{
  ImGuiIO& io = ImGui::GetIO();
  bool clicked = false;
  const auto step = [&] { gui.frame([&] { clicked |= button(); }); };
  io.AddMousePosEvent(from.x, from.y);
  step();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  step();
  io.AddMousePosEvent(to.x, to.y);
  step();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  step();
  step();
  return clicked;
}
} // namespace

int main()
{
  TEST("a click is a release on the item it was pressed on, not a drag")
  {
    Gui gui;
    CHECK(pressAndRelease(gui, ImVec2(60.0f, 40.0f), ImVec2(61.0f, 40.0f)));
    // Dragged past the threshold (to drop it somewhere): no click, even back
    // on the item.
    CHECK(!pressAndRelease(gui, ImVec2(60.0f, 40.0f), ImVec2(120.0f, 40.0f)));
    // Pressed elsewhere, released on it: no click.
    CHECK(!pressAndRelease(gui, ImVec2(300.0f, 300.0f), ImVec2(60.0f, 40.0f)));
  }

  TEST("of two panels docked together, only the focused one owns the Edit "
       "menu, a window inside it included")
  {
    Gui gui;
    bool ownsA = false, ownsB = false;
    const char* focus = nullptr;
    bool focusChild = false;
    ImGuiID dock = 0;
    const auto draw = [&]
    {
      if (!dock)
      {
        dock = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodePos(dock, ImVec2(0.0f, 0.0f));
        ImGui::DockBuilderSetNodeSize(dock, ImVec2(400.0f, 300.0f));
        ImGui::DockBuilderDockWindow("A", dock);
        ImGui::DockBuilderDockWindow("B", dock);
        ImGui::DockBuilderFinish(dock);
      }
      if (focus)
        ImGui::SetWindowFocus(focus);
      ImGui::Begin("A");
      ImGui::BeginChild("inside", ImVec2(100.0f, 100.0f));
      if (focusChild)
        ImGui::SetWindowFocus();
      ImGui::EndChild();
      widgets::trackFocus(ownsA);
      ImGui::End();
      ImGui::Begin("B");
      widgets::trackFocus(ownsB);
      ImGui::End();
    };
    const auto settle = [&]
    {
      for (int i = 0; i < 3; ++i)
      {
        gui.frame(draw);
        focus = nullptr;
        focusChild = false;
      }
    };
    settle(); // the panels appear (the last docked one taking focus)
    focus = "A";
    settle();
    CHECK(ownsA);
    CHECK(!ownsB);
    focus = "B";
    settle();
    CHECK(ownsB);
    CHECK(!ownsA);
    focus = "A";
    settle();
    focusChild = true;
    settle();
    CHECK(ownsA);
    CHECK(!ownsB);
  }

  TEST("a commit-on-Enter text field should keep its value while it's typed "
       "in, and change it once, on Enter")
  {
    Gui gui;
    std::string name = "wet";
    int changes = 0;
    const auto field = [&]
    {
      ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
      ImGui::SetNextWindowSize(ImVec2(400.0f, 400.0f));
      ImGui::Begin("Panel",
                   nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
      ImGui::SetNextItemWidth(200.0f);
      changes += widgets::inputTextOnCommit("##name", name) ? 1 : 0;
      ImGui::End();
    };
    ImGuiIO& io = ImGui::GetIO();
    gui.frame(field);
    // Clicked at its right end (after the text), then typed in.
    io.AddMousePosEvent(190.0f, 15.0f);
    gui.frame(field);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    gui.frame(field);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    gui.frame(field);
    for (const char c : std::string("ness"))
    {
      io.AddInputCharacter(static_cast<unsigned int>(c));
      gui.frame(field);
      CHECK(name == "wet");
    }
    CHECK(changes == 0);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    gui.frame(field);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    gui.frame(field);
    CHECK(name == "wetness");
    CHECK(changes == 1);
  }

  return testing::report("widgetsTests");
}
