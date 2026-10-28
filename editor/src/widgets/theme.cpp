#include "widgets/theme.h"

#include "engine/core/logger/logger.h"

#include <filesystem>
#include <string>

namespace theme
{

namespace
{
ImVec4 rgb(int r, int g, int b, float a = 1.0f)
{
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
} // namespace

void apply()
{
  ImGuiStyle& style = ImGui::GetStyle();

  // Shape: soft corners on everything inside a window; OS windows (detached
  // panels) stay square.
  style.WindowRounding = 0.0f;
  style.ChildRounding = 6.0f;
  style.FrameRounding = 5.0f;
  style.PopupRounding = 6.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 5.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowPadding = ImVec2(10.0f, 10.0f);
  style.FramePadding = ImVec2(8.0f, 4.0f);
  style.ItemSpacing = ImVec2(8.0f, 6.0f);
  style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
  style.CellPadding = ImVec2(6.0f, 3.0f);
  style.IndentSpacing = 16.0f;
  style.ScrollbarSize = 12.0f;
  style.GrabMinSize = 10.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.TabBorderSize = 0.0f;
  style.TabBarOverlineSize = 2.0f;
  style.SeparatorTextBorderSize = 1.0f;
  style.WindowMenuButtonPosition = ImGuiDir_None;

  // Colours: warm charcoal greys, a hint of green in them, and sage for
  // anything selected, active or in focus.
  const ImVec4 base = rgb(46, 49, 48);      // windows
  const ImVec4 raised = rgb(53, 56, 55);    // child panes, popups
  const ImVec4 deep = rgb(38, 40, 39);      // title bars, menu bar
  const ImVec4 frame = rgb(63, 67, 65);     // fields
  const ImVec4 frameHover = rgb(72, 77, 74);
  const ImVec4 frameActive = rgb(80, 86, 82);
  // Pastel for accents (lines, marks); deeper shades where text sits on it.
  const ImVec4 sage = rgb(170, 214, 160);
  const ImVec4 sageDim = rgb(92, 122, 88);
  const ImVec4 sageMid = rgb(108, 144, 102);
  const ImVec4 sageBright = rgb(124, 166, 116);
  const ImVec4 text = rgb(228, 230, 225);
  const ImVec4 line = rgb(74, 79, 76);

  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = rgb(140, 146, 140);
  c[ImGuiCol_WindowBg] = base;
  c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_PopupBg] = raised;
  c[ImGuiCol_Border] = rgb(32, 34, 33);
  c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_FrameBg] = frame;
  c[ImGuiCol_FrameBgHovered] = frameHover;
  c[ImGuiCol_FrameBgActive] = frameActive;
  c[ImGuiCol_TitleBg] = deep;
  c[ImGuiCol_TitleBgActive] = deep;
  c[ImGuiCol_TitleBgCollapsed] = deep;
  c[ImGuiCol_MenuBarBg] = deep;
  c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = line;
  c[ImGuiCol_ScrollbarGrabHovered] = rgb(92, 98, 94);
  c[ImGuiCol_ScrollbarGrabActive] = sageDim;
  c[ImGuiCol_CheckMark] = sage;
  c[ImGuiCol_SliderGrab] = sageMid;
  c[ImGuiCol_SliderGrabActive] = sage;
  c[ImGuiCol_Button] = rgb(68, 73, 70);
  c[ImGuiCol_ButtonHovered] = sageMid;
  c[ImGuiCol_ButtonActive] = sageBright;
  c[ImGuiCol_Header] = sageDim;
  c[ImGuiCol_HeaderHovered] = rgb(82, 106, 79);
  c[ImGuiCol_HeaderActive] = sageMid;
  c[ImGuiCol_Separator] = line;
  c[ImGuiCol_SeparatorHovered] = sageMid;
  c[ImGuiCol_SeparatorActive] = sage;
  c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_ResizeGripHovered] = sageMid;
  c[ImGuiCol_ResizeGripActive] = sage;
  c[ImGuiCol_InputTextCursor] = text;
  c[ImGuiCol_Tab] = deep;
  c[ImGuiCol_TabHovered] = sageDim;
  c[ImGuiCol_TabSelected] = base;
  c[ImGuiCol_TabSelectedOverline] = sage;
  c[ImGuiCol_TabDimmed] = deep;
  c[ImGuiCol_TabDimmedSelected] = rgb(50, 53, 52);
  c[ImGuiCol_TabDimmedSelectedOverline] = sageDim;
  c[ImGuiCol_DockingPreview] = rgb(170, 214, 160, 0.5f);
  c[ImGuiCol_DockingEmptyBg] = deep;
  c[ImGuiCol_PlotLines] = sage;
  c[ImGuiCol_PlotLinesHovered] = rgb(214, 190, 130);
  c[ImGuiCol_PlotHistogram] = sage;
  c[ImGuiCol_PlotHistogramHovered] = rgb(214, 190, 130);
  c[ImGuiCol_TableHeaderBg] = raised;
  c[ImGuiCol_TableBorderStrong] = line;
  c[ImGuiCol_TableBorderLight] = rgb(58, 62, 60);
  c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
  c[ImGuiCol_TextLink] = sage;
  c[ImGuiCol_TextSelectedBg] = rgb(170, 214, 160, 0.3f);
  c[ImGuiCol_TreeLines] = line;
  c[ImGuiCol_DragDropTarget] = sage;
  c[ImGuiCol_NavCursor] = sage;
  c[ImGuiCol_NavWindowingHighlight] = rgb(228, 230, 225, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = rgb(20, 22, 21, 0.4f);
  c[ImGuiCol_ModalWindowDimBg] = rgb(20, 22, 21, 0.5f);
}

void loadUiFont()
{
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas->Fonts.empty())
    return;

  const char* candidates[] = {
#ifdef _WIN32
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/tahoma.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
  };
  for (const char* path : candidates)
  {
    std::error_code error;
    if (!std::filesystem::exists(path, error))
      continue;
    // Sizes are chosen when used (ImGui 1.92 bakes glyphs on demand); the UI
    // is drawn at style.FontSizeBase.
    if (atlas->AddFontFromFileTTF(path, 0.0f))
    {
      ImGui::GetStyle().FontSizeBase = 16.0f;
      LOG_INFO(std::string("UI font: ") + path);
      return;
    }
  }
}

} // namespace theme
