#include "../../engine/tests/testHarness.h"
#include "widgets/codeFont.h"

#include <imgui.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

// The Code panel's font, from the fonts the editor ships: merged icons and
// symbols must fit the grid's cells without changing them.

namespace
{
// An ImGui with no window, whose "renderer" takes every texture as is.
struct Gui
{
  Gui()
  {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  }
  ~Gui() { ImGui::DestroyContext(); }

  // Draws one frame; `measure` runs inside it.
  template <typename F>
  void frame(const F& measure)
  {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    measure();
    ImGui::Render();

    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
      if (texture->Status == ImTextureStatus_WantCreate ||
          texture->Status == ImTextureStatus_WantUpdates)
      {
        texture->SetTexID(static_cast<ImTextureID>(1));
        texture->SetStatus(ImTextureStatus_OK);
      }
      else if (texture->Status == ImTextureStatus_WantDestroy)
        texture->SetStatus(ImTextureStatus_Destroyed);
  }
};

struct Cell
{
  float width = 0.0f;
  float height = 0.0f;
};

Cell cellAt(ImFont* font, float size)
{
  ImGui::PushFont(font, size);
  const Cell cell{ImGui::CalcTextSize("M").x, ImGui::GetTextLineHeight()};
  ImGui::PopFont();
  return cell;
}
} // namespace

int main()
{
#if defined(_WIN32)
  _putenv_s("SFS_EDITOR_CODE_FONT", "");
#else
  unsetenv("SFS_EDITOR_CODE_FONT");
#endif

  const std::filesystem::path fonts = SFS_TEST_FONTS_DIR;
  Gui gui;
  // Departure Mono alone, for the cells the merged font must keep.
  ImFont* plain = ImGui::GetIO().Fonts->AddFontFromFileTTF(
      (fonts / "DepartureMono" / "DepartureMono-Regular.otf").string().c_str(),
      0.0f);
  const CodeFont code = loadCodeFont(fonts, {});

  TEST("the shipped pixel font should load, with the Nerd Font icons merged")
  {
    CHECK(plain != nullptr);
    CHECK(code.font != nullptr);
    CHECK(code.icons);
    // Its 11 px em is 14 px of ImGui's size (ascent + descent).
    CHECK(code.pixelSize == 14.0f);
  }

  TEST("sizes should snap to whole multiples of a pixel font's size")
  {
    CHECK(codeFontSize(code, 15.0f, 1.0f) == 14.0f);
    CHECK(codeFontSize(code, 8.0f, 1.0f) == 14.0f);
    CHECK(codeFontSize(code, 22.0f, 1.0f) == 28.0f);
    CHECK(codeFontSize(code, 15.0f, 2.0f) == 14.0f); // 28 px on screen
    // 15 x 1.5 = 22.5 px on screen: 28 px.
    CHECK(std::abs(codeFontSize(code, 15.0f, 1.5f) * 1.5f - 28.0f) < 1.0f);
    CHECK(codeFontSize(CodeFont{}, 15.0f, 2.0f) == 15.0f);
  }

  TEST("merging should keep the cells, and icons should fill exactly one")
  {
    gui.frame(
        [&]
        {
          for (const float size : {14.0f, 28.0f, 42.0f})
          {
            const Cell before = cellAt(plain, size);
            const Cell after = cellAt(code.font, size);
            CHECK(after.width == before.width);
            CHECK(after.height == before.height);
            CHECK(after.width == size / 2.0f); // 7 x 14 pixels at 1x

            ImGui::PushFont(code.font, size);
            ImFontBaked* baked = ImGui::GetFontBaked();

            // A file, a folder and a C++ icon, as mini.icons draws them.
            for (const ImWchar icon : {0xf15b, 0xe5ff, 0xe61d})
            {
              const ImFontGlyph* glyph = baked->FindGlyphNoFallback(icon);
              CHECK(glyph != nullptr);

              if (!glyph)
                continue;

              CHECK(glyph->AdvanceX == after.width);
              CHECK(glyph->X0 >= -0.5f);
              CHECK(glyph->X1 <= after.width + 0.5f);
              CHECK(glyph->Y0 >= -0.5f);
              CHECK(glyph->Y1 <= after.height + 0.5f);
            }

            // Box drawing and the ellipsis come from the pixel font itself.
            for (const ImWchar symbol :
                 {0x2502, 0x2500, 0x256d, 0x2026, 0x258f})
              CHECK(baked->FindGlyphNoFallback(symbol) != nullptr);

            ImGui::PopFont();
          }
        });
  }

  return testing::report("codeFontTests");
}
